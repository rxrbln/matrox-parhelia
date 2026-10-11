# Parhelia vendor DMA queue depth and Q3 throughput

This note narrows the saved Linux and Windows vendor-driver analysis to command
queue depth, producer publication, retirement policy, and vertex placement.  It
then compares those properties with the open kernel's eight request slots and
Mesa's legacy three-slot/current seven-slot vertex paths.  Binary addresses are
file-relative `.text` offsets for `mtx_parhelia.o`, Windows image virtual
addresses for `MTXOGL.dll`, and image-relative offsets for `MTXPARD.dll`.

## Findings at a glance

* The Linux vendor kernel uses a persistent bus-master primary ring.  Ordinary
  enqueue appends a five-dword CALL and advances `0x0408`; it does not restart
  the fetcher or wait for completion.
* The ring constructor requests up to **four primary backing buffers** and up
  to **eight companion/secondary backing buffers**.  These are allocation
  maxima, not a claim that all allocations always succeed.  A reduced-memory
  constructor branch requests two of each.
* There is no normal-path programmable high/low MMIO watermark in the recovered
  code.  Space remaining in the current backing buffer and availability of one
  retired alternate buffer are the producer's effective watermarks.
* Both vendor ICDs build roughly 135 KiB high-level command lists and retire
  upload storage in 8 KiB pages.  Linux maps a 3.2 MiB vertex heap, or 400
  independently tagged pages.  Waiting occurs only before reuse of a live page.
* Linux vendor vertices are not embedded in the primary CALL list.  The ICD
  writes a persistent mapped system-memory staging heap; the kernel copies the
  selected vertices into its DMA-visible pushbuffer, and the GPU fetches that
  class-3 bus address.  The open `CREATE_VERTEX` path improves this by letting
  the GPU fetch directly from a persistently mapped coherent DMA32 object.
* The open kernel admits eight terminal-fenced requests.  Current Mesa uses
  seven 120 KiB vertex regions when the kernel reports eight slots, leaving one
  request slot free.  The old three-slot mode leaves five kernel slots unable
  to help when Mesa reaches vertex-region reuse.

## Linux vendor bus-master storage depth

