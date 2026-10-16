# M9 bounded queued-submit design checkpoint

2026-10-06; design, not executable queue support. The committed completion and
implicit-fence work retains one in-flight job. Native qualification of that
slice precedes a queue deployment. Current Q3 baseline is 56.6 FPS at
1024x768/vsync 0, around 55 jobs/frame and 7.5 ms/frame ioctl time; no queue FPS
improvement has been measured.

## Evidence and smallest safe queue

The retained genuine Linux vendor ExecuteDma captures show primary BmBuffer
appends and endpoint doorbells with ownership/ring switching. Normal submission
does not require a CPU writeback drain after each draw. Existing class-1
secondary framing and completion records are native-proved, so a four-job queue
can reuse those packet forms while giving each job an independent command
image. No opaque auxiliary `0x500` bank or uncaptured ring switch encoding is
needed for the first implementation.

Kernel user-submit currently overwrites one 32 KiB secondary, then appends
primary `2c/2d` range references. Two 32 KiB primary slots do not make that
secondary independent. Mesa alternates two sets of four CPU-written arenas
before filling a job. Removing the next-submit drain would let job three's
CPU producer overwrite job one's input. A new kernel must retain compatibility
with older Mesa: queuing needs an explicit new submit opt-in, admitted by a
validate-only probe, rather than silently changing flags-zero execution.

The first bounded implementation needs four independently allocated/mapped
32 KiB user secondary banks and a four-entry FIFO. Each FIFO entry owns its
target/extended point, primary slot/range, secondary slot, true implicit fence,
BO/pin vectors and preallocated retirement node. The existing diagnostic
secondary keeps its original exact 32 KiB extent and captured initializer.
User banks must use real compact-PTE GPUVA, not raw DMA backing addresses.
The diagnostic's 44-PTE bookkeeping limit must not be expanded by conflating
its source/system/secondary set with the new user-bank mappings.

Allocate/map every bank before publishing work. Append while a bank and primary
space are available. Queue-full backpressure waits for the oldest entry and
reuses only its retired bank. Every existing operation that requires engine
quiescence (diagnostics, translation edits, shared CPU clears, primary buffer
switch, synchronous NOOP, final teardown) drains the whole FIFO. The first
version can retain a full drain at primary-slot rollover; a vendor ring-switch
packet optimization can follow separately.

## Completion with more than one target

The current exact-equality waiter is correct for one owner, but can miss an
older queued target once a later job overwrites the common writeback. FIFO
retirement needs an observed monotonic high-water mark with explicitly bounded
32-bit wrap arithmetic. Each job's existing final completion/footer remains
unchanged. Dependency tokens are bounded by `PARHELIA_M9_DEPENDENCY_LIMIT`
`0x400`; four jobs' maximum token span stays far below the signed half-range.
The implementation must reject any span violating that bound and distinguish
the status nibble from the target. WAIT_POINT must retire all actually observed
FIFO entries up to its point; it cannot merely wait for the newest submitted
target or treat a returned point as completion.

Required offline proof includes skipped intermediate writeback observations,
status bits, wrap at `0xfffffff0`, an unfinished head, queue-full backpressure,
bank reuse only after observation, foreign dependency errors, unposted errors,
last-job automatic completion, timeout quarantine and primary-slot rollover.
Native proof then checks queued geometry/matrix A-B-A, a longer multi-job input,
depth, ordinary texture, and a single short 1024x768 Q3 regression/profile run.

## CPU writer ownership audit

| Object | Current owner/use | Queue requirement |
| --- | --- | --- |
| source/system/index/cpu_streams | CPU-filled, two arena sets per context | Five sets: four posted readers plus one CPU builder; each set has a last submitted point and is observed retired before CPU write/growth |
| userspace command BO | Kernel copies validated image before return | No GPU bank required for this copied staging image; keep context serialization |
| native VS transient views | GPU preprocess writes, raster reads in ordered job | May remain shared with proven final completion/order; kernel per-job refs/pins retain lifetime |
| per-screen inter-pass record | GPU-only split-pass scratch | Keep ordered access and existing intra-job dependencies; do not CPU clear in flight |
| per-target BE | CPU memset, currently two target slots; default context BE can also be CPU-cleared | More retirement-aware slots or wait for the bound object's last reader before memset; two-slot shape rotation cannot be assumed safe with four pending jobs |
| shader/fragment/program BOs | Normally immutable GPU-read code | Keep per-job refs/pins/fences; any patch/upload requires retirement before CPU modification |
| query result slots | GPU writes and CPU pending-sentinel initialization | Track slot/page reuse; pending sentinel writes cannot overwrite a queued query's result |
| render/texture/depth/HZ surfaces | GPU access, occasional CPU upload/window access | Ordered own fences plus foreign hazards; CPU/window paths must drain/wait all relevant readers before writing |
| FAA groups/store/aux/pool | Shared context state plus initial/growth CPU writes | Keep FAA command/lifetime proof separate; all CPU changes need complete ownership, not a blanket queued exception |

