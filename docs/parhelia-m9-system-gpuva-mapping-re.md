# M9148 system-memory GPU virtual mapping

This note closes the address-allocation and page-entry rules needed to bind
the first triangle's system-memory vertex and program arenas.  It distinguishes
three values which the saved ABI returns for one resource: CPU mapping cookie,
host physical/DMA pages, and GPU virtual address.  They are independent.

Run the offline checker from the `xf86-video-parhelia` root:

```sh
python3 tools/m9-system-gpuva-evidence.py
```

It hash-pins the canonical Linux proprietary core, saved Windows display and
miniport images, two allocation captures, and the first-triangle command
capture.  It also pins the exact Linux allocation, page-entry construction,
publication, and teardown slices.  It never opens a device.

## Closed result

`0x40f52000` and `0x40c3a000` are GPU virtual addresses allocated from the
vendor's system domain.  They are neither raw physical/bus addresses nor
`0x40000000 | physical` transforms.

The direct-transform hypothesis is falsified by the current clean-room DMA
observations:

| role | clean-room DMA | captured vendor GPU address | `40000000 | DMA` |
|---|---:|---:|---:|
| canonical vertex arena | `02944000` | `40f52000` | `42944000` |
| program/resource arena | `02b00000` | `40c3a000` | `42b00000` |

It is also structurally falsified by the saved allocator.  `m9x147` asks
`m9x953` for an address range and constructs an `m9x1046` suballocator.
`m9x953` computes:

```c
gpu_base = manager->page_base_dc << 12;
gpu_size = ((selected_descriptor->field_30 >> 2)
            - manager->page_base_dc) << 12;
```

The captured system range begins in the `0x40000000` domain.  Allocation is
4-KiB granular even when a resource advertises a smaller internal alignment.
The recorded `0x80` alignment of the `0x20080` program arena describes
suballocation and payload layout; its backing map is page-rounded to
`0x21000`.

Two simultaneous captured clients further demonstrate allocator identity:

| client | GPU address | mmap cookie | usable extent |
|---:|---:|---:|---:|
| 0 | `40c3a000` | `18cd1000` | `20080` |
| 1 | `41105000` | `18f33000` | `20080` |

The mmap cookie is an open-wrapper lookup key.  It is not passed to the GPU.

## Exact Linux allocation path

The first-triangle system arenas use internal mode 1:

```text
m9x1030 mode 1
  primary: m9x1046 GPU-virtual-range suballocation
  backing: m9x25 -> service +f0 -> m9x976
           -> KernelClientAllocScatter
  bind:    m9x887(address manager, mapping descriptor, GPU allocation)
```

The open wrapper allocates each backing page with
`get_zeroed_page(GFP_KERNEL | __GFP_DMA32)`.  It writes the physical page
frame numbers as `u64` values.  `m9x25` requests an eight-byte private prefix,
so the object returned to the proprietary core is:

```text
vm_base + 0x00: private u64
vm_base + 0x08: backing PFN 0
vm_base + 0x10: backing PFN 1
...
```

The wrapper also returns `__pa(first_page)`, but `m9x1030` does not derive the
GPU address from it.  It keeps the PFN table and the separately allocated GPU
interval, then passes both to `m9x887`.

The old wrapper does not use the Linux DMA API and assumes CPU physical equals
device-visible bus address.  A modern driver must use DMA mappings.  For the
first implementation, set both streaming and coherent masks to 32 bits and
reject any mapped page above `U32_MAX`; this reproduces the tested
`__GFP_DMA32` constraint.  There is no 30-bit constraint.  The `0x40000000`
GPU base belongs to the virtual allocator and consumes no host-address bit.

## Page-entry format

For each 4-KiB GPU virtual page, `m9x887` reads one `u64` PFN from
`vm_base + 8 + page_index*8`, converts it to a physical address, and creates a
16-byte record:

```text
dword 0: packed physical page plus low flags
dword 1: 0
dword 2: 0
dword 3: 0
```

The exact first dword is:

```c
pte = (physical & 0xfffff000)
    | ((physical >> 28) & 0x00000ff0)
    | flags_low_nibble;
```

Decoding is:

```c
valid = pte & 8;
physical = (pte & 0xfffff000)
         | (((pte >> 4) & 0xff) << 32);
```

Thus the format can name 40-bit physical pages.  The saved Linux wrapper still
chooses DMA32 pages, so 40-bit hardware acceptance is not a reason to enable a
wider DMA mask before hardware validation.

For an ordinary newly allocated mode-1 scatter resource, `m9x1030` sets
mapping-descriptor bit 4, leaves bit 5 clear because resource `+0x30` was
initialized to one, and normally leaves bit 7 clear.  `m9x887` converts these
to low nibble `0xa`:

