# Vendor linear framebuffer aperture and a DRM/GEM design

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is clean-room reverse engineering from the saved vendor binaries.  The
principal Linux userspace binary is
`matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so`; the Windows
cross-check is `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`.  No target
register writes were made for this analysis.

## Result

The vendor does **not** keep a linear shadow of the primary framebuffer.  The
primary is one native local-memory surface (type 3, the displayable/2D type),
and the CPU sees a linear alias of it through a programmable BAR0 host-aperture
window.  The same storage is consumed directly by display scanout and by the
2D/3D engines.  Therefore the current private tiled KMS BO plus damage-copy
scheme is a compatibility implementation, not the vendor performance model.

This is a surface-layout translation unit, not an MMU or page table.  GPU
surface addresses remain plain local-VRAM byte offsets.  Each of twenty global
descriptors tells the BAR host path that a range of aperture offsets aliases a
particular GPU surface and supplies that surface's format, type, width, height,
and depth.

## Descriptor format

`EmulMmSurfaceMapper::AllocateApertureBlock` at Linux X-driver `.text+0x74c40`
allocates and programs one 16-byte entry.  The bank occupies MMIO
`0x0000..0x013f`, twenty entries at `slot * 0x10`:

| Offset | Meaning |
|---|---|
| `+0x00` | bit 28 valid, bits 27:0 BAR0 aperture-window address; the Linux allocator combines the 256-KiB-aligned window offset with BAR address bits `[27:25]` cached at mapper `+0x160` |
| `+0x04` | aliased surface GPU byte address, with the same low two-bit memory-class encoding as a drawing-engine surface base |
| `+0x08` | surface control: width `[13:0]`, type `[18:16]`, format `[29:24]` |
| `+0x0c` | height `[13:0]`, depth `[27:16]` |

The driver's cold baseline is:

```
slot 0 = { 0x10000000, 0x00000000, 0x10000020, 0x00010001 }
slots 1..19: clear the valid top nibble of dword 0
```

That is an identity one-row type-1 bootstrap window.  It does not describe a
real screen.

For each actual surface, the allocator:

1. obtains `GetHostApertureSize()` from the surface descriptor;
2. reserves an aperture range in 256-KiB units, with a 256-KiB guard/gap in
   the observed allocation arithmetic;
3. chooses a free descriptor slot in `0..19`;
4. writes the surface base, control and dimensions to `+4/+8/+c`;
5. writes the valid aperture address to `+0` last.

When occupied entries are moved, it copies all four saved dwords to the new
slot.  On release it invalidates the entry.  Programming `+0` last and clearing
it on release are the publication/revocation operations.

The aperture address space starts at engine address `0x10000000`; the low
28-bit portion corresponds to an offset in the framebuffer BAR mapping.  This
is separate from the surface's local-VRAM address in dword 1.  Consequently a
DRM mmap of the BO's physical VRAM offset is not generally the vendor mapping:
the mmap must cover the descriptor's aperture-window offset.

## The primary framebuffer path

The Linux X binary's `InitPrimarySurface` at `.text+0x7f1a0` constructs the
primary as:

* local memory class 0;
* purpose 1, allocated upward from the bank-A primary heap;
* surface type 3;
* the active display format;
* width rounded as required by the descriptor and height/depth in the dimension
  word.

Live vendor captures contain the corresponding non-bootstrap descriptor, for
example:

```
{ 0x10040000, 0x00000000, 0x30030780, 0x00010708 }
```

This maps a BAR aperture window beginning at 0x40000 onto local surface base
zero, with 32-bpp format 0x30, type 3 and width 1920.  The CPU stride returned
by `MmSurfDesc::GetHostAperturePitch` (`.text+0x745c0`) is
`next_pow2(max(width, 32)) * bytes_per_pixel` for types 2 through 5.  Thus a
1920-wide 32-bpp primary is presented to the CPU with an 8192-byte linear
stride.  The hardware translates those sequential host accesses to the native
surface layout used by scanout and the engines.

`MmHostAperture::Lock` (`.text+0x72e20`) returns two views of the same mapping:

* an engine/aperture address derived from the allocated aperture block; and
* a CPU pointer derived from the BAR mapping plus the aperture offset.

There is no memcpy, detile loop, or second allocation in Lock.  Unlock merely
drops a lock count and marks the aperture releasable.  Low-priority transient
apertures are released immediately; cached/pinned apertures retain their
descriptor.  The primary descriptor is therefore long-lived rather than
reprogrammed for every access.

The alias is bidirectional.  The vendor XAA fast path implements
`UploadToScreen` for GXcopy/full-planemask as a row memcpy directly into the
destination pixmap aperture.  Its download paths wait for the surface fence and
read rows from an aperture mapping.  More complicated ROP/planemask transfers
use a temporary aperture-mapped linear surface plus a hardware blit, because
the CPU copy cannot implement those raster operations, not because the
aperture is write-only.

