# Vendor Q3 command buffering and the remaining submit-rate gap

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope and result

This note compares the saved Matrox Linux and Windows drivers with the current
open Mesa/kernel path for a Q3-style stream of many small, state-changing draws.
It concentrates on user/kernel transitions, command and vertex buffering,
bus-master DMA, fence placement, and presentation.  It changes no driver code
and makes no target changes.

The current measurement, approximately 1,599 DRM submit ioctls per second at
9 FPS, means:

```text
1599 / 9 = 177.67 submit ioctls per rendered frame
1 / 9 / 177.67 = 0.625 ms of frame time per submit transition
```

The three-slot asynchronous queue has removed the old unconditional wait after
every ioctl, but it has not fixed the command granularity.  About 178 validated
submissions, terminal fence sequences, object-reference sets, and syscall
entries are still created for each frame.  The vendor architecture instead
keeps high-level state and draw records in a large mapped userspace buffer and
enters the kernel only when that buffer fills or an explicit flush/end-of-frame
path requires it.  The saved artifacts do not contain a clean timed vendor
frame trace, so they cannot establish an exact vendor ioctls/frame number.  They
do establish that the number is governed by a 135,276-byte record-buffer
threshold and explicit flushes, rather than by the number of draws.

The smallest safe open-driver change has two parts:

1. Keep using the production `PARHELIA_RELOC_DRAW` path, enlarge its
   kernel-owned coherent DMA32 vertex area from 16 KiB to an initial 128 KiB,
   and aggregate complete state+draw streams by default.  This removes the
   present 4,096-dword aggregate vertex ceiling without mapping a raw BAR or
   changing the UAPI.  The kernel copies each aggregate once and patches the
   validated `0x97` ranges to class-3 bus addresses.
2. Add a bounded submit-chain form when the existing 221-packet CALL-plan cap
   is reached.  One ioctl validates a list of ordinary chunks, each still at
   most 221 user packets, queues them in order, and returns one terminal fence.
   The kernel may publish more than one safe primary span internally, but the
   application crosses the user/kernel boundary once and does not wait between
   chunks.

This copies the vendor's batching boundary without exposing unvalidated GPU
commands or physical addresses through a mapped ring.  In particular, the
vendor's 136 KiB allocation must not be used as evidence for a large local-
VRAM `CREATE_VERTEX`: it is a CPU-only `vmalloc` record buffer.  The vendor
copies ordinary system-memory vertices into a distinct DMA32 pushbuffer before
the GPU fetches them.

## Linux ICD: the user-side record buffer

The saved x86-64 ICD is
`matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0`; offsets below
are its disassembly virtual addresses in `scratchpad/libGL64.asm`.

`GLBufferImpl::Alloc` at `0x3579c0` asks the kernel for `0x2106c` bytes.  The
kernel rounds this to a `0x22000`-byte mapping, or 136 KiB.  The allocation and
following mmap are visible in the retained setup capture.  This is the
high-level command-record buffer, separate from the later vertex and GPU
command rings.

The record reservation routine at `0x1c9c50` implements the batching policy:

* `0x1c9c79..0x1c9c8f` computes
  `write + 8 + element_size * count` and compares it with the buffer end.
* `0x1c9d10..0x1c9d18` calls the flush routine only when that reservation no
  longer fits.
* `0x1c9c94..0x1c9c9b` compares the requested opcode with the preceding
  record.  Equal opcodes branch to `0x1c9d30..0x1c9d34`, which adds to the
  preceding record's item count.  A run of state writes therefore has one
  `(opcode,count)` header rather than one record per state call.
* A new record is written at `0x1c9ca1..0x1c9cb0`; its payload size is saved at
  `0x1c9cc5`.
* Commit at `0x1c9ad0..0x1c9adb` advances the write pointer by that saved
  payload size.  It does not enter the kernel.

Flush at `0x1c9be0` sends the entire byte interval in one operation.  The code
loads the current and base pointers at `0x1c9bf7..0x1c9bff`, subtracts them at
`0x1c9c06`, and calls the device virtual method at offset `+0x60` at
`0x1c9c0c`.  After it returns, `0x1c9c0f..0x1c9c28` writes the sentinel
`{0xff,1}` at the base and resets both the write and preceding-record pointers.

