# Vendor Q3 command buffering and the remaining submit-rate gap

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope and result

This note compares the saved Matrox Linux and Windows drivers with the current
open Mesa/kernel path for a Q3-style stream of many small, state-changing draws.
It concentrates on user/kernel transitions, command and vertex buffering,
bus-master DMA, fence placement, and presentation.  It changes no driver code
and makes no target changes.

The current measurement, approximately 2,459 asynchronous DRM submit ioctls
per second at 30 FPS, means:

```text
2459 / 30 = 81.97 submit ioctls per rendered frame
1 / 2459 = 0.4067 ms mean interval between submit ioctls
```

The interval is not the cost of an ioctl; this count alone cannot apportion CPU
time between userspace command construction and the kernel.  It does show that
the eight-slot asynchronous queue and packed secondary CALLs have not fixed the
request granularity.  About 82 validated submissions, terminal fence sequences,
object-reference sets, and syscall entries are still created for each frame.
The vendor architecture instead
keeps high-level state and draw records in a large mapped userspace buffer and
enters the kernel only when that buffer fills or an explicit flush/end-of-frame
path requires it.  The saved artifacts do not contain a clean timed vendor
frame trace, so they cannot establish an exact vendor ioctls/frame number.  They
do establish that the number is governed by a 135,276-byte record-buffer
threshold and explicit flushes, rather than by the number of draws.

The current trace is dominated by aggregates containing one logical batch and
170 or 188 packets.  Both are far below the 1,024-descriptor submit limit and
both carry almost exactly 4,096 vertex dwords: 4,077 and 4,080 respectively.
They are frontend-full expansions from the legacy CPU alpha-micro path, not
kernel descriptor-full aggregates.  Raising the descriptor limit again cannot
reduce those ioctls.  Replacing that expansion with the in-progress GPU alpha
path is the immediate fix; the batching work below applies to the stream that
remains afterward.

The next safe open-driver work has three parts:

1. Turn on complete ordered state-stream aggregation for the supported P690
   path, and record an explicit drain-reason counter.  Same-state aggregation
   cannot cross Q3's frequent texture/blend/depth changes; the complete-stream
   path already can.
2. Replace the blanket write-map drain/wait with per-resource read/write fence
   tracking.  A CPU write to an unrelated texture must not submit the current
   draw stream.  A write to a texture still read by the GPU should wait only for
   that resource or rename its backing, matching the vendor's tagged-page reuse
   rule.
3. Add a bounded submit-chain UAPI only after the above shows genuine 1,024
   packet or 16,384-command-dword drains.  One ioctl should validate multiple
   ordinary chunks, enqueue their safe 224-CALL/4,096-dword internal spans in
   order, and return one terminal fence.

This copies the vendor's batching boundary without exposing unvalidated GPU
commands or physical addresses through a mapped ring.  In particular, the
vendor's 136 KiB allocation must not be used as evidence for a large local-
VRAM `CREATE_VERTEX`: it is a CPU-only `vmalloc` record buffer.  The vendor
copies ordinary system-memory vertices into a distinct DMA32 pushbuffer before
the GPU fetches them.

The direct answers from the saved artifacts are:

