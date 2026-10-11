# P690 native render-target origin and VRAM-region policy

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The P690 accepts ordinary 32-bpp native colour targets at arbitrary 8-KiB
aligned byte origins.  Direct live tests cleared targets at `0x04002000` and
`0x06002000`, then used a GPU native-to-native BITBLT into the known mapped
anchor; both full 256x256 comparisons passed with 0/65536 mismatches.  The
extra `0x2000` origin therefore survives the 3-D surface path and addresses the
intended storage in both tested 32-MiB regions.

The saved P690 Windows driver requests ordinary layout-2/3 surfaces at an
8-KiB whole-allocation alignment.  The saved Linux X driver goes further: its
emulated surface manager suballocates successive local surfaces from software
heaps and copies each returned byte origin directly into the surface's engine
base.  It creates a separate host-aperture object only when CPU access is
needed.  This is direct vendor evidence for compact native allocations and for
keeping the GPU address distinct from the CPU view.

The Windows and Linux kernel vendor binaries also contain an alternating
32-MiB interval splitter.  That is a real memory-manager policy structure, but
the static call paths audited here do not prove that its even/odd vectors mean
colour versus depth or that every render surface is allocated through them.
Consequently the interval modes must not be exposed as semantic colour/depth
classes without an independent combined colour-plus-depth hardware test.

There is no vendor evidence for an origin-dependent rewrite of `SURF0_BASE` or
`SURF1_BASE`, a placement field in the surface control word, a host-aperture
descriptor needed by 3-D, or an absolute-base input to the native tile XOR.
The render/depth packet receives the allocation's ordinary physical engine
address.  On P690 this still includes the already-known bank-A/bank-B mapping;
the 32-MiB region index introduces no additional address transform.

The proven native-target allocator rule is therefore:

* align the native allocation to `0x2000`;
* provide a distinct non-overlapping local-memory extent; and
* emit the normal bank-encoded physical engine address and the unchanged
  `format << 24 | layout << 16 | pitch_pixels` control.

Keeping an allocation within one 32-MiB interval remains a conservative
bring-up policy while the meaning of the vendor interval lists is unresolved.
It is not an origin requirement: both tested targets began `0x2000` bytes into
their respective intervals.

## Sources and confidence

The binaries used for this audit are:

```
52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a  xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll
5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
8f5ca1d85f1c13fda731c8a0ba28db063ec0e697b7bc5dfff300c1500d564e4f  matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so
```

`MtxPser` is the Windows package whose INF binds PCI device 102b:2539 and names
the Millennium P690.  The Windows findings below are consequently direct P690
evidence.  The unstripped Linux object corroborates the memory-manager design
and exposes the allocator and final address resolver by symbol.

The binary facts and formulas below are static proof.  The live tests first
proved native-to-native GPU BITBLT at arbitrary physical node `0x01002000`,
then proved 3-D clear plus GPU BITBLT readback at `0x04002000` and `0x06002000`.
The latter results are the decisive arbitrary-origin 3-D evidence.

## The 32-MiB constant constructs allocation regions

The P690 Windows function at `mtxpserd.dll:0x3b0e0` reads the capability word
at `[manager+0x3e0]->+0xab50` and chooses a region quantum and mask:

```
if (caps & 0x00070000) {
        Q = 0x02000000;
        mask = 0xfe000000;
} else if (caps & 0x0007fc00) {
        Q = 0x00800000;
        mask = 0xff800000;
} else {
        Q = 0x01000000;
        mask = 0xff000000;
}
```

For an input range with `length` at `+0x08` and `start` at `+0x0c`, its loop
at `0x3b130..0x3b1b6` is:

```c
end = input_start + input_length;
start = input_start;
while (start < end) {
        next = min((start + Q) & -Q, end);
        relative_start = start - input_start;
        length = next - start;
        odd_region = (start & Q) != 0;
        add_interval(relative_start, length,
                     odd_region ? 1 + 2 * policy : 2 * policy);
        start = next;
}
```

The decisive instructions are `test ebx,edx` at `0x3b15a` for alternating
regions and calls to `0xcc360` at `0x3b16e`/`0x3b18a`.  The returned interval
index is appended to one vector at `manager+0x24` for odd regions and another
at `manager+0x14` for even regions.  Callers at `0x3b7e3`, `0x3b8cd`,
`0x3bf13`, and `0x3c003` invoke it while building the memory manager's ranges.

