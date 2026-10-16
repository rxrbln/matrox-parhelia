# Ordinary FAA output CPU cache policy

This evidence closes the CPU cache **call arguments** for the bounded internal
`ac1f0` ordinary-output request on the retained normal FAA graph under the
source-controlled `MmFlags=0x101f8` profile. It supplies no GPU reply and does
not establish public Lock admission, a returned CPU copy, or the cause of the
black native image.

The same run retains 34 live raw-MDL SYSTEM GPU bindings with descriptor flags
`0x30`, their exact PFNs, the actual `23306c` allocation requests/replies, and
actual `MmMapLockedPagesSpecifyCache` calls. All 34 use kernel mode `0`, cache
argument `2`, and priority `0x20`. The ordinary output joins its returned
backing GPU allocation, unique MDL object and CPU alias directly. Its real
request is extent `0x50000`, selector word `0`; its real reply returns the same
extent and the retained MDL/CPU address. It has 80 mapped pages, compact PTE
nibble `b`, and no source-known pixels. The separate typed completion proxy
is not a raw-MDL row and does not acquire this CPU cache policy.

The cache enum is [`MmWriteCombined` value 2](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_memory_caching_type).
The source is `MxMM.sys`, not `MxMD.dll`: handler `7203d` reads the second
request word; `720b3..720c3` computes `(word == 0) + 1` as its cache argument.
The actual wrapper `2482e..24855` forwards that argument to IAT slot `45b28`,
whose imported symbol is `MmMapLockedPagesSpecifyCache`. The checker executes
only those original selector instructions on three caller-owned request
controls: zero selects `2`; `1` and `0x10000` select `1`. It stops before the
helper call, without executing an OS map, GPU input or backing-memory read.

Capture and checker:

- `tools/m9emu/m9_faa_output_cache_policy.py` passively retains the actual
  OS-memory call ledger and joins each mapping to its same-run IO/MDL/PFN
  owner. Its `Boundary` observer stops the real continued CPU-copy call stack
  before the first completion read at `4e2fc`, common-buffer offset `0x20`.
- `captures/m9128-device-inputs-20261006/normal-faa-output-cache-{policy,source,contract}-101f8.json`
  contain the observer proof and hash-joined source/controller records.
- `tools/m9emu/check-faa-output-cache-policy.py` verifies image/instruction,
  observer, source-frame, input and controller hashes; the 34 actual request,
  map, MDL/PFN and compact-PTE joins; the unique ordinary output; the actual
  selector controls; and the strict unexecuted GPU-read boundary.

The host allocates synthetic OS pages and records the vendor's real cache
argument. It does not emulate physical CPU caching. The mapping ledger is
therefore source evidence for the requested policy, not a hardware cache-type
observation.

## Linux allocation distinction

Local Linux 7.2 sources show `dma_alloc_wc` adding
`DMA_ATTR_WRITE_COMBINE` before `dma_alloc_attrs`. That attribute alone does
not establish WC for this x86 coherent allocation: `dma_pgprot` returns its
original protection immediately for coherent devices; `dma_direct_alloc`
returns `page_address` for its coherent low-memory path without applying WC.
The IOMMU coherent path also obtains protection through `dma_pgprot`. The
[DMA attributes documentation](https://www.kernel.org/doc/html/latest/core-api/dma-attributes.html)
allows an architecture to ignore unsupported attributes. Existing use of
`dma_alloc_wc` elsewhere in the driver is not a proof of the private owner's
actual cache type.

The archived target boot disables its IOMMU and publishes a PAT table with WC
available. Those boot records still do not prove any particular owner's PTE
attributes. A native candidate must inspect each live page before relying on
WB or WC. CPU-alias DMA hashes proving zero bytes do not independently prove
zero DRAM bytes or identify a coherence failure.

The bounded candidate preserves the existing coherent DMA allocation, DMA
bus address and compact/companion policy. Only the generated private SYSTEM
`flags30` owners covered by the above source rows select WC preparation.
Before SYSTEM publication or source-byte upload it requires an aligned,
valid direct CPU alias, supported PAT, and verified WB attributes across the
whole owned allocation. It uses x86 `set_memory_wc`, whose documented API
handles cache/TLB transitions and alias consistency, then verifies WC page
attributes. Unsupported aliases and attribute failures refuse preparation.
The normal DMA path and shared completion-page alias remain separate.

Every attempted attribute change retains restoration ownership, including a
partial failed change. Teardown restores and verifies WB before returning any
allocation to `dma_free_coherent`. If restoration fails, the diagnostic freezes
and retains the owners; it must not return wrongly typed pages to the allocator.
After a submitted attempt, attribute restoration requires PCI bus mastering
stopped. No manual cache flush is performed after GPU retirement. This closes
a source/native CPU mapping contract; correct pixels require a separate
physical result.

## Physical qualification

Canonical kernel20df96d verifies all five live native SYSTEM owner mappings
WB->WC before publication on M9128 boot9666bcd7. The stream retires1d0;
the complete output remains zero through the verified WC CPU alias and the
image testFAILs. See [actual logs and image](../tests/m9-faa-wc-20261006/README.md).
This closes the CPU attribute contract for this bounded run; it does not
identify the black-image cause or prove FAA rendering. The fix is retained.