There is also a direct X-server witness.  `MTXScreenInit` (`.text+0x43010`)
loads the primary's host-aperture offset from driver-private `+0x50`, adds it to
the framebuffer BAR mapping at the mapped-device record's `+0x48`, and saves
that sum in `r13` (`0x432da..0x432df`).  At `0x433e0` it passes `r13` as the
framebuffer pointer to `fbScreenInit` (`0x433e7`).  No ShadowFB allocation,
detile pass, or screen-update memcpy lies between those operations.  Xorg's fb
layer therefore writes ordinary row-major pixels directly through the
hardware-translated aperture into the native primary allocation.

The Windows display driver uses the same four-dword layout and registration
model.  Its miniport only installs the slot-0 baseline; the display driver
programs the real surface entries.  Captures also show transient descriptors
for upload/readback buffers, confirming that the table is a general CPU host
view of surfaces rather than scanout state.

This is also the normal Windows policy rather than a Linux-only shortcut.
`MTXPARD.dll` registers `DirectX.PrimaryLockToUserMemEnable` at
`0x109b40..0x109b60`: the two default operands passed to the registry-value
constructor are zero and the resulting policy object is stored at manager
`+0x1c4`.  Its later consumer is constructed at `0x90fe2`.  Thus locking the
primary into user/system memory is an optional, default-disabled fallback; the
ordinary primary remains local VRAM and is made CPU-linear by the aperture.
The saved P-series/P690 `MtxPser/mtxpserd.dll` contains the same
`PrimaryLockToUserMemEnable`, `MmMaxApertureSize`, and `FlipOnVBlank` policy
set, which cross-checks that this architecture survived into that driver.

## Native views and chipset constraints

The vendor does not convert pixels when changing an allocation between 3D and
display use.  `SurfDesc2dList::AssignCurrentSurfDescTo` changes the same live
surface descriptor between type 2 (3D view) and type 3 (2D/display view), and
the allocator gives both types identical storage requirements.  The host
aperture is a third, CPU-linear view of those same bytes.  Consequently the
high-performance path is one native allocation with view metadata, not a
linear render allocation followed by a tiled scanout allocation.

Do not infer a universal tile geometry merely from the host pitch.  The saved
0527/0528 code requires 8-KiB display-start alignment at 32 bpp, while
`HSLPARGetAddressAlignmentMask` uses one less alignment bit for 2537--2539
(P-series/LX).  Surface alignment, display-start alignment and host-aperture
pitch are separate constraints.  Preserve chipset-specific layout data in a
modifier/BO-layout record rather than baking the original Parhelia alignment
into the P690 path.  See `parhelia-engine-context-bringup.md` section 2.2 and
`parhelia-own-render-target.md` section 8 for the underlying binary evidence
and the remaining open physical-swizzle question.

## Presentation and page flipping

The vendor also avoids CPU presentation copies.  A windowed GL swap submits a
2D engine blit from the native back surface to the native primary.  A
flip-capable swap queues a render-completion tag in the bus-master DMA stream;
the vblank ISR reads the retired tag at `0x41c` (masked with `~7`) before it
selects the new scanout slot.  The hardware sequence recovered from
`mtx_parhelia.o` is:

1. choose a free slot in the four-entry START bank;
2. write `surface_offset & 0x07fff800` to the slot register;
3. after the preceding DMA tag retires, select that slot in the plane control
   register from the vblank handler;
4. regard the flip as retired when the control register's pending and active
   slot fields agree.

| Plane | START bank | Control/select field | Active field |
|---|---:|---:|---:|
| head 0 graphics | `0x1824 + slot*4` | `0x1800[8:7]` | `0x1800[17:16]` |
| head 1 graphics | `0x1a24 + slot*4` | `0x1a00[8:7]` | `0x1a00[17:16]` |
| head 0 second plane | `0x19c8 + slot*4` | `0x19a8[10:9]` | corresponding active field is not separately decoded here |
| head 1 second plane | `0x1bc8 + slot*4` | `0x1ba8[10:9]` | corresponding active field is not separately decoded here |

For a graphics plane, pending is `(ctrl >> 7) & 3` and active is
`(ctrl >> 16) & 3`.  Control bit 6 requests immediate latching; leaving it
clear latches at retrace.  `reg3DFlipOnVBlank` defaults to application-defined
policy (value 3), and swap intervals greater than one are counted by the ISR.
The software flip queue has 32 entries even though the hardware START bank has
four.  The exact call chain and addresses are in `parhelia-rendertarget.md`
sections 6.2--6.5.

## Synchronization and ownership

The aperture lock function itself does not wait for rendering.  Vendor callers
perform surface-busy/DMA checks before CPU access, while the memory manager
serializes descriptor allocation, movement, and release.  A DRM implementation
therefore needs normal GEM reservation/fence synchronization around CPU access;
the descriptor is an address translation mechanism, not cache coherency.

Required ordering is:

* wait for conflicting GPU writes before granting CPU read/write access;
* finish/write-combine-flush CPU writes before a GPU or display consumer is
  allowed to observe them;
* never rewrite or invalidate a descriptor while a VMA using its BAR range can
  fault or access it;
* write descriptor dwords 1--3 before publishing valid dword 0;
* clear valid dword 0 before recycling the underlying surface or aperture
  range;
