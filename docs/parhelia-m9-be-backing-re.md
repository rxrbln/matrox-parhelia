# M9148 first-triangle BE backing

This note pins the private resource selected at raster command index `be` for
the bounded first triangle.  The reproducible checker is
`tools/m9-be-backing-evidence.py`.  It hashes every input before interpreting
it: Linux M9 package build 98, Windows `MxMD.dll` 2.10.00.105, the saved
M9148 single-triangle capture, and the fixed-function raw archive from
2026-09-14.

## Result and priority

There is no P0 or P1 mismatch in the current kernel diagnostic.  Its 1-MiB
local BO, 4-KiB alignment, direct base binding, zero fill, and device-context
lifetime are safe for the bounded first triangle.  Size, placement, and
binding match the vendor contract.  Zero fill is a conservative initialization
policy; the saved M9148 evidence does not establish that the vendor allocator
also returned zero-filled storage.

The future multi-client boundary is explicit: the vendor owns one BE resource
per 3D context.  A userspace context implementation must not share the current
device-wide diagnostic BO between mutually untrusted or independently retiring
contexts.

## Extent, geometry, and alignment

Both vendor binaries derive the two context-private temporary-resource sizes
from `TempBufferSize`, whose default is `0x400` pages.  Linux `m9x5653` stores
the derived byte counts directly:

```text
primary temporary resource = 0x400 << 12 = 0x400000 bytes
BE temporary resource      = 0x400000 / 4 = 0x100000 bytes
```

The independent Windows path is more descriptive.  Snapshot constructor
`0xbe425` computes the same `0x400000` and `0x100000` values.  BE factory
`0x75ff0` turns the latter into a type/format `0x0a` resource with:

```text
elements = ceil(0x100000 / 4) = 0x40000
width    = 0x400 32-bit elements
height   = ceil(0x40000 / 0x400) = 0x100
depth    = 1
levels   = 1
```

The resulting logical extent is exactly `0x400 * 0x100 * 4 = 0x100000`
bytes.  Because that size is already page-exact, it needs no tail padding.

The capture closes the actual placement and upper boundary:

```text
BE base                  112c1000
first transient base     113c1000

113c1000 - 112c1000 = 00100000
```

There is no gap between BE and the next private resource.  Its base is
divisible by `0x1000` and not by `0x2000`, so the effective alignment of this
exact allocation is 4 KiB.  The factory contains no request for a stronger
alignment.  The kernel's `SZ_4K` specification therefore reproduces the
observed placement contract.

## Address and use

The first-triangle command contains `be=3` in preprocessing and
`be=112c1000` only in the raster pass.  `0x112c1000` is the selector-returned
resource base without a low-bit tag or displacement.  In the M9148 local
engine domain its byte offset is:

```text
112c1000 - 10000000 = 012c1000
```

Linux cold setup `m9x17919` creates the 1-MiB object from configuration
`+0x88`, retains it at setup `+0xd8`, and installs it through `m9x3202` in
current-state slot `+0x8a`.  Lowerer `m9x11630` requests public resource
selector `0x0d` and copies that view into surface shadow `+0xd0/+0xd8`.
Surface emitter `m9x12791` emits index `be` and registers the resource with
relocation usage `8`.

Windows independently follows the same chain.  Vtable factory `+0x38`
(`0x75ff0`) returns the resource retained at setup `+0x6c`; setter `0x109090`
installs it in active state.  The lowerer at `0x1828c5` asks that object for
selector `0x0d` and writes the returned pair to surface shadow `+0x8c/+0x90`.
The public selector reaches internal view `0x0f` through the resource
interface.

Usage ID `8` proves relocation and job ownership, but its narrow hardware
read/write direction is not named in either pinned binary.  Keep the complete
object read/write and live through raster retirement.

## Constructor and lifetime

The Windows BE factory initializes a resource descriptor, calls the generic
allocator at `0xa3870`, adds a reference, and returns the resource interface.
It performs no BE-specific payload copy, fill, or clear after allocation.
The Linux cold-setup path likewise creates the object and immediately publishes
it through the current-state setter, with no intervening payload initializer.
Thus the constructor policy is: accept whatever initial storage policy the
generic local allocator supplies.

The capture wrapper began after these private context allocations.  All 54
saved allocation results are later client resources, none based at
`0x112c1000`, and no BE aperture image was saved.  No exact M9148 initial byte
image can be recovered from this capture.  The kernel's full zero fill removes
allocator residue and supplies deterministic first use, but it must be
described as a clean-room and information-disclosure policy rather than a
byte-for-byte reproduction of observed vendor initialization.

BE persists across draws.  Windows setup-to-active-state cloning republishes
setup `+0x6c`; context cleanup at `0x11c3ee..0x11c41e` drops its reference,
destroys the resource on the final reference, and clears the slot.  Linux
performs the corresponding reference drop and slot clear at
`0x4615ef..0x461623`.  It is a context-private resource, not a per-draw
transient.

The current kernel diagnostic has one device-owned synthetic context.  It
allocates BE once, reuses the same direct base for each synchronous draw, and
releases local resources only after `pci_clear_master()` during teardown.
That lifetime is equivalent for this singleton diagnostic and prevents the
device from retaining access after release.

## Reproduce and audit

Run the immutable vendor/capture check from the repository root:

```sh
python3 tools/m9-be-backing-evidence.py --self-test
```

Audit the current kernel source without modifying it:

```sh
python3 tools/m9-be-backing-evidence.py --self-test \
  --kernel-source ../src.linux.x86-64-desktop.260909.123559.3725172/\
linux-7.2/drivers/gpu/drm/m9x/m9_triangle.c
```

The kernel audit requires `SZ_1M`, `SZ_4K`, local placement, fill value zero,
the exact-extent allocation branch, direct `m9_triangle_bus()` binding, the
single raster relocation site, and teardown after bus mastering is disabled.
