# Parhelia vendor persistent-primary DMA and fenced reuse

Scope: focused follow-up to commit `dc7b6a9`. This note records exact producer,
completion, reuse, and CPU-visibility behavior found in the saved vendor
objects, then maps it onto the current open kernel/Mesa queue. It does not
repeat the earlier high-level command-buffer survey. Symbol names in the
Linux object are stripped/generated, so structure-field names below describe
observed roles rather than recovered source identifiers.

## Sources and confidence

* Linux vendor object:
  `xf86-video-parhelia/matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`
  and `xf86-video-parhelia/scratchpad/mtx_parhelia_ko.asm`.
* Windows vendor DLL:
  `xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`
  and `xf86-video-parhelia/scratchpad/pard.asm`.
* Current open paths inspected only:
  `linux-7.2/drivers/gpu/drm/parhelia/parhelia_submit.c`,
  `parhelia_drv.h`, and Mesa `parhelia_context.c`/`parhelia_batch.c`.

Instruction addresses and literal packet words are high confidence. Semantic
names assigned to private fields are medium confidence unless their use is
unambiguous. No target experiment was performed for this note.

## 1. Normal Linux producer advancement (`mtx1937`, `.text+0x32360`)

For a nonempty secondary interval (`start < end`) the routine reserves exactly
`0x14` bytes in the current primary allocation and emits five dwords:

```
0x20292820
0
start | start_flags
end   | end_flags
0
```

Evidence is at `0x323ce..0x323e3`. The CPU write cursor is at primary-object
offset `+0x60`; the limit is `+0x70`. If the new cursor exceeds the limit, the
virtual method at vtable `+0x20` is called and the append is retried. With no
special pending flags at primary `+0x98`, the routine computes

```
producer = new_cpu_cursor + primary->gpu_minus_cpu_bias (+0x9c)
```

and publishes it once with `memWriteDword(..., 0x408, producer)` at
`0x3243c..0x3244b`.

There is no `0x41c` completion read and no `0x404` restart on this ordinary
path. Thus `0x408` is the normal producer/doorbell advancement and `0x404` is
not a per-list launch register.

Before some MMIO writes, the generic register helper examines the previously
written MMIO offset. If it falls in `0x388..0x407`, it writes zero to `0x384`
before recording/writing `0x408`. This is a register-posting/order workaround;
it is not evidence for primary space reclamation or a texture-cache flush.

The `end_flags == 0` branch is exceptional. It polls either `0x31c` or `0x318`
until a masked readiness condition succeeds before appending. Ordinary tagged
secondary ends take the producer-only path.

## 2. Linux backing-buffer turnover (`mtx2095`, `.text+0x31fc0`)

The primary builder owns a configurable count at `builder+0x40`, an active
buffer at `builder+0x58`, and a circular buffer list linked through `buffer+8`.
With more than one backing buffer, turnover walks that list and rejects a
candidate when any of these hold:

* low two ownership bits at `buffer+0x00` are nonzero;
* byte `buffer+0x70` is nonzero;
* `mtx1170(buffer+0x38)` reports the stamped serial busy.

After selecting a candidate it sets ownership bit 0, increments a shared
software serial (skipping zero), and stamps that serial into the old active
buffer's completion object (`old+0x38 -> +0x78 -> +0x4c`) when the old
buffer's `+0x70` byte permits it.

At `0x3204f..0x32072` it emits this exact four-dword turnover record into the
old primary allocation:

```
0x21272425
serial
shared_status_address
new_buffer_gpu_address
```

It then clears the old buffer's low ownership bits, installs the new active
buffer, resets the CPU write/submitted cursors to `new+0x50`, recomputes the
new limit from `new+0x40`, and ultimately publishes a single new `0x408`
producer. The record appears to combine completion marking with transfer to
the new backing allocation. Its exact hardware wrap semantics should be
validated by a capture before the open driver emits it.