The device constructor is `mtx3282` at `0x2fb90` in
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`.

At `0x2fca9..0x2fcc6` it initializes two allocation descriptions.  It writes
`4` into the first description's maximum-count field and `8` into the second:

```text
0x2fcb7  movl $4,  0x40(%rsp)   first description
0x2fcbf  movl $8,  (%rsp)       second description
```

At `0x2fe40..0x2fe52` the first description constructs the object later stored
at device `+0x2168`, already identified as the primary bus-master ring.  At
`0x2ff00..0x2ff16` the second description constructs the companion object at
device `+0x2170`.  The latter inherits the former's allocator and per-buffer
size.  The low-memory/capability branch at `0x2fe20..0x2fe3b` changes both
maximum counts to two.

The common allocator `mtx2290` at `0x2f610` proves these values are buffer
counts.  It compares actual count `object+0x40` with maximum `object+0x08`,
calls the backing allocator at `0x2f68c..0x2f69b`, links each successful
allocation into a circular list, and increments `+0x40` at
`0x2f681..0x2f687`.  Allocation failure exits early.  Therefore the precise
statement is:

```text
normal constructor maxima: primary 4, companion 8
reduced constructor maxima: primary 2, companion 2
actual depth:              allocations that succeeded, at least 1
```

The primary vtable is `mtx3628` at `.rodata+0x3c80`; relocation
`.rodata+0x3cb0` installs `mtx2095` as its storage-turnover method.  The
companion vtable is `mtx3278` at `.rodata+0x3cc0`; relocation
`.rodata+0x3cf0` installs `mtx2100` as its corresponding turnover method.

### Producer and full condition

Normal enqueue is `mtx1937` at `0x32360`:

```text
0x323ab..0x323bd  reserve 0x14 bytes, compare new cursor with object+0x70
0x323ce..0x323e3  write {20292820,0,secondary_start,secondary_end,0}
0x323f8..0x3244b  translate the new cursor and publish it once to MMIO 0x0408
```

There is no completion read on this path.  When the 20-byte reservation would
cross the current buffer's limit, the vtable turnover method runs.

`mtx2095` at `0x31fc0` implements the real full/available policy.  If actual
buffer count `+0x40` is greater than one, `0x31fd7..0x3201d` walks the circular
list and accepts an entry only when its busy flags, pending byte, and
retirement callback all say it is reusable.  It then writes the four-dword
switch record at `0x3204f..0x32072`:

```text
21272425  generation  prior_status_address  new_buffer_dma_address
```

It changes the active base/cursor/limit and advances `0x0408` at
`0x32149..0x32158`.  The producer can therefore continue as soon as **one**
alternate backing buffer retires.  If all alternates are live, the list walk
continues until one becomes reusable.  This is an availability threshold,
not a numeric FIFO watermark.

Only the one-buffer path at `0x32168` publishes the old tail and waits for a
complete idle condition: internal count `+0x78 == 0`, `0x08f0` bit 17 set,
and MMIO `0x0450` equal to the saved FIFO baseline.  It can then reset the
cursor and invoke `mtx2508`.  `mtx2508` at `0x31880` uses those same checks
before exceptional `0x0404`/`0x0400` restart.  These waits are absent from
ordinary CALL publication.

The Windows display driver corroborates the policy.  `MTXPARD.dll` writer
`0x91840` drains its small indexed-register write cache only after the queued
count exceeds 16 (`0x91ac5..0x91ad7`).  That software cache threshold must not
be confused with bus-master ring depth.  Its primary restart at `0x328d0` is
again an exceptional `0x08f0`-idle path.

## Vendor fence and wait policy

Linux fence constructor `mtx3144` at `0x31c30` writes this nine-dword record at
`0x31c67..0x31ceb`:

```text
21c0c0c0 00000307 00000004 00000707 c0afafdd
00000000 tagA|1 tagB|1 saved_DWGCTL
```

Tags advance in nonzero steps of eight and retirement is read from
`0x041c & ~7`.  Fence insertion is explicit at synchronization, finish,
resource-reuse, and presentation boundaries.  `mtx1937` does not insert a
wait after each CALL or high-level draw record.

The Linux ICD page allocator at `libGL.so.1.3.0` address `0x1b2060` computes
the first and last 8 KiB pages touched by the requested byte interval
(`0x1b20a3..0x1b20d8`).  For each touched page it compares the page's saved tag
with completed/current generations (`0x1b20dd..0x1b213c`).  It flushes pending
records only when the chosen page is still live (`0x1b21b5..0x1b21cb`) and
then yields with `usleep(0)` while retirement catches up.  Commit routine
`0x1b21e0` tags only the pages actually consumed.

The one-page allocator at `0x1b22e0` shows the same rule more directly: wrap
on the 8 KiB boundary, test that page's tag, flush only under pressure, wait
only for that page, optionally assign a new generation, and advance exactly
one page.  The captured 3.2 MiB vertex mapping therefore supplies 400
independently reusable pages; the 0xa8000-byte client command ring supplies 84.

Windows independently implements this algorithm in `MTXOGL.dll` at
`0x10009960`.  It wraps in 8 KiB units at `0x10009967..0x1000999c`, checks the
selected page tag at `0x100099a2..0x100099b2`, and flushes records only on a
live-page collision.  The pressure loop uses
`WaitForSingleObject(Global\\MTXDMAWriteEvent, 100)` at
`0x10009a77..0x10009a91` when the event path is available, or `Sleep(0)` at
`0x10009a96..0x10009a9e`.  The possible interrupt-backed event accelerates a
reuse wait; it is not a per-draw completion interrupt.

## High-level batching and packet boundaries

Linux requests a `0x2106c`-byte high-level record buffer (mapped as 0x22000
bytes); Windows requests `0x2105c`.  The Windows reservation routine at
`0x10003640` is an independent readable example: it sends only when the next
record crosses capacity (`0x100036ae..0x100036ca`), and adjacent records with
the same opcode share one `(opcode,count)` header
(`0x100036cf..0x100036ed`).  Sender `0x10003400` submits one base/length span
and resets the builder.  Linux uses the equivalent reserve/commit/send split
at `libGL64` `0x1c9c50`, `0x1c9ad0`, and `0x1c9be0`.

This record merge does not erase hardware packet boundaries.  For current Q3
mode 3, each triangle needs its own `0x8f000001,first,last` packet for the
proven depth interlock.  Multiple complete packet byte strings may share one
secondary DMA CALL:

```text
CALL -> [8f000001,0,3, 8f000001,3,6]
```

The two packet headers remain visible to the parser.  A run must stop at a
WARP-stage insertion, opening/EOB/drain sequence, or logical state section.
Combining two triangle ranges under one `0x8f` header is not equivalent and
failed the live overlapping-Z test.

## Vertex placement: vendor and open paths

The saved Linux ICD first attempts its 0x320010-byte vertex allocation through
memory types 1, 2, and 3, then succeeds with type 4.  The recovered kernel ABI
maps type 1 to AGP, type 2 to PCI-locked, and type 4 to SYS.  Thus the working
vendor mapping is a CPU-written system-memory staging heap, not persistent
VRAM.

Kernel draw translation (`mtx1017+0x60e0`) copies the referenced staged
vertices into a kernel-owned DMA-visible pushbuffer and emits the stream and
draw commands that fetch its class-3 bus address.  The captured class-3 range
is `0x19dc13f8..0x19dc1434 | 3` in a pushbuffer based at `0x19dc0000`.
Primary CALL entries contain addresses of command secondaries; they do not
inline vertex floats.

The open stack has two paths:

* copy-submit snapshots at most 4096 vertex dwords into the selected kernel
  request slot before publication;
* `CREATE_VERTEX` maps one creator-owned coherent DMA32 object and puts that
  object's class-3 bus address directly in validated draw state.  Mesa writes
  it before the ioctl and uses the returned terminal fence to prevent reuse.

The direct `CREATE_VERTEX` path already removes the vendor Linux staging-to-
kernel copy.  Moving these vertices to VRAM or adding inline primary data would
discard this advantage and has no binary evidence behind it.

## Exact comparison with three and seven Mesa slots

The open kernel constants are:

```text
hardware CALL capacity        224
async request slots             8
per-slot primary reservation  224*5+16 = 1136 dwords = 4544 bytes
complete async primary          8*1136 = 9088 dwords = 36352 bytes
maximum CREATE_VERTEX object    1 MiB
```

Each accepted asynchronous ioctl owns a coherent secondary heap, optional
4096-dword copied-vertex area, status page, pinned GEM references, and one
terminal `dma_fence`.  First publication writes `0x0404`; later publications
only extend `0x0408`.  On eight live requests, the next submit waits.  Once the
queue drains and `0x08f0` bit 17 confirms idle, the primary cursor resets to
zero.  Retirement reads `0x041c` from a delayed worker rescheduled every 1 ms.

Mapped Mesa capacity negotiated for 1024 packet descriptors is 30,720 vertex
dwords, or exactly 122,880 bytes/15 pages per coarse slot:

| Mesa policy | mapped bytes | 8 KiB pages | maximum distinct vertex submits before exact-slot reuse |
|---|---:|---:|---:|
| legacy 3 slots | 368,640 | 45 | 3 |
| current 7 slots | 860,160 | 105 | 7 |
| vendor Linux heap | 3,276,800 | 400 | byte/page driven, not fixed submit count |

At context creation Mesa selects `async_slots - 1`, hence seven, when the
kernel reports 4..8 slots; it otherwise falls back to three.  Before writing a
slot, `ph_submit_batch_now()` waits on only that slot's prior returned fence.
Seven therefore matches the eight-request kernel while deliberately retaining
one spare request slot.  Three creates userspace back pressure after three
large regions even if five kernel request slots are free.

The remaining difference is granularity.  A batch that uses 8 KiB still owns
one 120 KiB Mesa slot until its terminal fence retires.  The vendor tags only
the pages touched.  This does not prove that page granularity is today's Q3
bottleneck: the eight-request kernel cap may be reached first, and ordered GPU
completion makes older batches retire in order.  Existing counters must decide.

The open fence is also broader than the vendor's page tag.  Every ioctl gets a
terminal DWGSYNC fence, and `parhelia_slot_attach_fence()` currently adds it as
`DMA_RESV_USAGE_WRITE` to every referenced object, including sampled textures.
That is safe but forces CPU/KMS users of read-only inputs to observe a writer
dependency that does not exist.

## Safest runtime probe

Do not change clocks, DRAM timings, ring depth, or IRQ routing first.  The
kernel already exports the evidence needed in its submit-statistics debugfs
file.  Around the same uncapped/no-vsync `glxgears` interval and fixed ioquake3
timedemo, capture at least:

```text
async_submits
async_slot_waits / async_slot_wait_ns
primary_tail_waits / primary_tail_wait_ns
queue_high_water
async_ring_start_0x404_writes / async_ring_end_0x408_writes
```

Also add or enable Mesa-side counts for vertex-slot waits and aggregate drain
reasons.  Compare the delta, not the boot-lifetime absolute values.

Interpretation is direct:

* high water 8 plus material `slot_wait_ns`: the kernel request lifetime/cap
  is limiting; adding Mesa vertex memory cannot help;
* high water below 8 plus vertex-slot waits: coarse three/seven-slot reuse is
  limiting; page-tagged suballocation is justified;
* low wait time but many `async_submits`: ioctl validation/allocation cadence
  is limiting; larger state-stream aggregation or a validated submit chain is
  the high-value change;
* many ring starts relative to submits: the queue drains between small bursts;
  Mesa is failing to keep the persistent producer fed;
* one ring start followed by many ring-end writes with low waits: queueing is
  working and remaining cost is packet/vertex/pixel work.

## Safe optimization order

1. Keep seven Mesa regions on the eight-slot kernel.  Do not revert to three
   and do not raise the kernel depth until the counters show an eight-slot
   pressure problem.
2. Reduce semantic/capacity drains in Mesa so complete ordered state streams
   share one ioctl whenever resources remain GPU-owned.  Preserve the exact
   `0x8f` headers and the WARP/opening/EOB/drain boundaries.
3. If vertex-slot wait time is measurable before kernel queue pressure,
   replace fixed 120 KiB reuse with an 8 KiB circular suballocator over the
   existing 860,160-byte object.  Associate each used page with the submit's
   returned fence and wait only on overlapping pages.  This needs no raw-ring
   UAPI and mirrors both vendor ICDs.
4. If submit count remains high while waits stay low, add a bounded validated
   submit-chain interface.  Validate the whole chain before publication, split
   it into internal spans of at most 224 CALLs and 4096 dwords per secondary,
   retain resources across the chain, and return one lifetime fence.  Never
   raise the recovered CALL-span limit merely to reduce syscalls.
5. Classify relocations derived from validated packet use.  Attach sampled
   textures as READ dependencies and render/depth destinations as WRITE.  Keep
   conservative WRITE only for unresolved uses.
6. Retain direct coherent class-3 `CREATE_VERTEX` fetching.  Do not introduce
   a vendor-style extra CPU copy, speculative AGP/VRAM placement, or inline
   vertex data.
7. Keep 1 ms polling until a render-completion IRQ bit and acknowledge sequence
   are proven from the vendor core or hardware.  The Windows DMA event supports
   sleeping on page pressure, but it does not identify the interrupt source.

The likely Q3 throughput gain lies in fewer request lifetimes and less CPU
record/validation work, not in restarting the fetcher faster.  The vendor
architecture deliberately amortizes those costs: large high-level lists,
persistent `0x0408` publication, page-granular reuse, and waits only at actual
ownership pressure.

## Evidence

* `scratchpad/mtx_parhelia_ko.asm`: `mtx2290` `0x2f610`, constructor
  `mtx3282` `0x2fb90`, restart `mtx2508` `0x31880`, fence `mtx3144`
  `0x31c30`, turnover `mtx2095` `0x31fc0`, enqueue `mtx1937` `0x32360`,
  companion turnover `mtx2100` `0x32690`.
* `scratchpad/libGL64.asm`: page allocators `0x1b2060` and `0x1b22e0`, record
  reserve/commit/send `0x1c9c50`/`0x1c9ad0`/`0x1c9be0`.
* `scratchpad/ogl.asm`: record sender/reservation `0x10003400`/`0x10003640`,
  page allocator `0x10009960`.
* `scratchpad/pard.asm`: primary restart `0x328d0`, cached writer/wait
  `0x91840`, fence constructor `0x32b60`.
* `docs/parhelia-kernel-abi.md`, `docs/parhelia-vertex-fetch-and-memory-classes.md`,
  and `docs/parhelia-vendor-q3-command-buffering-performance-re.md`.
* Open kernel `drivers/gpu/drm/parhelia/parhelia_submit.c`,
  `parhelia_drv.h`, and `parhelia_packets.h`; open Mesa
  `src/gallium/drivers/parhelia/parhelia_context.c`, `parhelia_batch.c`, and
  `parhelia_batch.h`, inspected 2026-09-11.