There are only seven direct calls to this flush routine in the complete ICD:

```text
0x1b21c6  0x1b2406  0x1ba5a2  0x1c9d13
0x1c9d7c  0x1c9f06  0x35897e
```

`0x1c9d13` is the capacity path above.  `0x35897e` is in the GLX
end-of-frame path and follows a device virtual call at `+0xa8`.  Ordinary draw
emission does not directly call flush.  The other sites are buffer/heap helper
paths and do not turn every GL draw into a syscall.

The render doorbell begins at `0x3589b0`.  It writes the mapped control block
as follows:

```text
0x358a16..0x358a1b  control[0x10]  = 0
0x358a27..0x358a2e  control[0x230] = record-stream length
0x358a35             control[0x00]  = 1
0x358a3b             control[0x08]  = context handle
0x358a61..0x358a6f  ioctl(fd, 0x4621, 0)
```

`0x4621` is a zero-sized `_IO('F',33)` doorbell.  The record bytes do not cross
the syscall argument; they remain in the user-mapped buffer until the kernel
decodes them.  The control page supplies only the start, length, operation, and
context.

The retained Q3 capture has doorbells 0 through 16 before its first recorded
`glFinish` (`captures/ioq3-p690-working-draw.txt:5-74`).  Those operations are
mostly context, heap, and resource initialization, so they are not 17 submits
for one Q3 frame.  The useful evidence is that a doorbell is distinct from the
first explicit GL completion call and carries no copied command argument.

## The 136 KiB allocation: exact allocation, mapping, and address path

The `0x22000` region is sometimes described as a command/vertex allocation.
That description conflates three different buffers.  It is specifically the
CPU command-record stream and is never a GPU vertex source.

The complete creation chain is:

```text
libGL GLBufferImpl::Alloc                       +0x3579c0
  CB+0x000 = 0x05
  CB+0x010 = 0x2106c requested bytes
  CB+0x018 = 4       request type SYS
  ioctl(fd, 0x4621, 0)
    generic control dispatcher mtx3683          .text+0x4de50
      buffer-create handler mtx3112             .text+0x4f9a0
        allocation switch mtx938                .text+0x4c470
          SYS wrapper mtx3185                   .text+0x571f0
            MtxRegion_Alloc(... MTX_MEMTYPE_SYS ...)
              vmalloc(0x22000)
  CB+0x010 = 0x22000 page-rounded bytes
  CB+0x020 = opaque allocation handle
  CB+0x028 = mmap cookie
mmap64(fd, CB+0x028, 0x22000)
```

`mtx938` reads the size at request offset `+0x00` and the request type at
`+0x08`, rounds the size to 4 KiB, and dispatches request type 1 to
`mtx3583+0x56f80` (AGP), type 2 to `mtx586+0x570c0` (PCI-locked DMA32), and
type 4 to `mtx3185+0x571f0` (SYS).  These request numbers are a dispatcher
enum; they are distinct from the internal region-type values stored in the
result object:

| request | allocator result | allocation | GPU class if resolved |
|---:|---|---|---:|
| 1 | AGP | `agp_allocate_memory` plus `agp_bind_memory` | 2 |
| 2 | PCI_LOCKED | contiguous `__get_free_pages(GFP_KERNEL | __GFP_DMA32)` | 3 |
| 3 | unsupported by this dispatcher | failure | n/a |
| 4 | SYS | `vmalloc` | not GPU-addressable |

The vendor region object keeps separate `base`, `busaddr`, and `kaddr`
members.  For SYS, `base = kaddr - VMALLOC_START` is a synthetic cookie made
small enough for 32-bit clients.  `mtx_mmap` requires
`vm_pgoff << PAGE_SHIFT == region->base` exactly and services SYS faults with
`vmalloc_to_page`.  Thus the returned mmap offset is neither a PCI address nor
a framebuffer offset.  The region's kernel-private bus address is never
returned to userspace.

