# P690 texture-upload aperture alias

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note records clean-room reverse engineering of the saved Matrox Linux and
Windows binaries and correlates it with the September 2026 P690 hardware
capture.  It addresses host-aperture descriptors and texture upload only.  The
labels used below are:

* **[BIN]** behavior recovered from a saved vendor binary;
* **[HW]** behavior directly observed on the P690 test board; and
* **[INF]** an inference that accounts for the binary and hardware evidence.

No vendor driver was executed and no target state was changed for this
analysis.

## Finding

The latest low-address upload test violated a critical vendor invariant: the
twenty hardware descriptors are kept in ascending dword-0 order.  The test
bank was:

```
slot 0  d0 = 0x00000000
slot 1  d0 = 0x00100000  (root)
slot 2  d0 = 0x00cc0000
slot 3  d0 = 0x00040000  (temporary upload)
```

**[BIN]** The vendor allocator would insert `0x00040000` at slot 1 and shift
the old slots 1 and 2 upward.  Appending it at slot 3 makes the bank unsorted,
so the all-low-address corruption does not implicate descriptor type 0, width
8192, or dword-1 target `0x00ca4000`.

The first native upload used a sorted bank but an unsafe dword-0 coordinate of
`0x01440000`.  Independent P690 BAR tests show that host coordinates at or
above 16 MiB are dropped or alias lower coordinates, and `0x01440000` aliases
the root aperture at `0x00440000`.  The native fix therefore needs a real
aperture allocator which preserves ascending slot order, keeps P690 aperture
coordinates below 16 MiB, and reserves the vendor's inter-range guard.  Whether
a dword-1 local target above 16 MiB is independently unsafe remains open; both
low-dword-0 follow-up tests were unsorted and must be repeated with a correctly
ordered bank.

## Descriptor bank and fields

**[BIN]** `EmulMmSurfaceMapper::AllocateApertureBlock` in Linux
`mtx_drv.so` (`.text+0x74c40`) manages twenty 16-byte descriptors in MMIO
`0x0000..0x013f`:

| Dword | Meaning |
|---|---|
| `+0x00` | bit 28 valid/publication; bits 27:0 host/BAR aperture coordinate, including cached BAR bits `[27:25]` where required |
| `+0x04` | translation target/local-memory byte address; low two bits retain the vendor memory-class encoding |
| `+0x08` | format `[29:24]`, surface type `[18:16]`, width `[13:0]` |
| `+0x0c` | depth/slices `[27:16]`, height `[13:0]` |

The allocator writes dwords 1, 2, and 3 before publishing valid dword 0.
Release invalidates dword 0 before the slot or range is recycled.  Descriptor
records are kept ordered by host coordinate and shifted/reprogrammed during
compaction.

The descriptor's two addresses must not be interchanged when programming an
engine surface slot.  **[BIN]** The complete allocation-to-bind chain proves
which address an aperture-backed surface uses:

* `EmulMmSurfaceMapper::AllocateApertureBlock` loads the unmodified old
  high-water coordinate at `.text+0x74c9c`.  A separate copy is combined with
  the cached BAR selector bits and `0x10000000` at `0x74cc9..0x74cfc`; only
  that copy becomes descriptor dword 0.
* At `0x756db..0x756f8`, the allocator stores the original, unmodified
  high-water coordinate into the returned `MmBlock+0x20`.
* `MmSurface::AttachToBlock` (`.text+0x73710`) copies `MmBlock+0x20` verbatim
  into the surface's engine address at `+0x90`; `Surface2d::Enable` later
  copies it into the active SURF base at `+0xe0`.

Thus descriptor `d0=0x10040000` is published with bit 28 set, while the SURF
base for that mapped surface is `0x00040000`.  Bit 28 is not part of the SURF
address.  Dword 1 is the translation target and likewise is not the value to
bind for an aperture-backed surface.  A native driver that gives a BO a host
descriptor must bind its low dword-0 coordinate, with the publication bit
removed, when an engine operation is intended to pass through that mapping.
Class-3 system surfaces are direct instead: their SURF base is the DMA bus
address ORed with 3.

**[BIN]** `MmSurfDesc::GetBitsPerPixel` (`.text+0x73e70`) maps format
`0x10` to 8 bits per pixel.  Dword-2 values used in the live test therefore
decode as:

```
0x10002000 = format 0x10, type 0, width 8192
0x10012000 = format 0x10, type 1, width 8192
```

