# Parhelia GART, AGP, and PCIe memory audit

**Scope.** This is a local-source audit of the current open DRM/Mesa driver and
the saved Matrox 2009 Linux source.  It does not claim a hardware test on an
AGP board.  In particular, it does not make the P690 test target depend on an
untested AGP path.

## Result

| Transport / storage | What the hardware receives | Current safe status | Evidence |
|---|---|---|---|
| P690 PCIe coherent DMA | 32-bit DMA address returned by the DMA API | Implemented for the kernel-owned primary ring, secondary packets, fence page, WARP storage, and fallback vertex upload | `parhelia_drv.c`, `parhelia_submit.c`, and `parhelia_warp.c` use `dma_set_mask_and_coherent(..., DMA_BIT_MASK(32))` followed by `dmam_alloc_coherent()` |
| P690 PCIe local VRAM | BAR0-relative local address, class 0 for the proven vertex stream | Implemented and hardware-verified only for bounded `0x97` vertex draws through `CREATE_VERTEX` / `DRAW_VRAM` | `parhelia_submit.c:parhelia_select_vram_vertex_stream()` accepts the known class-3 stream and changes its final stream-0 selector to 0; Mesa enables it only after the capability ioctl and GEM mmap succeed |
| PCIe GART | None in Parhelia itself | Do not expose one | The device has no GPU page-table or PCIe aperture programming in either current driver.  On a machine with an IOMMU, the DMA API supplies an IOVA transparently; that is host DMA mapping, not a Parhelia GART. |
| AGP aperture / GART | `aper_base + page_offset`, tagged class 2 in the engine address field | Vendor implementation exists; current DRM/Mesa intentionally do not expose it | The saved `mtx_mem_alloc_agp()` allocates `AGP_NORMAL_MEMORY`, binds it into the GATT, and returns `kern_info.aper_base + (pg_offset << PAGE_SHIFT)`.  `MtxRegion_Alloc()` records that value as both `base` and `busaddr` for `MTX_MEMTYPE_AGP`. |

The target's P690 is PCIe.  Adding an AGP GART interface there would describe
hardware which is absent and would be less safe than the existing VRAM path.

## Address-class evidence

The saved reverse-engineering record identifies the low address bits as bus
cycle classes:

* class 0: local frame-buffer memory;
* class 2: non-local memory accessed through AGP cycles;
* class 3: non-local memory accessed through PCI bus-master cycles.

See [parhelia-vertex-fetch-and-memory-classes.md](parhelia-vertex-fetch-and-memory-classes.md),
lines 30--40 and 127--135.  Its allocator decode maps vendor AGP type 1 to
class 2 and PCI-locked type 2 to class 3.  The captured 0528 board uses class
3.  It is therefore evidence for the selector encoding, but **not** a passing
AGP-board draw.

This distinction matters for the current implementation:

* `PARHELIA_RELOC_DRAW` patches `0x97` operands with
  `pdev->vertex_bus + range`; that is a class-3 coherent-DMA address.
* `PARHELIA_RELOC_DRAW_VRAM` checks a persistent, creating-file-owned VRAM GEM
  object, patches the same bounded range with its VRAM offset, then changes
  only the final stream-0 selector from 3 to 0.
* No current relocation can encode an AGP aperture address or select class 2.
  That is correct on the P690 and prevents an application from passing a host
  address as a claimed AGP address.

The primary and secondary DMA call addresses are also deliberately left class
3.  The saved vendor source proves AGP storage for client regions, not a
class-2 primary/secondary command-ring protocol.  An AGP experiment must not
retag the rings, WARP storage, or fence writeback merely because vertex memory
uses AGP.

## Vendor Linux implementation recovered from source

The source package is unusually useful here; the relevant code is readable,
not inferred from a string or a disassembly.

1. `mtx_dev.c` detects an AGP function with
   `pci_find_capability(pci_dev, PCI_CAP_ID_AGP)`, acquires the matching bridge,
   copies aperture information, and enables AGP during device initialization.
2. `mtx_agp.c` selects 1x/2x/4x/8x from the device/bridge capability words,
   calls the AGP backend enable function, and applies write combining to the
   aperture.
3. `mtx_mem.c:mtx_mem_alloc_agp()` allocates `AGP_NORMAL_MEMORY`, allocates a
   free page range in the aperture, binds it with `agp_bind_memory()`, and
   returns the aperture address.  Free reverses the bind before freeing.