The one-buffer fallback is intentionally expensive. It publishes outstanding
work, waits for the shared outstanding count to become zero, polls `0x8f0`
bit 17, polls `0x450` to a saved baseline, clears status, resets the CPU
pointers, and calls the restart routine. Therefore full idle is resource
exhaustion behavior, not normal submission cadence.

The constructors support this interpretation. `mtx2290` allocates
`builder+0x08` backing objects of configured size `builder+0x50` and links them
as a circular list. `mtx2001` initializes the active buffer and the write/limit
pointers. `mtx3766` records size at `buffer+0x40`, GPU address at `+0x48`, CPU
VA at `+0x50`, and GPU-minus-CPU bias at `+0x58`.

## 3. Exact Linux reuse predicate (`mtx1170`, `.text+0x5bc00`)

`mtx1170` is called for candidate `buffer+0x38`. If its `+0x78` completion
object is null, the candidate is reusable. Otherwise it loads:

```
required = completion_object[+0x4c]
status   = candidate_object[+0xb8]
lo       = status[+0x20]
hi       = status[+0x50]
override = status[+0x54]
```

The exact predicate is:

```
if (hi < lo)
        busy = true;
else if (lo >= required)
        busy = false;
else if (hi >= required)
        busy = true;
else
        busy = false;

if (override != 0)
        busy = false;
```

This is a wrap/epoch-aware interval test over a serial stamped on the backing
buffer. It is the vendor's normal consumer-side reuse test. It does not read a
primary hardware GET pointer.

A related slower helper beginning at `0x5bc50` reads `0x41c` only when private
device field `+0x8fc` requests it, masks the low three bits, and performs a
wrap-aware comparison. This establishes `0x41c` as an explicit completion
source, while showing that normal backing reuse can be mediated through the
shared serial/status interval.

## 4. Restart and execution fences are separate operations

Linux `mtx2508` (`.text+0x31880`) is the exceptional restart path. It waits for
the shared outstanding count, `0x8f0` bit 17, and the saved `0x450` baseline,
then writes `0x404 = start & ~3` and `0x400 = limit` (with a capability-based
32-byte mask). It does not advance `0x408` there.

Linux `mtx3144` (`.text+0x31c30`) can append this nine-dword execution
synchronization block when flag bit 1 is set:

```
0x21c0c0c0, 0x307, 4, 0x707, 0xc0afafdd,
0, tag_a | 1, tag_b | 1, saved_DWGCTL
```

It allocates the two tags separately in increments of eight, skipping zero,
and can add `0x21212121` alignment padding. It then stores/publishes the
producer. It does not turn every producer publication into a `0x41c` wait.

These paths distinguish three lifetimes:

1. Primary producer visibility: CPU stores must be visible before `0x408`.
2. Primary allocation reuse: a stamped serial/status predicate determines
   when the old backing allocation can be selected again.
3. Secondary, vertex, and render-resource reuse: wait for the terminal
   execution `DWGSYNC`/`0x41c` tag, because primary fetch alone does not prove
   the referenced work executed.

## 5. Windows producer publication and visibility barriers (`0x32b60`)

The Windows code independently confirms the same model and supplies the most
specific CPU-store visibility evidence.

When context flag bit 1 is set, `0x32b60` emits the same nine-dword sync block.
Its two tags advance by `0x10` in this driver. It aligns the write cursor
relative to base `ctx+0x78` to `ctx+0x74` dwords and fills padding with
`0x21212121`.

When the sign bit of `ctx+0x7c` is set, the routine serializes shared producers
with a `lock xchg` spin lock at `shared+0x5038`. It increments an outstanding
count at `shared+0x5040` once per local pending producer (`ctx+0x70`), computes
the producer and stores it at `shared+0x503c`.

Immediately before writing `0x408`, two platform-selected visibility methods
may run:

* flag `0x100`: an `xchg`-serialized volatile read from the command cursor
  (`ctx+0x54`);