**[BIN]** `MmSurfDesc::GetHostAperturePitch` (`.text+0x745c0`) decodes
width from bits `[13:0]` and type from `[18:16]`:

* types 0 and 1 use `ALIGN(width, 32) * bpp / 8`;
* types 2 through 5 use
  `next_power_of_two(max(width, 32)) * bpp / 8`; and
* widths greater than 8191 take the saturating path and produce
  `8192 * bpp / 8`.

For format `0x10`, width 8192 consequently has an 8192-byte host pitch.

**[BIN]** The software `MmSurfDesc` has one additional dword which is not part
of the hardware entry:

```
MmSurfDesc +0x00  target base       -> hardware dword 1
           +0x04  control           -> hardware dword 2
           +0x08  dimensions        -> hardware dword 3
           +0x0c  raw byte extent   -> software allocator only
```

`MmSurfDesc::GetHostApertureSize` (`.text+0x74670`) treats type 0 specially:
it rounds `MmSurfDesc+0x0c` to 32 bytes.  `AllocateApertureBlock` copies only
the first three software dwords into hardware dwords 1--3.  Thus hardware
`dims=0x00010001` is correct and is not accidentally interpreted as 65,537
bytes by the chip.  Type 1 rounds hardware height to an even value and
multiplies by host pitch.  Types 2 through 5 apply their height/depth rules and
multiply by host pitch.

The vendor uses a canonical minimum-width hardware shape for raw type-0
windows, even when their software extent is much larger:

```
whole-VRAM identity: ctrl=0x10000020, dims=0x00010001
cursor raw storage:  ctrl=0x04000020 or 0x05000020, dims=0x00010001
```

The cursor constructor stores a variable multi-buffer extent separately at
`MmSurfDesc+0x0c`.  Consequently an 8192-byte native raw window should first
be tested in the vendor form `ctrl=0x10000020`, `dims=0x00010001`, with an
8192-byte software-managed extent.  `ctrl=0x10002000` really does encode width
8192 and is accepted by the pitch helper, but no recovered vendor type-0 use
needs the raw extent in the hardware width field.

## Vendor aperture-coordinate allocation

**[BIN]** The Linux allocator stores its dword-0 high-water coordinate at
mapper offset `+0x15c`, independently of the local-memory allocator that
selects dword 1.  At `.text+0x74c9c..0x74cf5` it performs:

```
start  = old_high_water;
new_high_water = ALIGN(old_high_water + host_aperture_size + 0x40000,
                       0x40000);
```

The exact instruction sequence adds `0x7ffff` and masks with
`0xfffc0000`.  It therefore allocates in 256-KiB units and leaves an extra
256-KiB guard/gap.  The host coordinate is combined with valid bit 28 only
when the descriptor is published.

**[BIN]** Slot order is part of this allocator, not incidental bookkeeping.
At `.text+0x74d05..0x74d2f` it compares the new dword-0 key against each live
entry and selects the first entry with a greater key.  It then copies every
later 16-byte record one slot upward and reprograms the affected MMIO entries.
The resulting live prefix is strictly ascending by host coordinate.  The
type-0 identity entry at coordinate zero supplies the default translation;
later entries override it at increasing boundaries.  This explains why the
range extent need not be encoded in a type-0 hardware descriptor.

**[BIN]** `EmulMmSurfaceMapper::FreeApertureBlock` (`.text+0x75a00`)
subtracts `ALIGN(size, 0x40000) + 0x40000` from the high-water value, finds the
entry by dword-0 coordinate, and compacts later entries.  Transient mappings
are therefore reclaimed and moved within this independent virtual space; they
do not climb through VRAM as successive physical BOs are created.

The mapper caches `BAR0 & 0x0e000000` at `+0x160`; those bits are zero for the
test P690's `0xf0000000` BAR0 base.

**[BIN]** The saved P690 Windows display driver `MtxPser/mtxpserd.dll`
constructs a corresponding aperture manager around image offsets
`0x8fae7..0x8fb0f`.  It takes the lower of a policy/hardware value and the
framebuffer size, applies a default 128-MiB ceiling when VRAM is larger, and
rounds the resulting capacity down with `& 0xfffc0000`.  The associated
registry policy is named `MmMaxApertureSize`.  This cross-checks an explicit,
256-KiB-granular aperture-address capacity distinct from physical surface
allocation.  The static Windows code does not by itself prove the P690's
16-MiB effective limit; that limit comes from the live hardware measurements.

## Vendor upload behavior and chunk size