| question | recovered behavior |
|---|---|
| Multiple ranges/state blocks per userspace doorbell? | Yes.  Linux and Windows keep mixed high-level records until capacity or an explicit semantic flush; adjacent opcode `0x1b` draw items share a count header.  The Linux kernel then iterates every 40-byte draw item, so one doorbell can build many hardware draw packets and CALLs. |
| Multiple ranges/state blocks per MMIO DMA tail write? | The saved code does not prove one MMIO write for an entire high-level list.  `mtx1937` appends one five-dword secondary CALL and writes `0x408` once per invocation; the record decoder may invoke it more than once.  Persistence and lack of a wait/restart between publications are proven. |
| Persistent DMA layout? | Yes.  The Linux kernel appends five-dword secondary-CALL records to a persistent primary ring and advances MMIO `0x408`; separate reusable storage includes an 84-page command ring and a 400-page, 8 KiB-page vertex ring. |
| Fence/interrupt cadence? | Nonblocking tags are inserted at ordering and storage-lifetime boundaries.  Completion is checked on reuse, finish, and present back pressure.  No per-draw interrupt or wait is visible; Windows may use `Global\\MTXDMAWriteEvent` when a tagged page is still live. |
| Main thresholds/fast paths? | Linux high-level records request `0x2106c` bytes; Windows allocates `0x2105c`.  Same-opcode records merge, state is dirty-cached, upload reuse is tagged per 8 KiB page, Windows indexed writes drain after more than 16, window-present history is six tags, and the flip queue has 32 entries. |
| Current open-driver implication? | `coalesce_primary=true` already publishes one `0x408` tail per normal submit; `pack_draw_packets=1` lowers primary CALL/secondary work inside that submit.  Neither removes validation, pin/reference, fence, or syscall work.  Current 170/188 singletons are alpha-micro frontend expansions at about 4,096 vertex dwords; fix that path before changing the 1,024-descriptor limit. |

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
three 120 KiB vertex slots.

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
opening/EOB/drain record, or logical state-batch boundary.  The kernel always
recognizes the proven exact three-dword `0x8f000001` form and, with
`pack_draw_packets=1`, also packs adjacent relocated `0x97` draw streams.
General adjacent-packet packing remains a separate opt-in experiment.

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

No per-draw render interrupt is visible in the saved Linux path.  Normal
enqueue at `mtx1937` neither reads completion nor programs an interrupt; reuse
paths consume the fence page or poll `0x41c`.  The open wrapper
`kernel/src/mtx_irq.c:53..75` merely calls the proprietary core's `irq_fast`
and schedules `irq_slow` when requested, so the saved source cannot enumerate
all interrupt causes.  It does establish that IRQ delivery is outside the
record-buffer doorbell and ordinary secondary-CALL path.  The defensible
conclusion is therefore no interrupt per draw or per high-level record, not
that the device never raises a render-related interrupt.

## Windows OpenGL ICD: independent batching and reuse evidence

The saved 32-bit Windows ICD is
`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll`, image base `0x10000000`;
offsets below are virtual addresses in `scratchpad/ogl.asm`.  It independently
implements almost the same high-level policy as the Linux ICD.

The context constructor pushes `0x2105c` at `0x100077ff` and calls the buffer
initializer at `0x10007806`.  That initializer, `0x10003590`, stores the size
at object offset `+0x0c`, allocates the storage, sets base/current/previous at
`+0x10/+0x14/+0x18`, and writes the initial sentinel.  The Windows request is
16 bytes smaller than Linux's `0x2106c`, but both are approximately 135 KiB.

The generic reservation routine at `0x10003640` has the same decisive
properties:

* `0x100036ae..0x100036c4` compares the requested record end with
  `base + size`; `0x100036c8..0x100036ca` calls the sender only on capacity.
* `0x100036cf..0x100036dd` compares the previous opcode and adds the new item
  count to its header when equal.  Otherwise `0x100036df..0x100036ed` writes a
  new `(opcode,count)` header.
* Commit at `0x100033f0..0x100033fa` advances the local write pointer only.
* Sender `0x10003400` passes one base/byte-length interval to `0x1002ad80` at
  `0x1000342d..0x10003447` and then resets the record buffer at
  `0x10003459..0x10003478`.  The normal `0x1002ad80` dispatch does not wait;
  its `Sleep(10)` at `0x1002ae16..0x1002ae28` is an error fallback.

The draw builder at `0x1031ca6f..0x1031ca7f` requests opcode `0x1b` with a
`0x20`-byte element from `0x10003640`, serializes multiple elements, and calls
commit at `0x1031cc32..0x1031cc38`.  Consecutive Windows draw descriptors thus
share one high-level opcode/count record and one later sender call; state blocks
and triangle ranges are not doorbelled individually.

