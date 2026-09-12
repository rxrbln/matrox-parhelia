Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 mode-7 direct fetch from class-3 DMA memory

This note narrows one question left by the class-0 mode-7 screen test: what
the recovered `0x9f` packet requires when both vertex and index data live in
driver-owned PCI/PCIe DMA memory.  It separates facts proved by the saved
Linux and Windows drivers from the proposed, deliberately sealed diagnostic.
There is **no successful P690 class-3 mode-7 hardware result yet**.  This is a
packet and lifetime design for that test, not a feature to expose to Mesa.

## Result

For a four-byte index stream the vendor emits this three-dword packet:

```
0x9f000001                         // mode 7, normal layout
index_bus + index_offset            // low two bits clear: inclusive start
index_bus + index_offset + 4*n | 3  // exclusive end, class 3
```

The low memory-class bits are **not an index count, a base-vertex value, or an
index-end flag of their own**.  They are the address-space tag attached to the
final (exclusive) byte address.  The start operand is deliberately untagged.
The vertex fetch stream carries the same class independently:

```
STREAM0 (0x1000) = vertex_bus + vertex_offset | 3
STRIDE0 (0x1040) = vertex_stride      // low 16 bits; paired with stream 1
```

Thus a one-buffer test may place vertices at byte zero and indices at an
aligned later byte in the same contiguous allocation.  It must still form the
two address domains independently: `STREAM0` is a tagged base, while the
`0x9f` operands are a tagged byte interval.

## Linux vendor proof

The source-level allocator establishes the class-3 backing contract without
an inferred GPU virtual address:

* `kernel/src/mtx_mem.c:mtx_mem_alloc_pci()` obtains a physically contiguous
  `__get_free_pages(GFP_KERNEL | __GFP_DMA32, order)` allocation, reserves its
  pages, and zeros it.  It does not build an SG list or a page table.
* `kernel/src/mtx_strintf.c:MtxRegion_Alloc()` records
  `base = __pa(kaddr)` and `busaddr = virt_to_bus(kaddr)` for
  `MTX_MEMTYPE_PCI_LOCKED`.  `MTX_MEMTYPE_AGP` instead records the AGP
  aperture address.  The memory-type enum is in `kernel/src/mtx_mem_type.h`:
  PCI-locked is 1 and AGP is 2.
* `kernel/src/mtx_os.c:OsAllocLockedPages()` selects PCI-locked memory when
  the caller requests contiguous non-AGP pages.  This is the vendor's
  general “hardware may directly access this” route.

The unstripped HAL fixes the mode-7 operand roles precisely.

* `scratchpad/mtxk_r.asm:mtx2661` (`0xbf50..0xc02b`) selects
  `0x1c000000` for element size four at `0xc007`.  It obtains the index
  allocation base, clears its low two bits, adds the byte start offset, and
  returns that value as the first operand.  For the second operand it obtains
  the buffer base again, adds `4 * count`, then ORs in
  `mtx2320(buffer) & 3` at `0xc011..0xc02b`.  No class is added to the start.
* `mtx1443` (`0x5db0..0x5e12`) writes exactly three dwords: a header formed
  from the selected mode plus `0x03000001`, then the two values returned by
  `mtx2661`.  `mtx3249` (`0x5e30..0x5ee9`) emits the same packet and submits
  it.  Neither emitter modifies the two operands after `mtx2661` supplies
  them.
* The generic resolver described in
  `parhelia-vertex-fetch-and-memory-classes.md` §1.1 maps PCI-locked memory
  to class 3 on the measured non-AGP board.  Its stream-base result is
  `busaddr + offset | 3`; the direct-fetch state builder writes that result to
  `0x1000..0x101c`.

This also rules out a tempting but wrong packet form:

```
start = (index_bus + offset) | 3       // wrong: not vendor mode-7 framing
end   = index_bus + offset + bytes     // wrong: loses the memory class
```

## Windows cross-check

`MTXPARD.dll` independently implements the same asymmetric pair.

* `scratchpad/pard.asm:0x12e1c7..0x12e1fa` is the `index_size == 4` branch.
  It stores `0x1c000000`, calls the address resolver, computes
  `base + first_index * 4` for the first return dword, then computes
  `base + byte_start + index_count * 4` and stores it as the second return
  dword.  The resolver's low two bits are retained on that second calculation.
* The two emitters at `0x132920..0x132988` and `0x132990..0x1329ff` reserve
  twelve bytes and write `context_flags | mode | 0x03000001`, then those two
  returned operands.  They do not add a per-index packet class themselves.
* The Windows packet builder's direct-fetch setup at
  `0x12df80..0x12e111` calculates stream floors/limits and records resolved
  stream bases and 16-bit strides.  It agrees with the Linux model that
  ordinal values in the index payload select entries from separately bound
  streams.

The two implementations make it very unlikely that a low-bit class tag means
“last index” or a mode-7 packet modifier.  The tag is produced by memory
resolution and is carried exactly where the packet builder carries a terminal
address.

## What class 3 is — and is not

Class 3 is the Parhelia PCI bus-master route.  On the P690 PCIe target it is a
32-bit DMA address with low bits `3`; it is not a PCIe GART, a GPU page table,
or an AGP aperture.  The vendor kernel has no registration write for such a
buffer.  Conversely, a class-2 test needs a real AGP allocation bound into an
AGP GATT, and cannot be simulated by tagging arbitrary PCIe DMA memory with
`2`.