* flag `0x200`: save PCI CONFIG_ADDRESS, select config cycle
  `0x80000000`, perform eight reads from CONFIG_DATA, restore the old address.

The code then writes `0x408` once using shared `+0x503c`. It releases the lock,
handles a contended-owner republish case, and clears the local pending count.
Flag bit 1 invokes an additional helper after publication. Flag bit 2 checks
the fetch relationship and can pulse the bus-master wake state (`0x20000`),
but it is not a blanket render-completion wait.

Initialization at `0x32a20` selects alignment 0, 8, or 16 dwords from memory
type/capability data. It derives flags `0x100` and `0x200` from device/platform
capability bits. Restart at `0x328d0` waits for idle/wake conditions and writes
`0x404` followed by `0x400`; subsequent publication uses `0x408`.

The Linux object has no unresolved `dma_sync_*`, `clflush`, `wbinvd`, or other
cache-maintenance import. The evidence supports an ordering barrier matched to
the mapping type before the producer doorbell, not an unconditional GPU-cache
invalidate per draw. For the open coherent primary/secondary allocations,
`dma_alloc_coherent` plus `dma_wmb()` before `0x408` matches that contract. The
Mesa local-VRAM vertex map is potentially write-combined and therefore needs
its existing store-draining barrier before the ioctl/doorbell path.

No recovered packet justifies inventing a texture-cache invalidate here. GPU
cache/domain transitions should remain tied to already proven EOB/drain
sequences and resource dependencies.

## 6. Mapping to the current open stack

The current kernel already implements the vendor's central fast-path rule:

* one coherent persistent async primary allocation;
* append at `primary_put`;
* `dma_wmb()` before publication;
* write `0x404` only when arming an empty allocation;
* advance subsequent submissions with `0x408`;
* retain secondary, vertex, BO pins, and GEM references in a submit slot until
  terminal `DWGSYNC` completion.

The remaining throughput limits are software queue bounds:

* `PARHELIA_ASYNC_SLOTS` is 3 and Mesa's fenced local-VRAM vertex ring also has
  3 subranges.
* Each async submit consumes one slot even when its primary span is small.
  A fourth submit blocks in `wait_event_interruptible_timeout` until retirement.
* Retirement polls `0x41c` from delayed work every 1 ms. This kernel is
  `CONFIG_HZ=1000`, so the nominal poll is one jiffy rather than a hidden 10 ms.
* The primary span has storage for exactly three maximum slot reservations and
  deliberately never wraps while fetch is live. `primary_get` advances to
  `primary_put` only when the entire queue drains; individual completed spans
  are not reclaimed.
* Mesa's production state stream aggregates until 221 packets, 4096 command
  dwords, its vertex limit, or its resource-reference bound. Each drain is one
  ioctl and one independently owned terminal fence. State-stream mode itself
  uses no internal logical boundary; the older stateful aggregation ABI admits
  only one boundary (two logical sections).

The 3-slot choke can add recurring CPU stalls if ioquake3 produces more than
three GPU chunks before the oldest fence retires. It does not alone prove an
exact 30-FPS cap: at a 1 ms retirement cadence, a stable 33.3 ms frame period
also warrants checking swap/present/vblank cadence and actual GPU execution
time. Instrumentation must separate these causes.

## 7. Conservative integration plan

### Phase A: measure without changing packet semantics

Add per-device debug counters/tracepoints for:

* accepted async submits and `0x408` writes;
* waits caused by all slots busy, primary-tail exhaustion, vertex-subrange
  reuse, and explicit front-buffer/CPU waits;
* nanoseconds spent in each wait and observed queue high-water mark;
* `0x404` restarts, primary dwords appended, and delayed-retire passes;
* Mesa aggregate drains by reason: packets, command dwords, vertex dwords,
  resource references, clear/blit ordering, transfer/present, and explicit
  flush.

Record ioctls/frame, waits/frame, fence latency, and FPS for glxgears with
vsync disabled and ioquake3. This tells whether the fourth-submit sleep is the
limiter or only background overhead.

