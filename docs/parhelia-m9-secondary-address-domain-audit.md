# M9148 secondary BmBuffer address-domain audit

> **Correction (2026-09-15):** the bind sequence is `31c=f,1,0` (inside
> `m9x328`) followed by `314=1` (`.text+0x37a4b`, after `m9x1030` returns),
> not `314` then `31c`.  See
> `parhelia-m9-cold-transition-submit-decode.md` section 2.

The Linux vendor's class-1 BmBuffer uses a compact-PTE-mapped system GPU
virtual address in index `2c/2d`.  Its physical address, returned to userspace
as the mmap cookie, is a different field and never enters the secondary range
publisher.  Recheck the call graph, exact instruction slices, runtime ioctl
results, mmap joins, and reference descriptor with:

```sh
python3 tools/m9-secondary-address-domain-evidence.py
```

Evidence is from the shipped Linux core `KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`,
the open wrapper sources beside it, and preserved runtime archive
`raw-preserve.tgz`, SHA-256
`49859ba73319efd6d967e2a873aa6508b1de7eccfeb026508cd10bf35317d2c2`.

## Exact implementation rule

Let `G` be the page-aligned class-1 BmBuffer GPUVA allocated in the Linux
`0x40000000..0x4fffffff` system domain.  Publish a logical half-open range
`[a,b)` within extent `N` as:

```text
word 0 = 45452d2c
word 1 = (G + 8 + a) | 1
word 2 = (G + 8 + min(b, N - 8)) | 2
```

The first eight bytes are private BmBuffer framing.  The end is the exclusive
logical boundary; it is not decremented.  Builders which keep absolute framed
bounds can equivalently add `G` to `{8+a,8+b}`.  For example, `G=0x40000000`,
`N=0x8000`, and `[0,0x268)` gives:

```text
45452d2c 40000009 40000272
```

Allocate and bind the secondary exactly like other system rendering
resources: assign a system GPUVA, write its compact PTEs, issue the normal
post-bind translation invalidation, and keep both backing and GPUVA alive
until the primary call retires.  The vendor completes each mode-1 allocation
independently: publish that allocation's PTEs, write `31c=f,1,0`, then write
`314=1` before constructing the next allocation.  There is no additional
secondary-specific invalidation in `m9x830`.

The cold-start address-manager setup ends by writing `MMIO[0x31c]=0xc`, then
`MMIO[0x31c]=1`, and reading `MMIO[0x31c]` once after the second write to
drain posted MMIO before the first command stream.  For each target bind,
write its PTEs, execute the memory barrier, write `MMIO[0x31c]=0xf`, then `1`,
then `0`, read `MMIO[0x31c]` after the third write, and finally write
`MMIO[0x314]=1`.  No readback is required between the packed `31c` writes.
The `0,2` sequence belongs to the optional 16-byte companion-table path,
which target `MmFlags=0x100e8` disables.

`m9x1039` at `0x2d800..0x2d828` reads the current primary's class from
`BmBuffer+0x34` and writes `MMIO[0x31c]=0` only when that class is zero.  The
mode-2 allocation path below sets the class to 2, so it skips that write.
The compact bind has already ended in state zero; skipping another zero is
still the byte-exact class-2 submit rule.

## Allocation path and field identity

The returned buffer and the buffer grown by the pool are the same resource
class:

```text
m9x425 +0x72 -> m9x743
m9x743 +0x122 -> m9x306
m9x306 +0x11f -> m9x810
```

`m9x425` passes the pool at object `+0x68` to `m9x743`.  `m9x83` reaches the
same pool through interface `+0x68`, then traverses the pool list at pool
`+0x68`.  `m9x306` constructs each resource with `+0x44=0`, `+0x48=0`, and
calls `m9x810` to allocate its backing.

At `m9x810` `0x38270..0x3828c`, every successful mode copies:

```text
BmBuffer+0x44 = backing_request+0x1cc
BmBuffer+0x40 = backing_request+0x1f0
mode           = backing_request+0x230
```

The mode-1 branch at `0x38320..0x3839c` is the only branch which fills
`BmBuffer+0x48`.  It sets `BmBuffer+0x30` from request `+0x1b4`, maps a CPU
address at `+0x50`, sets `+0x58 = +0x44 - +0x50`, and stores the backing
page/mmap token at `+0x48`.  Final code copies `+0x30` to public class field
`+0x34`.

`m9x1215` at `0x130d2..0x13100` stores the underlying allocation object at
request `+0x228`, reads that allocation's `+0x48`, and publishes it as request
`+0x1cc`.  The allocation identity and compact mapper prove this first value
is the command-visible system GPUVA.  It is the address copied to
`BmBuffer+0x44`.

`m9x83` returns `BmBuffer+0x34` as the class.  For class 1 it returns
`BmBuffer+0x48` as the userspace mmap cookie, falling back to `+0x44` only if
`+0x48` is zero.  This field selection explains why observing the ioctl's
returned address alone previously suggested a raw physical secondary.

Finally, `m9x830` at `0xce40..0xcf40` loads only `BmBuffer+0x44`, adds eight,
adds each logical range offset, and applies the low tags.  It never reads
`BmBuffer+0x48` while forming `2c/2d`.

## Runtime field separation

All complete preserved ExecuteDma outputs return class 1 and extent `0x8000`.
The pool ping-pongs among three observed mmap cookies, including `0x15217000`
and `0x15323000`; each cookie is independently present in a `GET_BM` or
`NEXT_BM` mmap log entry.  These are `BmBuffer+0x48`, not its dispatch address.

The same session contains 25 public mode-1 allocation results with both
fields visible.  Each has a GPUVA in `0x40000000..0x4fffffff` at allocation
output `+8` and a different physical mmap cookie at output `+0x10`.  For
example:

```text
GPUVA 40c3a000, mmap cookie 15391000
GPUVA 40c5b000, mmap cookie 1527f000
GPUVA 40c7c000, mmap cookie 16a98000
```

The open Linux wrapper confirms the cookie side: locked/scatter allocation
returns `region->base`, initialized from the host physical mapping, and mmap
finds a region by that page offset.  The compact mapper consumes the separate
GPUVA side.

## Comparison with requester mode 2

The common `m9x810` code still copies request `+0x1cc` to resource `+0x44`
before switching on the backing mode.  The mode-2 branch at
`0x383a0..0x383cc` sets class `+0x30=2`, establishes CPU base `+0x50` and
translation `+0x58`, and leaves the constructor-zeroed `+0x48` unchanged.
Thus `+0x44` always means the GPU transport address, while `+0x48` is an
optional userspace mapping token and must never be substituted for it.

On the clean-room target, a class-2 raw-DMA secondary has separately retired
using its coherent DMA address and class-2 framing.  After kernel `b788cc4`, a
clean boot also retired the direct primary no-op at target `0x10`, but the
canonical class-1 secondary at GPUVA `0x40024000` timed out at target `0x20`
despite its expected leaf PTE.  The class-2 result therefore proves indirect
CALL/framing but does not generalize to class-1 BmBuffers: class 1 is the
mapped system-GPUVA path proven above.  Address class and low tags are
field-specific.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