At render time the ICD writes the byte length to `CB+0x230` and rings the same
zero-sized `0x4621` ioctl.  `mtx3683+0x4de50` selects operation 1 and calls the
record interpreter `mtx558+0x44ef0`.  `mtx558` bounds-checks
`CB+0x010 + CB+0x230` against the region size, then parses the mapped bytes on
the CPU.  That consumer path, plus the SYS allocator and `vmalloc_to_page`
mapping, proves the 136 KiB allocation is CPU staging rather than system DMA,
dedicated VRAM, or a BAR aperture.

## Linux ICD: large page-recycled vertex and command rings

After context initialization, the vendor tries request types 1, 2, and 3 for
its vertex ring; all fail on the retained system.  It then succeeds with type
4 SYS.  The exact capture sequence and backing are:

| use | requests | mapped bytes | backing and role |
|---|---:|---:|---|
| command records | `0x2106c`, type 4 | `0x22000` | SYS/`vmalloc`; CPU record stream |
| vertex ring | `0x320010`, types 1, 2, 3 fail; `0x321000`, type 4 succeeds | `0x320000` | SYS/`vmalloc`; CPU vertex staging, 400 8-KiB pages |
| client command ring | `0xa9000`, type 4 | `0xa8000` | SYS/`vmalloc`; CPU/client secondary staging, 84 8-KiB pages |
| texture heap | `0x401000`, type 4 | `0x400000` | SYS/`vmalloc`; CPU texture staging |

The failed type-2 attempt is also structurally incapable of satisfying the
3.2 MiB request: the saved PCI_LOCKED allocator stops at order 9, or 2 MiB.
The successful type-4 fallback does not make the vertex ring directly
GPU-readable.  It makes a large page-recycled CPU staging ring.

The vertex allocator at `libGL64+0x1b2060` uses 8 KiB ownership granules:
addresses are shifted by 13 at `0x1b20b9` and `0x1b20c2`, and the page-tag
array is reached from ring offset `+0x58`.  The unsafe-reuse path compares the
page's generation with the device's issued/completed snapshots at
`device+0xbc84` and the mapped fence state at `device+0xbc78`.  It yields with
`usleep(0)` at `0x1b2113..0x1b211a` only when a page cannot yet be reused.
Capacity and wrap handling are at `0x1b2177..0x1b21a4`; commit stamps the
generation over every page in the committed span at `0x1b21e0`.  Ring reset at
`0x1b2260` initializes all page tags.

The sibling allocator at `0x1b22e0` makes the fixed granularity explicit:
`0x1b22f7..0x1b2306` checks `current + 0x2000` against the end, and
`0x1b23bf..0x1b23df` advances exactly one page after assigning its generation.
Captured draws use matching `0x2000` strides in both the vertex and command
rings.

This is the vendor's important back-pressure rule: wait when reusing storage
whose tag has not retired, rather than after recording or submitting every
draw.  The 3.2 MiB vertex ring also has far more slack than the open driver's
three roughly 21 KiB vertex slots.

For ordinary client arrays, `mtx1017+0x60e0` copies the staged vertices into
the kernel pushbuffer and emits `0x17|flags,start,end` after the copied bytes.
The start operand is the pushbuffer write pointer before the copy and the end
operand is the exclusive pointer after it.  Immediately before that draw,
`mtx1017+0x4bf` re-emits `0x1000 = [[channel+0x58]+0x34]`, the pushbuffer's
class word.  The retained stream fetches vertices at
`0x19dc13f8..0x19dc1434 | 3`, inside the kernel DMA32 pushbuffer whose base is
`0x19dc0000`.  This is direct evidence that the GPU consumes class-3 system
DMA after a CPU copy, not the type-4 user mapping and not local framebuffer
memory.

`mtx1132+0x4c730` is the general address resolver: internal type 1 becomes
class 2, type 2 becomes class 3, and other types would become class 0.  The
last default must not be applied to SYS mappings; the vendor avoids doing so
by taking the copy builder whenever a stream lacks a GPU address.  Treating a
type-4 vmalloc cookie as a class-0 vertex offset in a new driver would turn an
opaque CPU mapping token into an arbitrary framebuffer address.