4. `mtx_strintf.c:MtxRegion_Alloc()` assigns the aperture address to the
   engine-visible `busaddr` field for `MTX_MEMTYPE_AGP`.
5. `mtx_vm.c:mtx_vm_map_agp()` maps that aperture address into userspace.

The current 7.2 source tree still contains the necessary in-kernel backend API:
`include/linux/agp_backend.h` declares `agp_backend_acquire`, `agp_copy_info`,
`agp_enable`, `agp_allocate_memory`, `agp_bind_memory`, and their teardown
counterparts.  `drivers/char/agp/{backend,generic}.c` exports them, and this
kernel configuration has `CONFIG_AGP=m` plus AMD64, Intel, SiS, and VIA bridge
drivers enabled as modules.  Thus the blocker is not an absent API; it is the
absence of an AGP Parhelia board and a clean hardware-validation sequence.

## Current integration gaps

1. `struct parhelia_device` contains no AGP bridge, aperture allocator, or
   AGP allocation lifetime state.  Probe does not call
   `pci_find_capability(..., PCI_CAP_ID_AGP)` and the UAPI has no bus-type
   parameter.
2. All execution-visible host allocations are DMA32-coherent.  That is right
   for PCI/PCIe and has been exercised on the P690, but it is not an AGP GEM
   backing.
3. Mesa deliberately regards `PARHELIA_PARAM_VRAM_VERTEX` as a P690 capability
   and has no generic AGP optimization switch.  It needs no change for P690.
4. The current UAPI has no `PARHELIA_RELOC_DRAW_AGP`, and its strict local-VRAM
   helper requires the final selector to be exactly class 3 before it replaces
   it with class 0.  Reusing either existing relocation for AGP would lose
   ownership/range validation or silently select the wrong class.
5. PCIe versus PCI versus AGP is not detected or reported by the open driver.
   Device ID is insufficient: the vendor detects AGP through the PCI capability
   list, and 0528 exists in more than one board transport.

## Safe implementation plan for an AGP-equipped board

This work is separate from the P690 path and should remain disabled until it
passes on an AGP host.

1. Add an internal transport enum (`PCI`, `PCIE`, `AGP`) determined from PCI
   capabilities: detect AGP first with `pci_find_capability(PCI_CAP_ID_AGP)`,
   then PCIe with the standard PCIe capability helper.  Report it through a
   read-only `GET_PARAM`; do not infer it from device ID.
2. At probe on an AGP function only, acquire the bridge with
   `agp_backend_acquire(pci)`, copy its information, choose an AGP command
   word from the intersection of bridge/device capabilities, and call
   `agp_enable()`.  Any failure must leave AGP disabled and the ordinary
   class-3 DMA32 path available.  Probe/remove error paths must unbind every
   allocation before `agp_backend_release()`.
3. Introduce a distinct, private AGP BO type carrying `struct agp_memory *`,
   page count, aperture-page offset, returned aperture address, GEM ownership,
   and a reservation fence.  Allocate `AGP_NORMAL_MEMORY`, bind a range from a
   device-local bitmap or `drm_mm`, and map it through the DRM GEM mmap offset.
   Never expose the raw aperture address to userspace.
4. Add a new explicit `DRAW_AGP` relocation rather than overloading `DRAW` or
   `DRAW_VRAM`.  Validate creator/file ownership, the exact `0x97` byte range,
   32-bit aperture-address arithmetic, no mixed vertex backing in a submit,
   and a final stream-0 selector of 2.  Keep primary/secondary command DMA,
   writeback, and WARP in DMA32 class 3 for the first experiment.
5. Keep the Mesa allocator conservative: enable AGP vertices only after the
   kernel advertises a separate AGP capability and the AGP GEM mmap succeeds.
   Prefer local VRAM where the hardware validation permits it.  Preserve the
   existing DMA32 fallback if allocation or submit returns a pre-submit
   capability error.
6. Validate on a disposable AGP host in small stages: capability and bridge
   inspection; one page allocate/bind/map/unbind; an aperture range read/write
   check; a fenced class-2 point/triangle with known colors; repeated
   allocation/free and close-after-submit; then glxgears and ioquake3.  Collect
   PCI config, AGP command/status, stream selector, `0x97` operands, fence
   completion, and recovery behavior at each stage.  Do not run this sequence
   on the P690 target.

## P690 recommendation

Retain and optimize the existing local-VRAM `CREATE_VERTEX` path, with coherent
DMA32 only for the buffers which are necessarily bus mastered.  That already
provides the requested zero-copy engine-visible vertex memory on the actual
PCIe hardware.  Treat IOMMU mappings returned by the DMA API as a host service;
do not advertise them as a device GART.  AGP support can be added safely only
behind the separate capability, BO, relocation, and hardware test gate above.