**[BIN]** The saved Linux kernel driver's upload routine `mtx2222`
(`mtx_parhelia.o`, `.text+0x50fb0`) clamps the requested offset and length to
the surface size, maps/locks the destination through `mtx1587(surface, 1,
flag)`, invokes one `LocMemCpy(mapped + offset, source, length)`, and unlocks
through `mtx3266`.  There is no fixed 8-KiB memcpy loop in this routine.

The vendor OpenGL driver also maintains a 4-MiB system-memory staging ring with
per-8-KiB-page fences.  Those pages are staging/fence bookkeeping, not evidence
of an 8-KiB host-aperture transfer limit.

**[BIN]** The actual opcode-`0x0a` texture path confirms that this is not a
bus-master pixel upload.  Dispatcher `mtx2515` (`.text+0x23140`) selects:

* `mtx2146` (`.text+0x21120`) for the swizzled texture pool;
* `mtx1828` (`.text+0x21730`) or `mtx3550` (`.text+0x21b70`) for ordinary
  uncompressed row copies; and
* `mtx2799` (`.text+0x20720`) for DXT block swizzling.

These routines obtain a CPU destination pointer and write it with
`LocMemCpy`, scalar stores, or the recovered 32-by-16 texture swizzle.  In the
mapped-object branch, `mtx2779` (`.text+0x55fd0`) calls
`OsMapPhysicalMemory(memobj+0x40, memobj+0x4c)` and returns that mapping plus
the allocation offset at object `+0x70`.  No 2D blit packet and no bus-master
pixel-data descriptor is emitted by this upload route.

The transient descriptor captured around vendor draws has the different form
`{d0 ~= 0x10f00000, d1 = system bus address, 0x3203xxxx, 0x0001002x}`.  It is a
system-memory pull window used for vertex/command data whose engine address has
space class 2.  It must not be copied as a texture-pixel DMA recipe.

The native driver's 8-KiB chunks are implementation-specific.  The vendor
upload path copies the full clamped request through the actual destination
surface mapping.  For type 0, the opaque byte extent is software state and the
vendor uses width 32 in hardware, so neither the 8-KiB staging pages nor the
pitch helper's 8192-pixel saturation establishes an 8-KiB hardware transfer
limit.

The destination base in dword 1 is the translation target address and retains
the low-bit memory-class convention used elsewhere by the engine.  Dword 0
must be 256-KiB aligned and within the chipset's usable host-coordinate range.
The current evidence does not isolate the address limit of dword 1 because the
two low-dword-0 trials both used an unsorted descriptor bank.