## Record merging versus hardware packet boundaries

The ICD's adjacent `(opcode,count)` merge saves CPU record headers; it does not
erase hardware draw boundaries.  Consecutive high-level key `0x1b` DRAW items
share one eight-byte key/count header, but `mtx558` still iterates the count and
calls `mtx2196+0x4acb0` for each 40-byte DRAW item.  `mtx2196` translates state,
queues the kernel pushbuffer work, and then queues the referenced client
command-ring interval for that item.

The distinction matters for the open P690 mode-3 path.  An `0x8f` packet is:

```text
header = 0x8f000000 | (2 * ranges - 1)
payload = { first, exclusive_last } repeated `ranges` times
```

The low 16 bits follow the normal `payload_dwords - 1` rule, so the encoding
can describe at most 32,768 ranges.  That format capacity is not permission to
combine Q3 triangles.  The live P690 overlapping-Z24 comparison found that
one `[0,6)` range and two `[0,3),[3,6)` ranges in one `0x8f` packet both
produced 18,336 wrong color pixels and 18,336 wrong depth pixels in the
far-first ordering.  Two separate `0x8f000001` packets matched the two-`0x97`
reference in both orders.  The draw packet is the proven depth-interlock
boundary.

Separate packet headers can still occupy one secondary DMA byte interval.
The fetcher stops each packet at the header-declared length and parses the next
header; a primary CALL only supplies the enclosing start/end interval.  The
safe optimization is therefore:

```text
before: CALL -> [8f000001, 0, 3]
        CALL -> [8f000001, 3, 6]

after:  CALL -> [8f000001, 0, 3,
                 8f000001, 3, 6]
```

This retains the two hardware packet/interlock boundaries while removing one
primary CALL.  Runs must end before a kernel-inserted WARP-stage CALL,
opening/EOB/drain record, or logical state-batch boundary.  The initial kernel
implementation deliberately recognizes only exact three-dword `0x8f000001`
packets; other classes remain one CALL each pending equally direct hardware
evidence.

## Linux kernel: bus-master DMA and producer publication