The fixed-page upload allocator at `0x10009960` proves reuse rather than draw
cadence controls waiting.  It wraps in 8 KiB units at
`0x10009967..0x1000999c`, compares the selected page tag with retirement at
`0x100099a2..0x100099b2`, and flushes buffered records at
`0x100099b4` only if that page is still live.  It then loops on retirement,
using `WaitForSingleObject(handle,100)` at `0x10009a77..0x10009a91` when the
DMA event facility is available or `Sleep(0)` at
`0x10009a96..0x10009a9e` otherwise.  The imported event name is
`Global\\MTXDMAWriteEvent`.  This may be interrupt-backed inside Windows, but
the event is consumed on page pressure; there is no wait after every draw or
record commit.

## Windows display-driver cross-check

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

## Present queueing is not the 82-per-frame problem

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
frame.  Even if it is synchronous, one transition is about 1.2 percent of the
observed approximately 82 render submits per frame.  Removing Present or
vblank ordering cannot account for the Q3 frame rate.  Once render batching is
fixed, Present should enqueue one blit behind the terminal render fence and
let the appropriate resource fence provide ordering.

## Exact open-driver mismatch

The open code already has the right low-level safety properties: user packets
are copied, validated, relocated, converted to kernel-owned coherent
secondaries, referenced BOs remain live, and asynchronous slots retire from
`0x41c`.  Several limits described by the first revision of this note have
since been raised or decoupled:

| resource | current open limit | vendor comparison |
|---|---:|---:|
| Mesa command storage | 16,384 dwords / 64 KiB | 135,276-byte high-level record request |
| Mesa aggregate vertices | 30,720 dwords / 120 KiB | 3.2 MiB mapped vertex ring |
| user packets per ioctl | 1,024 | lower command ring has 84 reusable 8 KiB pages |
| one frontend draw group | 180 triangles and 4,096 vertex dwords | reuse is page/tag based |
| one packed secondary | 4,096 dwords | vendor secondary size is allocator-driven |
| in-flight kernel requests | 8; three mapped vertex slots | window Present 6; flip queue 32; vertex ring 400 pages |

The current constants are `PH_BATCH_DWORDS=16384`, `PH_BATCH_PACKETS=1024`,
`PH_BATCH_VERTEX_DWORDS=30720`, and `PH_BATCH_MAX_DRAWS=180` in Mesa
`parhelia_batch.h`.  The kernel advertises 1,024 user descriptors and has eight
asynchronous slots.  Its copied-vertex fallback and each DRAW range remain
bounded at 4,096 dwords; the mapped dedicated-DMA `CREATE_VERTEX` object is
bounded to 1 MiB.  Adjacent relocated packet byte streams may be enclosed in
4,096-dword secondaries when `pack_draw_packets=1`.

`ph_submit_batch()` can append complete different-state streams when
`PARHELIA_BATCH_STATE_STREAM=1`, but that setting is default-off.  With a
mapped dedicated-DMA vertex object it drains before packet+hidden-stage calls
would exceed 1,024, commands exceed 16,384 dwords, relocations exceed 1,028,
or aggregate vertices exceed 30,720 dwords.  The frontend still finalizes an
individual state batch at 180 triangles or 4,096 vertex dwords; this does not
need to become an ioctl boundary because the state-stream aggregator can append
several such batches.

The former raw-BAR experiment has been replaced with a production dedicated
DMA32 `CREATE_VERTEX` object.  Mesa maps three 30,720-dword slots.  Before
writing a slot it waits for that slot's previous terminal fence, then converts
ordinary DRAW relocations to creator-owned class-3 DMA ranges.  This is safe
and follows the vendor's tagged-reuse principle, although three coarse slots
provide less slack than the vendor's 400 independently tagged 8 KiB pages.