* serialize descriptor programming with engine reset and KMS teardown.

The twenty-entry table is global hardware state.  It cannot safely be exposed
as a per-process register interface, and slots cannot be leased only for the
duration of a syscall because VMAs outlive syscalls.

## Proposed DRM/GEM implementation

Use a distinct mmap mode for native-layout VRAM BOs rather than changing the
meaning of every existing VRAM mmap.

1. Add an internal `parhelia_host_aperture` manager owning slots 1--19 and BAR0
   aperture ranges.  Keep slot 0 as the bootstrap identity entry.  Protect the
   manager with its own mutex, nested outside submit/reset locking in one fixed
   order.
2. A scanout-capable native BO records `{slot, aperture_offset, gpu_offset,
   ctrl, dims, map_refcount}`.  Allocate the descriptor at BO creation or first
   mmap and keep it pinned while the BO can be scanned out or has live VMAs.
3. Return a GEM mmap offset whose VMA handler remaps
   `BAR0_physical + aperture_offset`, **not** `BAR0 + gpu_offset`.  The mapping
   remains ordinary linear pixels to userspace, with pitch equal to the vendor
   host-aperture pitch.  Report that pitch in `CREATE_DUMB`/framebuffer metadata.
4. Program the same BO's native GPU offset and type-3 control directly into the
   display controller.  Remove the private native scanout BO and the
   linear-to-tiled damage resolve for this path.
5. Keep raw/direct VRAM mappings as a separate internal/debug path.  A BO must
   never simultaneously promise raw-native and aperture-linear mmap semantics
   through the same offset.
6. Use `drm_gem_object` VMA open/close hooks to hold the slot and aperture range.
   Descriptor eviction is allowed only for BOs with no VMA, no framebuffer pin,
   and no CPU access in progress.  With only one root scanout, simply pinning its
   slot for its lifetime is preferable to implementing eviction first.
7. Integrate `dma_resv` fences and begin/end CPU-access hooks.  WC mappings need
   an `mmiowb()`/architecture WC flush before publishing CPU completion.  GPU
   completion fences cover GPU-to-CPU transitions.
8. Validate exact descriptor coverage and pitch.  The aperture allocation size
   can exceed physical BO size because it represents a linear alias; bounds
   checks must use both quantities independently.
9. Define a Matrox native-layout DRM format modifier before exporting these BOs
   through PRIME.  `DRM_FORMAT_MOD_LINEAR` describes the physical layout seen by
   other devices, not merely the row-major view provided by this device's CPU
   aperture.  Importers without the modifier must continue receiving a true
   linear BO.
10. Let Mesa allocate native render/display BOs with that modifier.  Present a
    window with the 2D native-to-native blit and use an atomic KMS page flip for
    an eligible full-screen BO.  Carry render fences through `dma_resv`; only
    add the four-slot vendor queue after its controller fields are confirmed on
    the target chipset.

The first safe implementation can reserve one fixed slot and one fixed BAR
aperture range for the primary BO.  That is sufficient to eliminate the KMS
shadow/damage copy and matches the vendor primary behavior.  Generalizing it to
offscreen tiled GEM mappings should follow only after VMA lifetime and slot
exhaustion behavior are complete.

## What this changes for performance

This removes the current full-frame linear-to-native resolve and the extra
private scanout allocation.  CPU X rendering writes through the hardware
linear alias directly into the native screen surface; GPU rendering can target
the same native BO.

It does not by itself accelerate X's software composition of a GL window.  The
vendor stack also uses its 2D engine/DRI presentation paths instead of copying
all window content on the CPU.  After the direct primary mapping is in place,
glamor or a native accelerated Present path is still needed for high windowed
OpenGL throughput.  The aperture change removes one large mandatory copy and
makes that acceleration possible without layout conversion.

## Evidence addresses

* Linux X driver `MmHostAperture::Lock`: `0x72e20`; `Unlock`: `0x72ec0`.
* Linux X driver `MmSurfaceMapper::AllocateAperture`: `0x74a60`.
* Linux X driver `EmulMmSurfaceMapper::AllocateApertureBlock`: `0x74c40`;
  `FreeApertureBlock`: `0x75a00`.
* Linux X driver `MmSurfDesc::GetHostAperturePitch`: `0x745c0`;
  `GetHostApertureSize`: `0x74670`.
* Linux X driver `MmSurface::AttachToBlock`: `0x73710`.
* Linux X driver `InitPrimarySurface`: `0x7f1a0`.
* Linux X driver `MTXScreenInit`: aperture pointer construction
  `0x432da..0x432df`, direct `fbScreenInit` call `0x433e0..0x433e7`.
* Windows `MTXPARD.dll` `PrimaryLockToUserMemEnable` registration:
  `0x109b40..0x109b60`; consumer construction: `0x90fe2`.
* Windows `MTXPARD.dll` aperture registration and the saved MMIO captures are
  summarized in `parhelia-engine-and-context-init-complete.md` section 1.4.
* Linux kernel-object DMA-tag/vblank/START-bank flip path is decoded in
  `parhelia-rendertarget.md` sections 6.2--6.5.