### Phase B: deepen the existing queue, preserving one submit per slot

Raise the kernel slot count and Mesa fenced vertex-subrange count together,
initially from 3 to 8. Expand the linear coherent primary to the same number of
maximum slot reservations. Keep all current validation, terminal fences,
per-slot storage, BO ownership, `dma_wmb()`, and producer-only `0x408` behavior.

This is safer than implementing the vendor turnover record immediately:
hardware sees the same proven linear CALL stream, no live allocation wraps,
and each chunk retains its own existing fence lifetime. The cost is modest
coherent metadata/storage and a longer maximum queued workload. Bound timeout
accounting per slot as today and retain wedge quarantine.

Do not advance `primary_get` merely when an arbitrary slot completes. FIFO
submission means retirement should be consumed in submission order; the
current loop can observe a later tag that implies earlier completion, but a
cyclic allocator also needs ordered span bookkeeping. With a linear primary,
`primary_get` can remain diagnostic until the queue drains.

### Phase C: batch multiple bounded chunks in one ioctl

If measurements show ioctl/validation overhead after queue deepening, add a
new bounded chain ioctl/capability instead of enlarging one vertex payload. A
chain contains, for example, at most eight ordinary submit descriptors. Each
child retains the current independent limits (221 user packets from Mesa,
bounded command dwords, and its own vertex range/subrange).

Kernel algorithm:

1. Copy and validate every child, resolve/pin every BO, reserve all required
   slots/secondary heaps/vertex storage, and construct all dma-resv fences
   before any doorbell. Failure before publication has no GPU side effects.
2. Under `submit_lock`, assign monotonically ordered terminal tags and append
   every child's proven five-dword CALLs and terminal marker to consecutive
   primary space.
3. Execute one `dma_wmb()` and one final `0x408` producer update for the whole
   chain. Write `0x404` only if the primary was unarmed.
4. Return either each child fence or one terminal fence. If returning only the
   terminal fence, it owns all child storage and BO references; intermediate
   execution brackets still stay wherever state/depth correctness requires.
5. On timeout, quarantine all unretired chain children exactly as current
   slots are quarantined.

Mesa can accumulate already bounded `parhelia_batch` chunks in a small chain
array. A CPU visibility boundary, clear/blit dependency, present, transfer
map, explicit flush, or chain-capacity limit drains the chain. This reduces
syscalls and producer writes without combining vertex data beyond the known
per-chunk range.

### Phase D: only then consider cyclic primary turnover

The vendor's `0x21272425` record is promising, but its status address and
turnover semantics need a controlled vendor capture or a disposable-target
probe. Until that is proven, retain linear no-wrap primary allocations and
rearm at zero only after all queued work retires and `0x8f0` bit 17 confirms
idle. A larger linear allocation makes this rare without guessing packet
semantics.

## 8. State-shadow and invalidation rules

The open register shadow may span queued submits only while all producers are
serialized by `submit_lock` and validation accounts for every register write.
Commit predicted state only after the doorbell is accepted. Invalidate the
software shadow after:

* engine reset/restart, timeout, or wedge;
* any diagnostic/raw submission path that can write untracked state;
* ownership transfer to another untracked client/engine class;
* a failed submission after partially changing hardware state.

Do not invalidate it merely because a fence completed; FIFO completion does
not change register state. Do not call CPU DMA-sync APIs for coherent command
allocations. Do not add per-draw GPU-cache invalidation without a recovered
packet and a test demonstrating the required domain transition.

## 9. Practical conclusion

The vendor fast path is persistent and fine grained: append CALLs, order CPU
stores, publish `0x408`, and fence reuse of backing storage. The current open
driver already follows that hardware protocol. The next low-risk performance
work is to measure and remove its three-slot software back-pressure, then add
a bounded multi-chunk ioctl that preserves existing 4K/range limits and emits
one producer update. Primary cyclic wrap should remain deferred until the
four-dword turnover record is validated on hardware.