The saved kernel object is
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`; offsets below are
`.text` offsets in `scratchpad/mtx_parhelia_ko.asm`.

`mtx1937` at `0x32360` is the normal secondary-buffer enqueue routine:

* `0x323ab..0x323bd` gets the persistent primary write pointer from ring object
  offset `+0x60`, reserves 20 bytes, and checks the limit at `+0x70`.
* `0x323ce..0x323e3` appends the five dwords
  `{0x20292820, 0, secondary_start|flags, secondary_end|flags, 0}`.
* `0x323f8..0x3244b` converts the new pointer to its bus address and writes it
  once to MMIO `0x408`, the producer/tail register.

The normal path contains no `0x41c` completion read.  The conditional path at
`0x32473..0x324f0` is selected by a special zero end/flag argument and polls a
bus/FIFO condition; it is not the ordinary CALL append.

`mtx2095` at `0x31fc0` handles command-ring storage turnover.  When multiple
buffers are available it finds a free buffer, emits the switch record beginning
with `0x21272425`, changes the active pointers, and publishes the producer.
Only the one-buffer fallback performs a full bus-master wait.  `mtx2508` at
`0x31880` is the exceptional restart path: it waits for `0x8f0` bit 17 and the
indexed FIFO baseline before programming `0x404` and `0x400`.  The vendor does
not restart the DMA fetcher for every draw or command record.

This proves actual bus-master DMA.  The kernel decodes the high-level mapped
records into kernel-owned secondary buffers, appends secondary CALLs to a
persistent primary ring, and advances `0x408`.  `mtx1937` may publish more than
one lower-level CALL while decoding one user doorbell; the strong batching
claim is one user/kernel transition for the high-level record list, not
necessarily one MMIO tail write for the whole list.

## Fence placement and waits

The Linux fence constructor `mtx3144` starts at `0x31c30`.  Its record at
`0x31c67..0x31ceb` is:

```text
21c0c0c0 00000307 00000004 00000707 c0afafdd
00000000 tagA|1 tagB|1 saved_DWGCTL
```

It advances nonzero tags in steps of eight.  Register `0x12bc` writes the tag;
MMIO `0x41c`, after masking low flag bits, reports retirement.  A four-byte
per-context fence/serial page is separately mapped by the ICD, so completion
state does not require another command-copy ioctl.

Fence records occur at logical synchronization, resource-reuse, finish, and
presentation boundaries.  They are not implicit in every high-level record.
The page-granular vertex allocator checks retirement only before overwriting a
live page.  This is materially different from assigning a terminal hardware
fence and a complete kernel request object to every small Mesa batch.

## Windows cross-check

The saved Windows display driver is
`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`; offsets below are image
relative offsets in `scratchpad/pard.asm`.

The command writer at `0x91840` keeps ordinary register values in the context
shadow at `[context+0x10+register/4]`.  Its indexed-write queue is drained only
after its count exceeds 16 (`0x91ac5..0x91ad7`).  Register `0x1300` low-nibble
operations 6 and 7 bypass the ordinary cache because they are ordering
operations.

The `0x12bc` route masks the supplied tag to its nonblocking value, writes it at
`0x91b09..0x91b10`, and tests bit zero at `0x91b15`.  Only the blocking form
enters the loop at `0x91b40..0x91b54`, which reads `0x41c` and compares it with
the requested tag.  Thus completion waiting is explicitly selected; it is not
the default consequence of a cached register write.

`ValidateState` at `0xc9ac0` conditionally calls dirty-state groups at
`0xc9ad1`, `0xc9add`, `0xc9ae9`, `0xc9afc`, `0xc9b08`, `0xc9b15`, and
`0xc9b22`.  Its surface/state arm path `0xc98b0..0xc9ab0` emits only dirty
surface groups, advances the synchronization tag by `0x10` at
`0xc9a17..0xc9a1d`, builds the arm record at `0xc9a37..0xc9a5c`, optionally
emits DWGCTL, and clears the handled dirty bits at `0xc9aa6..0xc9aae`.
Repeated state is suppressed while ordering-sensitive arm/fence operations
remain explicit.

The primary restart routine at `0x328d0` polls `0x8f0` bit 17 before writing
`0x404` at `0x329c4..0x329d3` and `0x400` at `0x32a01..0x32a15`.  The fence
constructor at `0x32b60` emits the matching `0x21c0c0c0` record and allocates
tags in `0x10` steps.  As in Linux, restart and blocking completion are special
paths around a persistent DMA producer.

## Present queueing is not the 178-per-frame problem

The vendor Linux window swap uses escape `0x10`, handled by `mtx986` at
`0x50980`.  It queues clipped native back-to-primary BITBLTs and, for a nonzero
return tag, appends the four-dword asynchronous completion packet at
`0x50b94..0x50bb1`:

```text
21afddc0 00000707 00000000 tag|1
```

There is no vblank or `0x41c` wait on the normal return path.  Its six-entry tag
history applies back pressure only when full: `0x50bff..0x50cd7` reads
`0x41c & ~7`, rejects reuse of an unretired oldest tag with `-4`, or allocates
the next tag in steps of eight.  The blitter itself tags referenced surfaces at
`mtx1637+0x23e64..0x23e98` and publishes work through `0x408`.

Fullscreen presentation is separate.  Escape `mtx646+0x50680` queues a record
in the 32-entry software flip queue at `mtx3795+0x315b0`.  Producer
`mtx742+0x313e0` places a render-retirement tag in the DMA stream.  The vblank
helper `mtx4025+0x3a590` reads `0x41c & ~7` and selects the new START bank only
after that render tag has retired.  Rendering retirement and retrace latching
are therefore independent gates.

The current open windowed Present path may add one ordered X-server blit per
frame.  Even if it is synchronous, one transition is under 0.6 percent of the
observed approximately 178 render submits per frame.  Removing Present or
vblank ordering cannot account for the Q3 frame rate.  Once render batching is
fixed, Present should enqueue one blit behind the terminal render fence and
let the appropriate resource fence provide ordering.

## Exact open-driver mismatch

The open code already has the right low-level safety properties: user packets
are copied, validated, relocated, converted to kernel-owned coherent
secondaries, referenced BOs remain live, and asynchronous slots retire from
`0x41c`.  Its batching limits are much smaller and tied together:

| resource | current open limit | vendor comparison |
|---|---:|---:|
| Mesa command storage | 4,096 dwords / 16 KiB | 135,276-byte high-level record request |
| Mesa aggregate vertices | 4,096 dwords / 16 KiB | 3.2 MiB mapped vertex ring |
| user packets per ioctl | 221 | lower command ring has 84 reusable 8 KiB pages |
| triangles in one frontend group | 180, often less by dwords/vertex | reuse is page/tag based |
| in-flight kernel requests | 3 | window Present 6; flip queue 32; vertex ring 400 pages |

The constants are `PH_BATCH_DWORDS=4096`, `PH_BATCH_PACKETS=221`, and
`PH_BATCH_MAX_DRAWS=180` in Mesa `parhelia_batch.h`.  The kernel permits 64K
command dwords but retains `PARHELIA_MAX_USER_PACKETS=221`, 4,096 host vertex
dwords, and three asynchronous slots in `parhelia_drv.h`.

`ph_submit_batch()` can append complete different-state streams when
`PARHELIA_BATCH_STATE_STREAM` is set, but that setting is default-off.  It
drains when packet+hidden-stage CALLs reach 221, resources reach the relocation
cap, commands reach 4,096 dwords, or vertices reach 4,096 dwords.  It first
concatenates every transformed vertex into the 16 KiB CPU-side
`pending_vertices` array.  The production-safe drain path uses
`PARHELIA_RELOC_DRAW`: the kernel copies that aggregate into a coherent DMA32
slot and patches each `0x97` start/end to class 3.  The coupling remains: one
aggregate must fit in the kernel's 4,096-dword staging range.

An opt-in experimental drain can instead copy the aggregate into a mapped
class-0 `CREATE_VERTEX` slot and convert relocations to
`PARHELIA_RELOC_DRAW_VRAM`.  It is not a production solution.  BAR0 is a
programmable host-aperture window rather than a linear CPU view of local
memory; an observed raw `0x01364000` vertex mapping wrote into framebuffer
rows 403/404.  The kernel therefore defaults `vram_vertex=false` and describes
the feature as an unsafe raw-BAR experiment.

The experimental class-0 helper has only three subranges.  Before writing
one it calls `WAIT_FENCE` if that slot's prior tag is live.  This is correct,
but it recreates frequent page-reuse back pressure with only about 64.8 KiB of
total mapped storage, versus the vendor's 3.2 MiB/400-page CPU staging ring.
Expanding that raw mapping would expand the scanout-corruption hazard.

On the kernel side, one ioctl performs `memdup_user` separately for packet,
command, and relocation arrays, looks up every relocation object, constructs a
CALL plan, prepares one slot's coherent secondary heap, creates and attaches a
terminal `dma_fence`, and advances `0x408`.  Repeating that approximately 178
times per frame is substantial even when none of those calls performs a final
busy wait.

## Smallest implementation plan

### Implemented foundation: pack packet runs into secondary CALLs

The kernel now represents a CALL-plan entry as a first packet plus a packet
span.  Consecutive exact `0x8f000001` packets are copied, after relocation,
into one coherent secondary allocation and enclosed by one primary CALL.  It
copies each already validated packet separately, so a malformed offset table
cannot pull unchecked gap bytes into the DMA span.  WARP-stage, opening,
EOB/drain, and logical state-boundary plan entries terminate a run.

The static init test covers byte-exact two-header concatenation, operands
patched after the plan was built, a WARP-stage split, both fence triples around
a stateful boundary, and refusal to coalesce packets separated by an offset
gap.  This reduces primary CALL traffic for the mode-3 path without weakening
the proven one-packet-per-triangle depth rule.  It does not by itself reduce
the number of DRM ioctls or lift the input packet-count check.

### Phase 1: enlarge the safe class-3 aggregate

The first syscall reduction needs no new mmap ABI.  Keep
`PARHELIA_RELOC_DRAW`, raise the kernel-owned coherent vertex capacity from
4,096 to an initial 32,768 dwords (128 KiB), and report that capacity through
`GET_PARAM` so Mesa does not encode a build-time assumption.  The async slot
owns the coherent allocation until its terminal `dma_fence` retires.  On each
submit the kernel performs one `copy_from_user`, applies `dma_wmb()` before
the producer update, and patches every validated `0x97` range with
`lower_32_bits(vertex_dma) + offset | 3`.

Mesa should separate command and vertex limits, retain transformed vertices
in the pending aggregate up to the queried vertex limit, and make complete
state-stream aggregation the normal P690 path after the existing image gates
pass.  It should drain at command/packet/relocation capacity, CPU map or
readback, a real resource hazard, explicit finish, and end-of-frame handoff.
This costs one CPU copy per aggregate, but removes scores of ioctl validation,
allocation, fence, and object-reference lifetimes.  The backing is system DMA
and physically cannot alias scanout.

### Phase 2: optional mapped DMA upload object

If the aggregate copy becomes measurable, add a dedicated
`CREATE_DMA_VERTEX` or `CREATE_UPLOAD` object.  Back it with
`dma_alloc_coherent`/`dma_alloc_wc` under the device's 32-bit DMA mask and map
it only through a DRM fake mmap offset.  Return a GEM handle and mmap offset,
never the DMA address.  A new draw relocation names `{handle, offset, length}`;
the kernel must verify that the creating file owns this exact upload-object
type, range-check it, require the normal `0x97` packet, and patch class-3
start/end operands from its private `dma_addr_t`.

Do not accept imported dma-bufs, dumb/scanout BOs, TTM VRAM objects, or raw
addresses for this relocation.  Reject allocations or subranges above
`U32_MAX`, call `dma_wmb()` before the doorbell, and tag each userspace 8-KiB
ring page with the terminal fence before reuse.  Those rules give Mesa the
vendor's mapped-ring convenience while keeping commands and addresses under
kernel control and keeping the storage physically separate from VRAM.

The existing descriptor-managed host aperture can map a local VRAM BO without
the raw-BAR alias: it reserves low logical aperture ranges independently of
TTM physical offsets, keeps a 256-KiB guard, and publishes one of 19 dynamic
descriptor slots.  A 3.2-MiB mapping would fit under its 16-MiB logical limit.
Live P690 texture-aperture tests have nevertheless shown stale or aliased
destination behavior, so this route needs its own hardware proof before it is
used for vertices.  It is not the first safe performance fix.

### Phase 3: decouple syscall count from the proven CALL-plan cap

Do not simply raise `PARHELIA_CALL_CAPACITY` past the tested range.  The open
hardware work found unreliable behavior above roughly 257 CALLs, and the
current 221-user-packet limit reserves opening/EOB/drain and hidden WARP-stage
CALLs inside a 224-entry plan.

Add a submit-chain ioctl or a versioned extension of `drm_parhelia_submit` that
accepts a bounded array of ordinary submit descriptors.  A practical first
bound is 128 KiB of command records and a fixed maximum number of chunks and
relocations.  The kernel must:

1. copy and validate the complete descriptor list before publishing any work;
2. validate every chunk with the existing packet and relocation rules;
3. keep each internal CALL plan at or below the current 224-entry limit;
4. retain all GEM references, VRAM page ranges, and coherent secondaries as one
   request lifetime;
5. preserve every internal EOB/drain or state-arm boundary already required by
   the validated chunks, while allocating their tag sequence monotonically;
6. enqueue the chunks in order in the persistent primary allocation, using as
   many safe primary spans/tail publications as needed; and
7. append one final nonblocking lifetime fence, return immediately after the
   final producer publication, and retire the entire chain from that returned
   fence.  Internal ordering tags may remain; userspace needs only the final
   completion value.

Mesa then keeps recording until frame end or true hazard and crosses into the
kernel once for the common frame.  Internal hardware CALL and `0x408` cadence
remains bounded by what has been tested; only the expensive userspace/kernel
and per-request lifetime cadence changes.

A mapped raw GPU command ring is unnecessary and would weaken validation.  A
normal userspace record builder plus one bulk ioctl obtains the relevant vendor
property.  If copying 128 KiB later becomes measurable, sealed mapped staging
slots can replace the copy without changing the validation and ownership
model.

### Phase 4: reduce GPU packet work independently

Native indexed draws remain a separate high-value project.  Q3's
`glDrawElements` calls are currently expanded by Draw into one `0x97` packet
per triangle.  A proven `0x9f` indexed path would turn each GL indexed draw into
far fewer packets, lowering both kernel validation work and the number of
submit-chain chunks.  It is not required to eliminate the 178 syscall entries,
and its P690 mode-7 contract should keep its own image/hang gate.

## Measurement contract

Count all four levels separately during the same fixed `timedemo` or demo
segment:

```text
GL draw calls/frame
Mesa logical state+draw groups/frame
DRM submit ioctls/frame
kernel primary CALLs and 0x408 publications/frame
```

Also record aggregate command bytes, vertex-ring bytes/pages, packet count,
relocation count, queue-full/page-reuse waits, terminal-fence latency, process
user/system CPU time, and FPS.  The immediate success criterion is a large drop
from approximately 178 DRM submits/frame with identical framebuffer hashes.
The architectural target is one to a few DRM submit transitions per frame;
primary CALL count can remain larger because that is how the vendor kernel
feeds the hardware.

Run uncapped no-vsync `glxgears` after every batching change as the standing
performance and correctness regression, followed by the fixed Q3 image and
timedemo.  Present/vblank tests should verify that a queued window blit or flip
waits for the aggregate's terminal render fence without introducing a CPU
wait.

## Evidence files

* `scratchpad/libGL64.asm`: record reserve/commit/flush at `0x1c9c50`,
  `0x1c9ad0`, and `0x1c9be0`; GLX flush at `0x35897e`; render doorbell at
  `0x3589b0`; ring allocators at `0x1b2060` and `0x1b22e0`.
* `scratchpad/mtx_parhelia_ko.asm`: persistent CALL enqueue `mtx1937` at
  `0x32360`; ring turnover `mtx2095` at `0x31fc0`; restart `mtx2508` at
  `0x31880`; fence constructor `mtx3144` at `0x31c30`; Present/flip helpers at
  `0x50980`, `0x50680`, `0x315b0`, `0x313e0`, and `0x3a590`.
* `scratchpad/pard.asm`: cached writer/wait at `0x91840`, `ValidateState` at
  `0xc9ac0`, primary restart at `0x328d0`, and fence constructor at `0x32b60`.
* `captures/ioq3-p690-working-draw.txt`: allocations, mappings, zero-argument
  doorbells, record lengths, and command/vertex ring changes.
* `docs/parhelia-control-commands.md` and `docs/parhelia-kernel-abi.md`:
  operation-5 allocation fields, `mtx3112`/`mtx938` dispatch, region backing,
  exact mmap-cookie matching, and the SYS fault handler.
* `docs/parhelia-draw-builders-kernel.md` and
  `docs/parhelia-vertex-fetch-and-memory-classes.md`: `mtx1017` CPU copy,
  class-3 pushbuffer fetch addresses, and `mtx1132` address classes.
* `docs/parhelia-p690-bound-range-probe.md`: live two-triangle Z24 comparison
  establishing one `0x8f` hardware packet per interlocked triangle.
* Open Mesa: `src/gallium/drivers/parhelia/parhelia_context.c`,
  `parhelia_batch.c`, `parhelia_batch.h`, and `parhelia_drm.h`.
* Open kernel: `drivers/gpu/drm/parhelia/parhelia_submit.c`,
  `parhelia_packets.h`, `parhelia_drv.h`, and UAPI `parhelia_drm.h`.