On the kernel side, one ioctl performs `memdup_user` separately for packet,
command, and relocation arrays, looks up every relocation object, constructs a
CALL plan, prepares one slot's coherent secondary heap, creates and attaches a
terminal `dma_fence`, and advances `0x408`.  Repeating that approximately 82
times per frame is substantial even when none of those calls performs a final
busy wait.

### What the current Q3 trace says

The supplied large Q3 trace was captured with
`PARHELIA_BATCH_STATE_STREAM=1`, the 1,024-packet/16,384-command/30,720-vertex
Mesa limits, the mapped DMA vertex ring, asynchronous submit, and kernel
`pack_draw_packets=1`.  Across the retained run it contains 18,255 aggregate
drains representing 50,448 logical batches:

```text
aggregate drains                 18,255
one-logical-batch drains         12,419 (68.0%)
multi-logical-batch drains        5,836
explicit aggregate fence waits      279
170-packet singleton              6,677
  commands/relocs/vertices      613 / 155 / 4,077, every occurrence
188-packet singleton              2,644
  commands/relocs/vertices      666 / 173 / 4,080, every occurrence
packet count >= 975               1,368
maximum command dwords            5,807
maximum vertex dwords            22,644
```

The two dominant singleton shapes are capped by the legacy alpha-micro
frontend's per-group 4,096-vertex-dword check.  They do not approach the
aggregate command or vertex limits.  The trace also shows that complete-stream
aggregation works for ordinary work: aggregates contain as many as 22 logical
batches, and 1,368 drains are packed to at least 975 descriptors.  Those
near-1,024 drains are the cases a later submit-chain can combine without
raising the hardware CALL span.  Only 279 explicit aggregate fence waits appear
beside 18,255 drains, so a wait after every ioctl is no longer the primary
cadence; request construction and the remaining semantic/capacity drains are.

The correct order of work is therefore measurable.  First replace the CPU
alpha-micro geometry expansion with the GPU two-pass path and repeat the
trace.  Then classify remaining drains by reason.  A new trace field should
distinguish `packet`, `command`, `vertex`, `reloc`, `read-map`, `write-map`,
`resolve`, `clear-after-draw`, `flush`, `present`, and `finish`; aggregate size
alone cannot distinguish capacity from an ownership boundary.

## Smallest implementation plan

### Implemented foundation: pack packet runs into secondary CALLs

The kernel now represents a CALL-plan entry as a first packet plus a packet
span.  With `pack_draw_packets=1`, consecutive relocated `0x97` packet byte
streams are copied into at most 4,096-dword secondary allocations and enclosed
by one primary CALL.  It copies each already validated packet separately, so a
malformed offset table
cannot pull unchecked gap bytes into the DMA span.  WARP-stage, opening,
EOB/drain, and logical state-boundary plan entries terminate a run.

The static init test covers byte-exact two-header concatenation, operands
patched after the plan was built, a WARP-stage split, both fence triples around
a stateful boundary, and refusal to coalesce packets separated by an offset
gap.  This reduces primary CALL traffic for the mode-3 path without weakening
the proven one-packet-per-triangle depth rule.  It does not by itself reduce
the number of DRM ioctls or lift the input packet-count check.

### Phase 1: finish alpha on the GPU and make state streams normal

The safe 120 KiB mapped class-3 aggregate already exists.  The trace shows its
capacity is not the dominant singleton cause; the legacy alpha-micro path
expands a draw on the CPU until its local 4,096-dword geometry array fills.
Finish and image-gate the GPU two-pass alpha path first.  Then enable complete
ordered state-stream aggregation by default on the same proven P690 path,
rather than requiring `PARHELIA_BATCH_STATE_STREAM=1`.