`0xcc360` makes the data structure explicit.  It permits at most 32 records,
stores them at `object+0x30 + index*12`, and writes:

```
record.start = relative_start;
record.end   = relative_start + length;
record.mode  = mode;                         /* 0..3 */
```

The modes are allocation/availability categories.  `0xcbf10` intersects an
operation with one record's start/end and uses `record.mode` to index resource
masks at `device+0x21e8`; `0xcc000` iterates all records and calls it.
`0xcc050` likewise intersects a requested range at `0xcc074..0xcc093`, then
uses the mode at `0xcc095..0xcc0b9` to query availability masks.  Neither path
rewrites an engine address or a surface descriptor.

Thus `0x02000000` is the P690 memory-manager partition size selected by this
capability branch.  It is not the local surface-alignment result.  The records
are generic availability categories; this path alone does not identify either
vector as a colour or depth class.

## The local native-surface alignment is 8 KiB

The P690 layout/alignment selector is `mtxpserd.dll:0xa5330`.  It extracts
layout bits with `descriptor[1] & 0x00070000`.  Layouts 2 and 3 enter the arm at
`0xa5368`.  For the 32-bpp result from `0xa50f0`, the instructions at
`0xa5376..0xa5393` return:

```c
alignment = 0x1000 + (whole_allocation ? 0x1000 : 0);
```

Consequently an ordinary complete 32-bpp native allocation uses `0x2000`,
while the internal per-level calculation can use `0x1000`.  The P690 size
helper at `0xa5590` consumes this alignment, `0xa5670` is its size trampoline,
and `0xa5660` is the alignment trampoline.  The parent mip-chain constructor
passes the whole-allocation flag to `0xa5660` at `0xc688d` and stores its result
in the surface allocation field at `surface+0x24` at `0xc6895`.  See
`parhelia-p690-vendor-mipmap-layout-re.md` for the fully traced P690 chain and
the independently recovered 32-row allocation granularity of layout 2/3.

The surface object therefore carries the 8-KiB alignment into allocation.  The
memory manager chooses the eventual local byte origin independently.

The Windows allocation and attach path preserves that origin.  The surface
manager at `mtxpserd.dll:0x39c50` first selects an allocator table through
`manager+0x358`: the outer selection consumes allocation option `+0x18`, while
the nested class is derived from `option[1] * 26 + option[0]` at
`0x39cb8..0x39cd2`.  This selects a heap/policy object before allocation; it
does not alter the address returned by that object.  The selected allocator's
first virtual method is called at `0x39ce3..0x39ce6` with the complete
allocation options, including the previously computed alignment.

On success the returned `MmBlock *` is passed directly to the common attach
routine at `0x97aa0`.  That routine stores the block at `surface+0x68`, loads
`MmBlock+0x20` at `0x97aaa`, and writes it as the surface engine base at
`surface+0x78` at `0x97aad`.  Its following branches only OR memory-class tag
bits 2 or 3 for non-local storage; they do not transform local origin bits.
This is the Windows counterpart of the Linux X attach path below.

## Linux X suballocates surfaces and keeps CPU mapping separate

The saved Linux X driver's dynamic symbols expose its complete emulated
surface path.  `EmulMmSurfaceManager::AllocateSurfaceBlock(MmSurfAllocOpt*)`
is at `mtx_drv.so:0x76b30`.  For the normal local-surface arm it calls
`MemoryHeap::Allocate` at `0x76bd9`, keeps the returned byte address in `r13d`,
and writes it directly to `MmBlock+0x20` at `0x76bf7`.  It stores the requested
extent at `MmBlock+0x08` and explicitly clears the CPU pointer at
`MmBlock+0x18` at `0x76c09`.

The allocator `MemoryHeap::Allocate(unsigned int *, unsigned int, unsigned
int)` at `0x76740` is a simple bounded head/tail suballocator.  A head
allocation returns the current start and advances it by the requested size; a
tail allocation subtracts the size from the current end and returns the new
end.  It neither rounds the returned address to 32 MiB nor transforms it.
Successive local surfaces therefore occupy ordinary adjacent offsets in the
selected heap.  The surface-size/alignment calculation performed before this
call is what preserves native layout alignment.

The local heap is selected by the allocation usage jump table rooted at
`.rodata:0x110e8c`.  The observed cases select managers embedded at
`EmulMmSurfaceManager+0x40`, `+0x58`, `+0x70`, `+0x88`, or the fixed range at
`+0xa0`; usage 1 additionally rounds the requested size to 1 MiB at
`0x76c69..0x76c7b`.  This is allocator-class selection, not an address encoding
step.