Mesa arena rotation must select an actually retired bank before writing one
byte. If none is free it waits the oldest required point through WAIT_POINT;
this is bounded backpressure, not a guessed delay. Destruction or growth of an
old arena must also respect its point. GPU-only local scratch can stay shared
because jobs execute in the same ordered primary stream, while CPU-modified
objects need explicit retirement. The existing implicit fences make foreign
consumers and the normal KMS prepare_fb dependency possible, but do not prevent
an application's own mmap producer from overwriting an in-flight input.

Queue capability, command slots, ownership FIFO and Mesa banks should land as
one coherent opt-in feature. Keep the current single-job path available for
older kernels/clients and diagnostics. Do not turn queue support on solely
because a Boolean reports async posting.

## 2026-10-06 opt-in implementation checkpoint

The isolated kernel candidate is based on canonical `4b84cbc`, including the
WAIT/implicit-fence fixes, native D24 depth-bias grammar, and the root's typed
FAA validation-only families. The isolated Mesa candidate is based on
`a9c5668`, preserving both the depth-bias capability probe and ETIME-compatible
WAIT admission. Native deployment and queue throughput are **not yet proved**.

The new `PARHELIA_M9_SUBMIT_QUEUED` bit is `1 << 3`. Flags zero keep the original
single outstanding job, two Mesa input sets, and the following-submit drain.
Only `QUEUED` or `VALIDATE_ONLY | QUEUED` are new accepted combinations; the
kernel refuses queued admission when `async_submit=N`. Mesa defaults to the
legacy path and enables the candidate only for `M9X_QUEUE=1`, successful flagged
validate-only admission, an available WAIT ioctl, and `screen->faa.enable=false`.
The flagged capability probe submits no GPU work. Old kernels reject the bit
and Mesa retains ordinary rendering.

Four additional exact 32 KiB, zero-tailed DMA32 secondary images are allocated
and mapped through the existing compact class-1 PTE owner before user BO WW
reservation. They have independent mapping nodes and do not alter the captured
diagnostic image or its 44-PTE accounting. A FIFO entry retains each job's real
fence, BO/pin arrays, preallocated retirement node and selected secondary bank.
The primary still appends the existing captured range grammar and footer. On
primary-slot rollover it drains the whole FIFO before changing `0x400`.

The writeback observer extends its low-nibble-masked value relative to the last
actually completed 64-bit final point. It rejects stale/future observations,
retires every FIFO final point passed by a valid observation, and never
publishes an intermediate dependency token as completion. Admission rejects
nonaligned/nonmonotonic issue points and an outstanding span at least `2^31`.
A full FIFO waits its oldest point; the existing short spin optimization is
retained before sleeping. WAIT can complete an earlier point while a newer
owner remains outstanding. The completion worker observes the FIFO even when
no following job/readback occurs and tracks its oldest owner for the watchdog.
A post-doorbell failure or watchdog timeout retains every reader and its
unsignaled fence. Teardown stops DMA, signals all quarantined queue fences,
then releases any shared BO; this preserves TTM's last-put reservation-wait
contract. Successful retired BO releases remain outside the transport lock.
The first increase to each queue high-water value is logged once per device.

Mesa adds three parked sets to the existing active/alternate arenas, for five
sets total. Rotation waits the selected set's actual last point before swapping
it into the CPU builder. Source/index growth therefore occurs only after that
set retires. Every successful raster-pass submission updates the same set's
point, so a later pass cannot outlive the bank ownership. A failed wait loses
the context without granting overwrite permission. CPU BE clears, query
slot/page reuse, synchronized resource maps and direct CPU surface/depth clears
wait the context's prior submissions. Existing kernel window/translation edits
still drain all jobs. GPU-only transient/inter-pass scratch stays ordered by
the captured footer. FAA queueing remains deferred to its separate ownership
proof. The opt-in mode does not grant applications permission to overwrite
UNSYNCHRONIZED maps or omit cross-context synchronization.

Offline receipts:

- Full matching Linux 7.2 external module build PASS; existing pahole/objtool
  warnings retained. No new loaded-module or GPU success claim.