Keep finalization of a frontend group separate from `ph_drain_batch()`.  State,
texture, blend, and depth changes should append another validated logical batch
to the pending stream.  Drain only at reported capacity, a same-resource CPU
ownership hazard, explicit finish, or presentation handoff.

### Phase 2: track hazards per resource and upload range

The vendor allocators attach retirement tags to 8 KiB pages and wait only when
the next allocation would overwrite a live page.  Mesa currently has only
three coarse mapped-vertex slots, and `ph_map()` drains and waits for every
texture write whenever aggregation is enabled.  Replace that blanket rule with
resource epochs:

1. Record the last queued GPU read and write fence on each texture/render
   resource, and the terminal fence on each mapped vertex subrange.
2. Before a CPU read, submit pending work that writes that resource and wait
   for its last writer.  Before a CPU write, submit pending work that reads or
   writes that resource and wait for the last conflicting fence.
3. Do neither for an unrelated resource.  For frequently updated Q3 lightmaps,
   rename or rotate upload backing so the CPU normally writes a free range
   instead of waiting.
4. Retain resource references for the aggregate/request lifetime; update the
   per-resource fences only after the kernel accepts the request.

This change copies the vendor's actual fast path more closely than simply
adding vertex slots.  Instrument queue-full, vertex-slot reuse, resource-read,
and resource-write waits separately before increasing ring sizes.

### Phase 3: decouple syscall count from the proven CALL-span cap

Do not simply raise `PARHELIA_CALL_CAPACITY` past the tested range.  The open
hardware work found unreliable behavior above roughly 257 CALLs.  The current
validator accepts 1,024 descriptors only because it packs eligible packet runs
into at most 224 primary CALLs, with each secondary capped at 4,096 dwords.

Add a submit-chain ioctl or a versioned extension of `drm_parhelia_submit` that
accepts a bounded array of ordinary submit descriptors.  A practical first
bound is 128 KiB of command records, 64 logical chunks, and fixed relocation
and vertex-byte maxima.  Each chunk may retain the present 1,024-descriptor
validator bound.  The kernel must:

1. copy and validate the complete descriptor list before publishing any work;
2. validate every chunk with the existing packet and relocation rules;
3. split publication into internal spans whose packed CALL plan is at most 224
   entries and whose secondaries remain at most 4,096 dwords;
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

Mesa then keeps recording until frame end or a true same-resource hazard and
crosses into the kernel once for the common frame.  Internal hardware CALL and
`0x408` cadence remains bounded by what has been tested; only the expensive
userspace/kernel and per-request lifetime cadence changes.  The existing UAPI's
single `PARHELIA_RELOC_BATCH_BOUNDARY` is insufficient for this job: the chain
must describe every logical section or preserve each already validated chunk
as a section while returning one terminal request fence.

A mapped raw GPU command ring is unnecessary and would weaken validation.  A
normal userspace record builder plus one bulk ioctl obtains the relevant vendor
property.  The mapped vertex upload path can remain unchanged; a chain holds
all of its referenced subranges until the terminal fence retires.

### Phase 4: reduce GPU packet work independently

Native indexed draws remain a separate high-value project.  Q3's
`glDrawElements` calls are currently expanded by Draw into one `0x97` packet
per triangle.  A proven `0x9f` indexed path would turn each GL indexed draw into
far fewer packets, lowering both kernel validation work and the number of
submit-chain chunks.  It is not required to eliminate the 82 syscall entries,
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
from approximately 82 DRM submits/frame with identical framebuffer hashes.
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
* `scratchpad/ogl.asm`: Windows record allocation at `0x100077ff`, reserve and
  merge at `0x10003640`, sender at `0x10003400`, draw builder at
  `0x1031ca6f`, and 8 KiB tagged-page reuse at `0x10009960`.
* `matroxdriver-x86_64-cvs-20090511/kernel/src/mtx_irq.c`: open Linux IRQ
  wrapper, including the core top-half/bottom-half delegation at lines 53--75.
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