The kernel may only use `dma_addr_t` returned by its DMA API and must reject
an address above 32 bits.  It must never derive a class-3 operand from a
userspace virtual address, a physical frame number supplied by userspace, or
the address of a shmem/TTM page.

## Cache and publication evidence

There is no `dma_map_*`, `dma_sync_*`, explicit cache-flush helper, or
hardware cache-control packet in the saved Linux allocator/submit path around
PCI-locked vertex data.  The legacy driver used physically contiguous DMA32
pages directly and relied on the platform's coherent x86 PCI DMA.  Its only
visible ordering operation is publishing command data before the DMA
doorbell; it does not establish a distinct index-cache flush sequence.

That does **not** license the modern kernel to use raw `__pa()` or assume
coherency for arbitrary mapped pages.  The correct current-driver analogue is
`dmam_alloc_coherent(&pdev->pdev->dev, size, &bus, GFP_KERNEL)`, after the
driver has called `dma_set_mask_and_coherent(... DMA_BIT_MASK(32))`.  This is
already how its primary ring, upload blocks, and writeback page are allocated.

Required ordering for a coherent class-3 test is therefore:

1. Fill the private vertex/index allocation while it is CPU-owned.
2. Fill the private secondary and primary DMA blocks.
3. Execute `dma_wmb()` immediately before publishing `RING_END` / the
   front-end doorbell.
4. After observing the coherent writeback word, execute `dma_rmb()` before
   inspecting any GPU-written result.

No `dma_sync_single_for_device()` is required for a coherent allocation, and
adding an undocumented Parhelia “cache flush” packet would not reproduce
vendor behaviour.  If a later implementation intentionally changes to a
streaming mapped allocation, it must use DMA-API ownership transitions, not
this coherent-buffer recipe.

## Sealed kernel-only class-3 screen-test design

Do not extend `DRM_IOCTL_PARHELIA_SUBMIT` or add a class-3 relocation.  The
test should be compiled into the DRM driver and selected only with a
read-only module parameter such as `mode7_dma_class3_test=1`; an optional
debugfs trigger may start it, but accepts no addresses, indices, packet bytes,
or handles.  It is safe to leave the parameter off in the ordinary staged
module.

The test owns one persistent coherent allocation for the entire GPU lifetime:

```
struct parhelia_mode7_dma_test {
        void       *cpu;
        dma_addr_t  bus;       // must fit in u32 and be 4-byte aligned
        u32         bytes;
        u32         vertex_offset;  // 0
        u32         index_offset;   // ALIGN(vertex_bytes, 4)
};
```

It fills a fixed four-vertex, five-dword (20-byte stride) coloured quad and
the fixed six-u32 list `{0, 1, 2, 0, 2, 3}`.  It generates, rather than
accepts, the known screen-test state and all following packets:

```
reglist: STREAM0..7 = {
    lower_32_bits(bus) | 3, 0, 0, 0, 0, 0, 0, 0 }
reglist: STRIDE0..3 = { 20, 0, 0, 0 }
reglist: FETCH_LIMIT = 4; FETCH_FLOOR = 0
reglist: descriptor index = 0; known five-dword descriptors; terminator
reglist: known triangle primitive state
draw:    0x9f000001,
         lower_32_bits(bus) + index_offset,
         lower_32_bits(bus) + index_offset + 6 * 4 | 3
```

The full eight-base/four-packed-stride update is intentional.  It follows the
vendor direct-fetch shape and clears stale stream state; it is not merely the
old `0x97` stream-0 placeholder.  The secondary and primary must remain
driver-owned coherent allocations too.  The allocation must not be freed,
reused, remapped to userspace, or overwritten until the final writeback fence
has retired.  On timeout, it joins the existing quarantine list before PCI
bus mastering is disabled.

Before the doorbell, reject any failed invariant: P690 only; active raw
aperture owner; 32-bit and four-byte-aligned DMA address; addition overflow;
the full test allocation under the 32-bit mask; exactly six valid ordinals;
and every fetch byte within the fixed vertex portion.  Compare the tiled
colour result against the existing `0x97` reference only after the fence.

This diagnostic distinguishes the remaining hypotheses cleanly:

* a successful result proves class-3 mode-7 memory framing and rules out the
  class-0 local index copy as the cause of the prior timeout;
* a timeout after the full vendor-shaped state proves that another mode-7
  execution prerequisite remains, without granting userspace a raw-DMA ABI;
* no result should be generalized to AGP class 2 or ordinary Gallium indexed
  rendering until the exact test is repeatable and its asynchronous lifetime
  model is implemented.

## Hardware result (2026-09-10)

The sealed DMA32 diagnostic was implemented with `STREAM0 = bus + offset | 3`,
an untagged mode-7 start operand, and a class-3-tagged exclusive end. The
kernel copied the four vertices and six u32 ordinals into its coherent submit
buffer, validated every ordinal before patching either address, and kept the
public capability test-gated.

The P690 nevertheless stalled at the same final indexed CALL as both earlier
class-0 attempts:

```
submit timeout at CALL 31/31:
BM=03000000 prim=355c8274/355c827b sec=00000000/00000000
fence=0048a360 want=0048a368; wedged
DMA status450=fbefdf0f want=00000008 writeback=00000007
DWGCTL=00a60ec4 908=638fa047 924=00201002
```

The target was reset and restored with `mode7_screen_test=N` before Xorg and
Q3 restarted. This eliminates the direct-fetch memory class as the sole
missing condition. The DMA relocation remains test-gated and no mode-7 path
is available to normal Mesa or Q3.