`MmSurface::AttachToBlock(MmBlock*)` at `0x73710` loads `MmBlock+0x20` at
`0x73720`.  For local memory it ORs no class bits and writes that value directly
to the surface engine base at `MmSurface+0x90` at `0x73733..0x73736`.  Other
memory classes can add low tag bits 2 or 3, matching the kernel HAL resolver.

CPU access has a different lifetime and address path.  Depending on surface
type and access flags, attach allocates an `MmHostAperture` object at
`MmSurface+0xa0` beginning at `0x737d3`; it does not replace the engine base
with that descriptor.  `MmSurface::DetachFromBlock()` at `0x738e0` clears the
block association and engine address, then independently releases the host
aperture at `0x73916..0x7392d`.  A raw BAR mapping is consequently not a valid
way to verify an arbitrary high-VRAM target on P690.

The base `MmSurfaceManager::AllocateSurfaceBlock` at `0x76590` follows the same
contract when it delegates storage allocation to the OS: it stores the
returned physical base at `MmBlock+0x20` at `0x7660f`, and the common attach
path consumes it unchanged.

## The Linux HAL has the same splitter and honors requested alignment

The unstripped Linux `mtx_parhelia.o` function `mtx2744` at `0x5f2e0` is the
64-bit counterpart of the P690 Windows function.  It reads capabilities at
`[manager+0x2b0]->+0x2440` and selects exactly the same constants:

```
caps & 0x00070000   -> Q 0x02000000, mask 0xfe000000
caps & 0x0007fc00   -> Q 0x00800000, mask 0xff800000
otherwise           -> Q 0x01000000, mask 0xff000000
```

Its loop at `0x5f32a..0x5f3df` computes the next boundary, calls `mtx1497`,
and appends the returned interval index to separate lists at list-owner
`+0x48` or `+0x28` according to `start & Q`.  `mtx1497` at `0x58400` creates
up to 16 independent `0xa8`-byte interval allocators.  It stores the interval
start/end at descriptor `+0x00/+0x08`; `mtx3897` links a later adjacent
descriptor to the preceding one.  Memory-manager constructors `mtx2157` and
`mtx3736` call `mtx2744` for their input memory ranges.

The allocator entry `mtx2194` at `0x58e30` exposes the request contract:

```
request+0x00  size
request+0x04  alignment
request+0x08  selection/direction flags
```

At `0x58e50..0x58e64` it verifies that `request.alignment` has exactly one bit
set.  It then selects one interval and passes the request's size and alignment
unchanged to `mtx608` (`0x55820`) and the bounded-fit helper `mtx2349`
(`0x558a0`).  Both helpers use the standard power-of-two formula:

```c
candidate = (free_start + alignment - 1) & -alignment;
fits = candidate + size <= free_end;
```

`mtx2194` then splits the chosen free node with `mtx1597` or `mtx2155` and
finalizes its addresses through `mtx952`.  There is no `0x02000000` rounding in
this allocation path.  When a caller selects one of the earlier constructed
interval allocators, its chosen node is already bounded by that interval's
edge.  This establishes how an interval is consumed, but not which surface
usage selects each interval vector.

## Surface registers keep the normal engine address

The Linux HAL's address resolver is `mtx1132` at `0x4c730`:

```c
address = allocation_physical_base + byte_offset;
if (memory_type == 1)
        address |= 2;                 /* system/AGP class */
else if (memory_type == 2)
        address |= 3;                 /* system/PCI class */
return address;                       /* local memory is unchanged */
```

It reads only allocation fields `+0x18/+0x20`; it does not read the region
parity lists, the host-aperture mapper, or a tile-origin parameter.  Normal
bind paths `mtx1450` and `mtx631` call this resolver.

The colour/depth emission preserves the resulting base independently of the
control word.  `mtx2372` copies four base values from drawable state `+0xb28`
to context `+0x144`, and four controls from `+0xb38` to context `+0x154`.
`mtx2954` at `0xf96c..0xf9a9` emits tag `0x8d8c8988` and writes:

```
context+0x144 -> 0x1220 SURF0_BASE
context+0x154 -> 0x1224 SURF0_CFG
context+0x14c -> 0x1230 depth base
context+0x15c -> 0x1234 depth cfg
```

No operation combines the 32-MiB interval index with the cfg word.  This also
means colour and depth do not need matching offsets inside their respective
regions; each has its own complete base.