```text
bit 3 = 1   valid
bit 1 = 1   attribute selected by mapping-descriptor bit 4
bit 0 = 0   attribute selected by mapping-descriptor bit 5
```

The separately observed low nibble `8` in index-78 PTE-shaped records belongs
to another mapping form and must not replace `0xa` for a fresh mode-1 scatter
binding.

`m9x887` uses `m9x318` to select the mapping-domain synchronization record.
It selects the 16-byte table resource through the address manager's current
table index.  Its GPU upload destination is:

```c
table_gpu_base + 16 * (gpu_va >> 12)
```

After chunks are copied it writes `314=1` when the copy helper reports that a
kick is needed, and emits the `31c` invalidation sequence derived from the
address-manager masks.  A companion CPU-copy translation path uses the same
first-dword grammar in a compact four-byte-per-page view.  `m9x715` clears
that compact view during resource destruction.  These are two table views;
their strides must not be conflated.

## Address low tags

Low address tags are field-specific.  They do not alter the GPU-to-host page
translation and are unrelated to the PTE low nibble.

| consumer | stored address | rule |
|---|---|---|
| vertex source index `00` | `40f52001` | GPU VA plus byte offset, then `|1` |
| linked-resource selector `6c` | e.g. `40c3a301` | GPU VA plus record offset, then `|1` |
| system constant ranges `8d/8e` | e.g. `40c3a011`, `40c3a0c1` | GPU VA endpoint, then `|1` |
| primary command start `400` | raw coherent DMA | `start | 2` |
| primary command end `404` | raw coherent DMA | untagged exclusive end |
| completion index `1b` / `106c` | raw coherent DMA | `fence_dma | 2` |
| secondary `2c/2d` start | class-1 compact-PTE GPUVA plus offset | `start | 1` |
| secondary `2c/2d` end | class-1 compact-PTE GPUVA plus offset | `end | 2` |

The first three rows and the class-1 secondary are translated GPU virtual
resources.  Primary-command and completion storage use raw DMA addresses and
have no GPUVA allocation or PTE binding.  The secondary field identity is
proved in `parhelia-m9-secondary-address-domain-audit.md`.

## Binding and lifetime rule for the clean-room driver

For each system GEM object needed by rendering:

1. Allocate a page-aligned interval from the M9 system GPU-VA range.  Keep
   `0x40000000..0x4fffffff` reserved for this purpose and do not calculate the
   address from the host DMA value.
2. Pin each backing page and DMA-map it with a 32-bit streaming mask.  Record
   the returned DMA address, not `page_to_phys()`.  Require 4-KiB page
   alignment.
3. Build one 16-byte record per page with PTE low nibble `0xa` for the normal
   first-triangle scatter mapping.
4. Publish each record at `table_gpu_base + 16*(gpu_va>>12)`, then perform the
   matching `31c` translation invalidation before commands can refer to the
   object.
5. Put `gpu_va|1`, with the appropriate byte/record offset, into vertex,
   selector-6c, and system-range packet fields.
6. Keep the pages, DMA mappings, GPU-VA interval, and table entries alive until
   every command and secondary buffer which refers to them has retired.  On
   teardown, first quiesce those users, invalidate/clear the mapping, complete
   the translation flush, then unmap/free the backing and release the GPU-VA
   interval.

The saved vendor destructor waits for resource use to stop before freeing its
scatter pages and clearing allocator/table state.  The order above keeps the
mapping valid through retirement and removes it before returning memory to the
OS.

## Remaining hardware join

Static analysis closes the allocation split, PTE packing, strides, normal
flags, field tags, and DMA width used by the tested Linux stack.  One item is
still not safe to guess: the concrete first-M9148 mapping-table object's GPU
base and its complete hardware publication tuple.

`m9x421` programs `360/364`, `f64/f68`, the selected primary windows, and the
`e6=40000000` transition from address-manager descriptors.  `m9x887` then
uses the selected table object's `+0x48` GPU base and emits `31c` invalidation.
The saved capture has not joined those descriptor values to a cold clean-room
allocation.  Therefore the implementation rule above is complete once the
existing pending `360/364` and `f64/f68` capture identifies the table base and
range.  Substituting the vertex/program DMA addresses, the `0x40000000`
virtual base, or the local `0x10000000` window for that table object is not
supported by the evidence.

The saved Windows boundary is consistent but not stronger: `MxMM.sys` imports
the MDL page-allocation/locking and physical-address APIs, while `MxMD.dll`
independently emits the captured secondary start/end tags.  No Windows static
path establishes a direct `0x40000000 | physical` mapping, and none is needed
after the canonical Linux allocator and PTE builder close the relationship.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