**[BIN]** Dword 1 is a byte address, not a page number.  The common resolver
adds the allocation byte offset and does not mask any high address bits; only
bits `[1:0]` are consumed as the memory-class tag.  For a local format-`0x10`,
type-0 allocation, `GetRequiredAlignment(1)` returns `0x200` bytes (or
`0x100` when the descriptor's half-width flag is set).  The alternate selector
returns `0x40` bytes.  The corresponding system-memory helper returns `0x20`
bytes.  The live target `0x00ca4000` satisfies all of these base constraints,
which further points at the unsorted bank rather than base misalignment in the
all-low test.

## Coherent system-memory 2D fallback

**[BIN]** The Linux kernel helper at `mtx_parhelia.o:.text+0x4c730` resolves a
surface address as `allocation_base + byte_offset`, then applies the memory
class directly in bits `[1:0]`: 0 for local VRAM, 2 for AGP and 3 for
PCI/system memory.  `Surface2d::Enable` in the Linux X driver
(`mtx_drv.so:.text+0x7bf00`) independently ORs 3 into the base of a non-local
surface.  There is no page shift or separate registration index.  A class-3
address must therefore be a real 32-bit DMA bus address with its low two bits
available for the tag.

The vendor-exercised Linux readback at `mtx_parhelia.o:.text+0x4fff0` proves a
2D BITBLT between local and class-3 system memory.  It binds an arbitrary local
source in slot 2, a type-1 system destination in slot 0, sets destination base
to `bus_address | 3`, and uses the ordinary copy state:

```
DWGCTL = 0x000002c2   # opcode 2, GXcopy, source slot 2, destination slot 0
PLNWT  = 0xffffffff
SRC, DSTXY, LEN
```

There is no residency or direction bit beyond `0x2c2`.  Before repeated
readback blits, this particular Linux path emits the normal serialized engine
bracket encoded by tag `0xc0afddc0`: `DWGCTL=0x707`, a zero auxiliary value,
`DWGSYNC=sequence|1`, then restores `DWGCTL=0x2c2`.  That is ordering/fence
state, not a class-3 transfer mode.  The Windows generic GXcopy builder does
not add a direction flag either.

The generic Linux and Windows 2D binders copy the class-tagged base unchanged
into either source or destination slot.  The Windows copy routine around
`mtxpserd.dll:0x155200` emits the same slot-2 to slot-0 `0x2c2` packet from two
general surface objects.  At `mtxpserd.dll:0x9f663`, the Windows driver also
constructs such a general system surface by ORing 3 into its bus address and
storing the result at object `+0xa0`; the generic copy routine reads base/config
pairs at object `+0xa0/+0xa4` without changing the class.

**[BIN/HW]** Reversing residency needs no direction or bus-master flag: bind
`dma_bus | 3` as the source and a class-0 local coordinate as destination.
This 2D path addresses the coherent buffer directly through the class-3 base;
it does not allocate or consult an MMIO `0x0000..0x013f` host-aperture
descriptor.  The saved driver explicitly exercises the opposite
local-to-system direction; a dedicated system-to-local pixel upload call site
was not found because the vendor normally CPU-copies texture pixels through
host aperture mappings.  The later live test nevertheless proves that this
P690 reads the class-3 source in the system-to-local direction.

For a modern coherent allocation, set the device's coherent DMA mask to 32
bits, reject a returned address above `U32_MAX`, fill it on the CPU, and issue
`dma_wmb()` before the command doorbell.  Wait for the engine fence before
rewriting or freeing it.  `dma_alloc_coherent` needs no `dma_sync_single_*`
ownership transition.  A streaming mapped buffer would require the normal DMA
API sync for device ownership instead.  Neither saved driver emits an extra
cache-control packet for class-3 reads.

## Live P690 alias proof

**[HW]** After one native upload the descriptor bank contained:

```
slot 0  { 0x10000000, 0x00000000, 0x10000020, 0x00010001 }
slot 1  { 0x10080000, 0x00a40000, 0x30030780, 0x000104b0 }
slot 2  { 0x113c0000, 0x013a0000, 0x30030040, 0x00010040 }
slot 3  { 0x00000000, 0x013a4000, 0x10012000, 0x00010001 }
```

Slot 3 is shown after revocation; while armed, its dword 0 was `0x11440000`.
The type-0 trial used control `0x10002000`; the type-1 trial shown above used
`0x10012000`.  Both changed the identical 8192 bytes of the root image.

The root native BO occupies GPU/physical pages `0x0a40..0x13a0`, byte range
`0x00a40000..0x013a0000`.  That physical placement is dword 1 and is distinct
from the root's dword-0 aperture range.  Slot 1 maps a host range beginning at
`0x00080000`; its 8192-byte pitch and 1200-line height make the range:

```
[0x00080000, 0x00080000 + 8192 * 1200)
    = [0x00080000, 0x009e0000)
```

**[HW]** Independent BAR tests on this P690 found that raw CPU writes at host
coordinates at or above `0x01000000` are not uniquely decoded: they are
dropped or alias lower coordinates.  For the failed upload:

```
0x01440000 & 0x00ffffff = 0x00440000
```

`0x00440000` is inside slot 1's root aperture range.  The lower-numbered root
descriptor wins the overlap, so the 8192-byte CPU write is translated into
the root surface.  This predicts the observed damage size and location.

Slot 2's coordinate `0x013c0000` likewise exceeds 16 MiB and folds to
`0x003c0000`, also inside the root aperture.  It is therefore a latent mmap
corruption path even though it was not the transient upload slot.

### Direct class-3 BITBLT launched, but its SURF state was stale

**[HW]** A follow-up bound coherent source `0x35400000 | 3` and physical
texture address `0x013a4000` directly in the 2D source/destination registers.
The BITBLT retired normally in 74 microseconds, but changed the root beginning
at row 402.  That result is an exact address-translation signature, not an
ignored MMIO surface write:

```
class-0 SURF destination       = 0x013a4000
P690 24-bit coordinate fold    = 0x003a4000
root descriptor coordinate d0 = 0x00080000
root translation target d1    = 0x00a40000

0x00a40000 + (0x003a4000 - 0x00080000) = 0x00d64000
0x00d64000 - 0x00a40000 = 0x00324000 = 402 * 8192
```

The intended 8192-byte source data reached local memory, so an operation
launched and the class-3 source direction is viable.  Initially, the exact
row-402 arithmetic looked like proof that the newly written high class-0 SURF
base was translated through the root descriptor.  A stricter follow-up rules
that conclusion out: with a correctly sorted low descriptor live, changing
the destination successively to `0x00040000` and `0x10040000` still produced
byte-identical root rows 402/403 for a 16-KiB copy.  The writes to the SURF
register bank therefore had not become active; the operation reused stale
destination state.  Reads of `0x1220..0x138c` all returned the last DWGCTL
value `0x2c2`, so these registers do not have ordinary readback semantics.

The vendor allocation-to-bind chain nevertheless proves the address required
once the state is accepted: descriptor `{d0=0x10040000,d1=target,...}` binds
as `SURF0_BASE=0x00040000`, with bit 28 removed.  The ordinary live root SURF
analogously uses its dword-0 coordinate rather than its translation target.
The existing texture mapping at `d0=0x013c0000`, `d1=0x013a0000` remains
unusable because its dword-0 coordinate exceeds the P690 host limit.

The Windows P690 copy path is direct MMIO, not DMA-only.  Its `memWriteDword`
helper at `mtxpserd.dll:0x1b730` stores the value directly at
`device[0x2024] + register`.  Direct access has an explicit hardware ownership
protocol, however.  The acquire helper at `0x33bc0` writes 1 to `0x0940`, polls
that register until bit 16 is set, and then writes `DWGCTL=0x707` and
`0x1374=0`.  The release helper at `0x33c80` again waits for bit 16 and writes
zero to `0x0940`.  A kernel mutex does not replace this hardware handshake.

Within that bracket, ordering is significant: the copy path writes state at
`0x12b0`, writes `DWGCTL=0x0a`, then programs `0x1220/0x1224` and
`0x1230/0x1234`, the plane mask and geometry, and finally the actual DWGCTL and
LEN.  It closes with `DWGCTL=0x707` and `0x1374=0`.  The native direct test
omitted both the `0x0940` ownership bracket and this state-opening sequence;
that is the leading explanation for the stale SURF values.  Tagged DMA remains
a sound way to inherit ordered context state without the direct-access gate,
but packet delivery is not an intrinsic property of these registers.

### Low-dword-0 countertests were unsorted

**[HW]** A second trial reused a fixed low aperture coordinate
`d0 = 0x00040000` and pointed dword 1 at texture surface `0x01424000`.  A later
1280-wide trial kept both addresses low, using dword 1 `0x00ca4000`.  Both
changed root pixels.  In both cases the temporary entry was appended after
entries with larger dword-0 keys, contrary to the vendor insertion algorithm:

```
slot 0  { d0=0x00000000, ... }
slot 1  { d0=0x00100000, d1=0x00700000, ctrl=0x30030500,
          dims=0x010002d0 }                         # root
slot 2  { d0=0x00cc0000, d1=0x00ca0000, ... }
slot 3  { d0=0x00040000, d1=0x00ca4000,
          ctrl=0x10002000, dims=0x00010001 }        # upload
```

The all-low result rules out a high dword-1 address as a necessary condition
for corruption.  More importantly, the common ordering violation means these
tests cannot distinguish dword-1 limits from wrong descriptor selection.  A
valid countertest must insert the upload as slot 1 and shift the root and old
slot 2 to slots 2 and 3.  It should use the canonical type-0 control
`0x10000020`, then vary only one of type, width, and target address per run.

The evidence currently supports two separate statements:

1. **[HW]** a sorted upload at dword 0 `0x01440000` aliases a lower host
   coordinate on this P690; and
2. **[BIN]** every vendor-created bank is an ascending live prefix, whereas
   both low-address native tests appended an out-of-order entry.

The first establishes a P690 dword-0 allocation limit.  The second is the most
direct explanation of the latest all-low corruption.  No present test proves
that high bits of dword 1 fold or recursively translate.

## Required native fix

The complete fix needs a per-device host-aperture manager.  A staged upload
path is needed only if a correctly ordered test later shows that high local
targets cannot be reached directly:

1. Manage dword-0 ranges separately, for example with `drm_mm`.  On P690 keep
   every live range in `[0x00040000, 0x01000000)` and reject an allocation if
   `start + extent` exceeds `0x01000000`.  Slot 0 owns the first 256 KiB.
2. Align starts and allocation extents to 256 KiB and retain the vendor's extra
   256-KiB guard/gap.  Never allow wraparound or overlapping live descriptors.
3. Let the descriptor manager own both slot and range.  Insert by ascending
   dword-0 key and move later descriptors exactly as the vendor does.  Program
   dwords 1--3, issue the required ordering barrier, then publish dword 0.
   Clear and read back dword 0 before recycling the range or destination
   surface.
4. Serialize the manager with CPU aperture access, submit, and reset/teardown
   using one documented lock order.  A range backing a live VMA cannot move.
5. Use the vendor-shaped raw entry `ctrl=0x10000020`, `dims=0x00010001` for
   the first type-0 upload test.  Track the byte extent in software, since it is
   absent from the hardware entry.  Do not encode chunk length as type-0 width.
6. Test high dword-1 targets only after ordering is correct.  If they fail with
   a low, sorted dword-0 entry, constrain direct targets or use the staged copy
   below.
7. Protect dual-use BAR coordinates from raw physical mappings.  Either reserve
   the low aperture arena from TTM at probe or prove that no raw CPU mapping can
   address it while translated descriptors are live.

For the immediate serialized direct path, reserve one permanent low upload
window before cursor, scanout, and texture allocations: 256 KiB of aperture
payload plus the vendor's 256-KiB guard.  Insert it at the correct ordered slot
instead of appending it.  Reuse the window under the submit/aperture locks.

If a correctly ordered high-target test fails, first test the class-3 2D path:
allocate a 32-bit coherent system buffer, fill its linear rows, bind its
`dma_bus | 3` base and matching type-1 format/pitch as source slot 2, bind the
local texture view's **low dword-0 coordinate** as destination slot 0, then
issue `DWGCTL=0x2c2`.  The class-3 source bypasses the host-aperture descriptor
bank, but the class-0 local destination still requires a live low-coordinate
translation to its physical dword-1 target.  This avoids a low local staging
BO.  Fence the coherent buffer before reusing it.  Keep this path only if the
destination texture layout works through the new low translation; the live
row-402 result already proves system-to-local class-3 reads.

The host descriptor and both BITBLT surface configurations must describe the
same byte-preserving view.  For the smallest proof, copy one 8192-byte row as
2048 32-bit pixels:

```
descriptor d2 = dst SURF cfg = src SURF cfg = 0x30010800
descriptor d3 = 0x00010001
LEN           = 0x00010800
```

The canonical raw type-0 descriptor `0x10000020` above is for a CPU-window
test and must not be mixed with this 32-bpp type-1 BITBLT view.  Keep the
descriptor published and correctly sorted until the DMA completion fence has
retired; invalidate it only afterward.

If that test fails, reserve a low local staging surface, CPU-copy into it
through the low host window, revoke the aperture, then submit a local-to-local
GPU copy to the high texture destination.  Reuse both allocations under the
same locks.

The existing 2D engine supplies a concrete native copy mechanism.  Bind the
low staging source and high destination in surface slots, set `PLNWT` at
`0x1324` to `0xffffffff`, and issue GXcopy BITBLT:

```
DWGCTL = (dstSlot << 10) | (srcSlot << 8) | 0x0c2   # 0x1300
SRC    = (ysrc << 16) | xsrc                         # 0x1304
DSTXY  = (ydst << 16) | xdst                         # 0x1308
LEN    = (height << 16) | width                      # 0x138c, fires
```

The equivalent DMA tag is `0xe3c2c1c0`, followed by those four values.  This
is the vendor's established screen-to-screen copy opcode, not its texture
upload transport.  Before adopting it, test that the P690 2D source and
destination surface configurations reproduce the texture's selected linear
or tiled layout.  If a tiled texture cannot be a 2D destination directly,
write the recovered texture swizzle into the low staging image and copy a
layout-compatible byte rectangle, or initially allocate aperture-uploadable
textures from a dedicated low-address heap.

That staged fix would make high texture destinations reachable without putting
a high address in dword 1.  A simpler fallback policy is to allocate all
CPU-aperture-uploaded texture BOs below 16 MiB, but available low VRAM is
limited by scanout and will not scale to Quake 3 texture sets.

Neither choice cures existing high-coordinate dumb mappings.  The original
sorted high-coordinate failure proves the general mmap path also needs the
capped allocator.  A conservative design can reserve the low 16-MiB host arena
at probe and suballocate descriptor coordinates within it, while physical
surfaces outside that range use GPU copies if the ordered-target experiment
shows that direct access is unsafe.