Host-aperture descriptors are a separate CPU-view mechanism.  Their logical
dword 0 must not be substituted for the physical engine base.  The exact
separate object lifetimes, the physical-base bind chain, and the P690 bank-B
qualification are documented in `parhelia-vendor-submit-surface-addressing-re.md`.

## Native tile XOR is relative to the surface

The live-recovered P690 layout is 64x16 pixels and 4096 bytes per tile.  For a
32-bpp surface its relative byte offset is:

```c
tpr  = pitch_pixels / 64;
tcol = x / 64;
trow = y / 16;
ix   = x % 64;
iy   = y % 16;
x5   = ((ix >> 5) & 1) ^ (trow & 1);

i = (ix & 1) |
    (((ix >> 1) & 1) << 1) |
    (((ix >> 2) & 1) << 2) |
    (((iy >> 1) & 1) << 3) |
    (((ix >> 3) & 1) << 4) |
    (((ix >> 4) & 1) << 5) |
    ((iy & 1) << 6) | (x5 << 7) |
    (((iy >> 2) & 1) << 8) |
    (((iy >> 3) & 1) << 9);

relative = (trow * tpr + tcol) * 4096 + i * 4;
```

The recovered software formula consumes `trow`, which is derived from
surface-relative `y`; no vendor call path supplies a programmable base-XOR
parameter.  Nothing in the static paths requests a tile transform according to
`SURF0_BASE`.  A focused arbitrary-base coordinate-ramp probe remains the way
to exclude an undocumented implicit silicon dependency on base bits.

## Actionable allocation policy

For ordinary P690 32-bpp native surfaces, compact allocation is now safe at
8-KiB granularity.  A conservative implementation can keep each allocation
inside a `Q=0x02000000` interval while retaining multiple placements per
interval:

```c
#define P690_NATIVE_ALIGN  0x00002000u
#define P690_REGION_SIZE   0x02000000u

size = ALIGN(native_size(width, height, pitch), P690_NATIVE_ALIGN);
candidate = ALIGN(free_start, P690_NATIVE_ALIGN);
region_end = ALIGN_DOWN(candidate, P690_REGION_SIZE) + P690_REGION_SIZE;

if (candidate + size <= free_end && candidate + size <= region_end)
        accept(candidate);
else
        retry_at(region_end);
```

Use overflow-safe arithmetic for the end comparisons.  This policy provides
many placements per region instead of one placement per 32 MiB.  A 1920x1080
32-bpp target occupies about 8 MiB after native row rounding, so several such
allocations can fit in a region when other reservations permit.  A normal
maximum-size 2048x2048 target occupies 16 MiB and still fits within one region.
The 32-MiB containment is presently conservative allocator policy, not a
tested hardware requirement.

For each accepted physical placement `P`:

```
SURF base = bank_encoded_engine_address(P)
SURF cfg  = (format << 24) | (layout << 16) | pitch_pixels
```

Do not allocate a host-aperture descriptor merely to render, do not move
origin bits into cfg, and do not change the native tile permutation according
to `P`.  Create a kernel-owned host-aperture view only for CPU map/upload or
readback, and keep its logical address out of submitted surface registers.

## Live arbitrary-origin correction

The first direct tests appeared to fail all 65536 pixels when the target moved
to `0x04002000` or `0x06002000`.  They read those objects through the raw BAR
mapping, however.  P690 host coordinates above the usable raw aperture fold or
drop into another low-VRAM view, so neither the mismatched target read nor the
unchanged poison at an interval anchor described what the GPU had written.

The corrected test kept dimensions, pitch, cfg, state, geometry, and the
`+0x2000` origins unchanged, then copied the completed native target with the
GPU BITBLT into a known anchored object before comparing it.  Both BAND2
physical `0x04002000` and BAND3 physical `0x06002000` passed 0/65536.  This
matches the Linux X attach path exactly: 3-D consumes the allocation byte base,
whereas CPU access requires its own aperture mapping.

A combined colour/depth test is still required before assigning semantic roles
to the alternating 32-MiB lists.  That test should allocate colour and depth at
distinct 8-KiB offsets, clear both, draw overlapping geometry with depth test
and depth write enabled, then GPU-copy each result to independently readable
anchors.  Passing that test permits compact direct per-client targets without
execution-surface save/restore.  It still does not justify calling BAND2
"colour" and BAND3 "depth" unless the swapped placement is compared or vendor
usage-to-list selection is recovered.
