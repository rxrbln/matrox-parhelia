# M9/Phoenix allocation-mode joins

This note pins the proprietary Linux core's six internal allocation modes to
their final allocator objects, and joins the paths that actually cross into
the open kernel wrapper to its public memory types.  Evidence is from
`m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/KernelLibraries.o_shipped`
(SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`)
and the source beside it.  Addresses below are `.text` or `.rodata` offsets in
that relocatable ELF object.

## Final selector table

`m9x1030` at `0x1cfb0` indexes a 6-by-25 table at manager `+0x88` as
`25 * resource_mode + request_type`, then calls method zero of the selected
object (`0x1d088..0x1d0c4`).  `m9x92` at `0x1e4e0` first fills this table with
the zero-return object at manager `+0x08`, but that is only initialization.
After its backing constructors succeed, `0x1e7c1..0x1eb04` overwrites every
entry with these final objects:

| Internal mode | Final selector object | Allocation backend |
|---:|---|---|
| 0 | manager `+0x40`, `m9x603+0x10` | `m9x944`, over the manager `+0x50` wrapper and an `m9x1046` range |
| 1 | manager `+0x10`, selector ID 1 | manager `+0x60`, an `m9x1046` range built by `m9x147` |
| 2 | manager `+0x20`, selector ID 2 | manager `+0x70`, an `m9x984` page allocator built by `m9x941` |
| 3 | manager `+0x30`, selector ID 3 | manager `+0x58`, an `m9x1046` range built by `m9x565` |
| 4 | manager `+0x40`, `m9x603+0x10` | same path as mode 0 |
| 5 | manager `+0x48`, `m9x484+0x10` | `m9x63`, over the manager `+0x68` wrapper and an `m9x1046` range |

The direct selector is `m9x141` at `0x228a0`.  Its selector IDs 1, 2, and 3
choose respectively `argument+0x10`, `argument+0x20`, and `argument+0x08`;
`m9x1030` supplies manager `+0x50` as that argument.  The final joins are
therefore `+0x60`, `+0x70`, and `+0x58`, rather than an inferred ordering of
the constructed objects.

The two specialized selectors are equally explicit.  `m9x944` reads the
wrapper at selector argument `+0x00` (manager `+0x50`), while `m9x63` reads
the wrapper at `+0x18` (manager `+0x68`).  Both eventually call `m9x470` on
the wrapper's `+0x20` backing.  `m9x565` constructs each backing with
`m9x1046` and installs those wrappers at `0x1dc94..0x1dca3` and
`0x1dd46..0x1dd58`.  Mode 0 is consequently a real allocator and does not
mean public `M9X_MEMTYPE_IO == 0`.

## Public Linux memory joins

The `m9x1046` objects are finite address-range allocators.  Their allocation
method is `.rodata+0xca0 -> m9x1057`, which calls `m9x572` and suballocates a
range prepared by `m9x565` or `m9x147`.  It does not call a kernel allocation
service per resource.  Static evidence therefore does not assign modes
0/1/3/4/5 a single public `m9x_memory_type`: their backing provenance is a
property of the range supplied to `m9x1046`, not the internal mode number.

Mode 2 has a complete dynamic-page join:

```text
m9x92 mode 2 row -> selector ID 2 -> manager+0x70
  -> m9x984 vtable method m9x408
  -> m9x1166 @ 0x1a560
  -> m9x781 @ 0x11db0
  -> service vptr + 0xc0
  -> .rodata+0x130 = m9x1072
  -> KernelClientAllocLockedPages
```

The memory class is conditional, not intrinsic to the mode number.  `m9x141`
passes `(request[0x10] >> 5) & 1` as the final argument to the mode-2 backend.
`m9x408` preserves it at its local descriptor `+0x10`; `m9x1166` passes it as
`ecx` to `m9x781`.  At `0x11dc4..0x11de6`, `m9x781` converts a set value to
allocation descriptor `+0x14 == 0x10` and a clear value to `1`.  `m9x1072` at
`0x574d..0x5760` passes `bContiguous = (descriptor[0x14] == 0x10)`.
The open wrapper then selects:

```text
request bit 5 set   -> M9X_MEMTYPE_PCI_LOCKED
request bit 5 clear -> M9X_MEMTYPE_SYS
```

The wrapper's address contract is also exact.  `M9X_MEMTYPE_PCI_LOCKED` uses
`m9x_mem_alloc_pci`, returns `region->base = __pa(kaddr)`, and records
`virt_to_bus(kaddr)`.  `M9X_MEMTYPE_SYS` uses `m9x_mem_alloc` and records no
PCI bus address.

Mode 1 has a separate, conditional secondary backing path after its primary
range allocation.  Only mode 1 reaches `m9x1030` `0x1d227`.  If resource
`+0x40` is nonzero, it uses the caller-provided pointer at `+0x48`.  Otherwise
`0x1d267..0x1d291` calls:

```text
m9x25 -> service vptr + 0xf0
      -> .rodata+0x160 = m9x976
      -> KernelClientAllocScatter
      -> M9X_MEMTYPE_SCATTER
```

The scatter wrapper returns `region->base = __pa(kaddr)` plus the CPU pointer
and VM table.  This is secondary page backing for a mode-1 range allocation;
it does not rename internal mode 1 to the public scatter enum.  Failure or an
undersized scatter result releases the primary allocation at
`0x1d291..0x1d2e6`.

`KernelClientMapLocalLinMemory` is a third independent service.  Service
slot `+0x100` resolves through `.rodata+0x170` to `m9x386` at `0x5910`, which
maps the framebuffer BAR and stores the CPU mapping in the service object at
`+0x30`.  No final mode-table allocation entry calls this slot.  It is thus
the public `M9X_MEMTYPE_IO` aperture mapping boundary, not an allocation mode.

## Address tags are a different namespace

After allocation, `m9x1030` stores the internal mode at resource `+0x230`.
The later resource initializer derives an access/address class at resource
`+0x30` and copies it to `+0x34`.  The requester then indexes the exact table
`{2, 3, 0, 3}` at `.rodata+0x5890` with resource `+0x34` when programming
`MMIO[0x500]`.  This class can depend on allocator output and device
capabilities.  It is neither the six-way allocation mode nor the public
four-way `m9x_memory_type`, so a new driver must keep all three namespaces
separate.

The deterministic companion checker is
`tools/m9-allocation-mode-evidence.py`.  It pins the selector constructors,
vtable joins, OS-service joins, branch sites, and the open wrapper's public
enum/allocator choices.