- Full Mesa batch suite PASS; changed context, screen and query objects compile.
- `tools/m9-completion-check.py <kernel>/m9x <mesa>`: **131 actual-function
  ASAN/UBSAN checks PASS**, including four owner transfers, skipped observations,
  full-queue backpressure, wrap, stale/future/status tokens, posted/unposted
  errors, automatic last-owner completion, all-owner quarantine/teardown and
  actual requester append/primary-switch ordering. Hardware writeback is a
  controlled test input, not a vendor capture or native completion.
- `tools/m9-queue-arena-check.py <mesa>`: **68 actual Mesa rotation/WAIT
  ASAN/UBSAN checks PASS**, including five distinct sets, reuse only after
  retirement, a failed wait granting no writable bank and the legacy path.
- New `m9-mesa-glsl vsqueue` / `vsqueued` emits 49 flushed textured matrix jobs,
  ending A/B/A with changed final source colors, followed by GLFinish before
  pixel validation. Both independent llvmpipe references PASS **0/76800**.
  The `d` suffix supplies ordinary depth testing. Native result is pending.

Sole-owner native qualification at a controlled kernel checkpoint: first run
ordinary queue-disabled texture/depth and the existing completion fixture;
then enable `M9X_QUEUE=1` with FAA disabled for `vsqueue`, `vsqueued`, ordinary
texture and one query reuse case. Confirm the driver's queue high-water log,
zero skipped/refused jobs, WAIT progress after GLFinish and a healthy subsequent
texture draw. Only after those controls pass use one short 1024x768 fullscreen
Q3 ballpark/profile run. Do not repeat the baseline benchmark. Retain the
candidate and its proof if FPS is equal; default enablement awaits native
ownership evidence. Fullscreen page flipping is separate: root observed actual
M9 DC_START changes with PageFlip=true, but DP enable reported sending-nothing
`-67`, so physical fullscreen presentation is not closed by that register proof.

### Queue review correction: immutable shader images

Root review found `m9_native_vs_c2` reusing and CPU-rewriting a sufficiently
large fragment-owner image when the vertex shader serial changes. A queued
reader can still use those bytes. There is also an unsubmitted-owner hazard:
this helper runs before `m9_native_vs_append`/job reservation flushes a changed
key, so merely destroying/replacing its cached handle can invalidate a prior
recorded draw.

The queue-only correction retains immutable native C2 images keyed by VS
serial in the FS variant (or the context for the fixed captured fragment
fallback). Each new serial builds a fresh owned BO; A/B/A finds the original A
handle without rewriting or reallocating it. The cache survives earlier
recorded and posted jobs and is released with its existing owner. Kernel
per-job refs/pins retain any submitted images beyond userspace owner release.
Legacy sufficient-size image reuse is unchanged. Cold BO creation still goes
through the kernel's conservative allocation drain; this correction adds no
explicit CPU completion wait and warm shader switches reuse the immutable
images. Images cost one BO per compiled VS serial for that owner.

The adjacent CPU shader/image audit found fragment C2 upload allocates a fresh
BO for each new FS variant, while fixed program/database uploads occur only in
new context resources. Arena/constants uploads use retirement-owned sets;
BE clears and query reuse use explicit queue waits. Texture/depth window
writes retain kernel drain; direct synchronized CPU maps/clears wait. FAA is
still excluded. The native C2 sufficient-size rewrite was the additional
shared program upload that needed a separate owner.

`tools/m9-queue-program-check.py <mesa>` extracts the actual image helper:
**14 ASAN/UBSAN checks PASS**, including distinct A/B images, unchanged earlier
bytes and handles, A/B/A cache reuse, FS/context owners, allocation/compiler/
cache-growth failure cleanup and legacy reuse. Controlled image generation is
not a native ISA or hardware proof. Full batch suite/context build still PASS.

`vsqueueprog` and `vsqueueprogd` use two genuine ARB vertex programs (A full
color, B half RGB) and one unchanged fixed-function MODULATE textured fragment
state, rather than two linked GLSL programs that might own separate fragment
variants. They alternate over 49 flushed matrix-positioned draws, finish with
A on the first tile using changed source colors, and GLFinish before readback.
Both independent llvmpipe pixel references PASS **0/76800**. Native proof must
also confirm actual GPU transforms and the debug `queued C2 owner` lines show
multiple serials in the same owner cache; a CPU fallback pixel pass alone
cannot close this hazard. No native queue or shader-switch success is claimed.