## 2026-09-10 P690 revalidation: what is and is not zero-copy

The live target is `102b:2539`, subsystem `102b:0040`, and its PCI capability
list identifies an Express v2 endpoint, linked at 2.5 GT/s x16.  It has no AGP
capability.  The staged open module has `init_aperture=Y`,
`coalesce_primary=Y`, and `replay_fences=Y`; its current IRQ is shared on
IRQ 18.  This confirms that the target is the PCIe case in this document, not
an AGP board being used through a bridge.

The word *zero-copy* needs a precise boundary here:

* The P690 path is zero-copy **between the persistent mapped local-VRAM BO and
  the engine**.  `CREATE_VERTEX` creates a 4-byte-aligned, file-owned, pinned
  VRAM object (up to 1 MiB); Mesa maps it and `DRAW_VRAM` resolves the bounded
  `0x97` ranges directly to its class-0 offset.  No kernel staging vertex
  upload or GPU-visible host address is supplied on that path.
* Mesa currently still copies its software-transformed temporary vertices into
  that mapping before every submit.  It allocates only `sizeof(ctx->vertices)`
  (the current 5400-dword maximum transformed batch), always writes at offset
  zero, and waits for the synchronous submit to retire before reuse.  The
  separate class-3 fallback upload remains capped at 4096 dwords.  This is a
  CPU copy into VRAM, not a hidden GART operation.
* The fallback is a genuine class-3 bus-master path: the kernel copies the
  transient user vertices to its coherent DMA32 buffer, patches the `0x97`
  start/end operands with `vertex_bus + range`, and keeps the primary,
  secondary, WARP, and writeback allocations kernel-owned.  The verified
  aperture initialization makes this raw host-DMA path usable; it does not
  install a Parhelia GPU page table.

The old Parhelia vendor source independently supports this split.  Its
`MTX_MEMTYPE_PCI_LOCKED` allocator demands physically contiguous, DMA-visible
pages and gives the engine `virt_to_bus()`; its AGP allocator instead returns
`aper_base + page_offset` after binding pages into the GATT.  The distinct
addresses explain class 3 versus class 2.  The newer M9 source's `SCATTER`
allocator is for a different Phoenix-generation device and is not evidence
that P690 can consume a scatter-gather list.

### P690-safe next capability

The implementable P690 improvement is **resident local-VRAM indexed drawing**,
not a PCIe-GART interface.  Extend the existing file-owned `CREATE_VERTEX`
object with narrowly validated class-0 stream and index relocations, then let
Mesa retain the software-transform result and index list in that object.  The
exact `0x9f` packet and validation boundary are specified in
[parhelia-p690-native-indexed-draw-feasibility.md](parhelia-p690-native-indexed-draw-feasibility.md).
It eliminates the per-triangle `0x97` fetch submission while preserving the
current trusted address model.  It is the highest-value way to use the 1 MiB
allocation limit, but must start with its standalone readback test.

Do **not** add a general imported/user-page `DRAW_DMA` interface on this
evidence.  A direct `0x97` range requires one contiguous 32-bit DMA address;
the current UAPI cannot describe an SG list, and an IOMMU can return several
DMA segments.  Treating the first mapped segment plus a byte count as one
range can fetch unrelated memory.  Such an interface needs a proven hardware
scatter-list format or a kernel-owned contiguous DMA allocation, neither of
which current P690 captures show.

### Revalidation test plan

1. Run the existing Mesa `vram_vertex_reloc_selftest` and the standalone P690
   class-0 triangle/readback test after every kernel ABI change.
2. For indexed drawing, first compare one local-VRAM four-vertex/two-triangle
   `0x9f` render with the existing `0x97` image hash, then stress all offset and
   exclusive-end bounds, close-after-submit, and repeated map/write/submit.
3. Count Q3 `DRM_IOCTL_PARHELIA_SUBMIT` calls and compare `glxgears` and
   `ioquake3 +set cl_renderer opengl1 +map q3dm1` images and uncapped FPS.
   Test only with the staged `/tmp/parhelia-stage` module; never place an
   experimental module under `/lib/modules`.
4. Reserve any AGP implementation or test for an actual AGP Parhelia host.
   On this target, verify that any attempted AGP capability query returns
   unsupported rather than silently retagging class-3 DMA as class 2.
