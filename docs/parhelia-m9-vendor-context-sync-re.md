# M9 vendor context, dependency, and retirement contract

This note records the statically recovered ownership model from the saved
Windows `MxMD.dll` and Linux `KernelLibraries.o_shipped`.  It is the reference
for implementing the clean-room DRM scheduler.  Capture labels such as
`seq=0` are tracer ordinals, not named hardware operations.

## Engine and context lifetimes

Every complete process trace contains one initial `ExecuteDma` with an
`0xfa0`-byte secondary image.  It consumes nine timeline values, patches three
context-private scratch VRAM addresses and the completion address, and is
followed by smaller incremental submissions.  The three scratch allocations
therefore remain owned by the context until its last job retires.  Replaying
the initial image before each draw is inconsistent with every complete trace.

The following operations have engine lifetime and must not be repeated for a
draw or context switch:

- service construction;
- both cold `m9x421` address-window transitions;
- construction of the 0x400-entry restore pool;
- the `m9x109` requester arm.

A new context receives a full default/dirty-state emission on its first
activation.  A normal context switch appends outgoing serialization, incoming
restore, and the draw to the same ordered GPU stream without a CPU completion
wait.  A lost engine generation invalidates every context's initialized state.

## Windows TaskMgr evidence

The relevant `MxMD.dll` image base is `0x10000`; the TaskMgr vtable is at
`0x1f48e8`.

- Constructor `0xf8120` stores the shared submission object at `+0x04` and
  clears current context `+0x5b8` and unused field `+0x5bc`.
- Destructor `0xf8070` clears the current-context backlink and releases the
  vectors at `+0x5e4..+0x5ec` and `+0x5f4..+0x5fc`.
- Main path `0xf85c0` compares the incoming context with `+0x5b8` and invokes
  the switch path when they differ.
- Switch `0xf8fd0` serializes the old state through `0xfc4b0` and `0xfce50`,
  clears the old backlink, attaches and restores the incoming context through
  `0xad170`, handles dirty bit 2 through `0xfcb60`, and finally updates
  `+0x5b8`.  There is no completion wait and no live producer for `+0x5bc`, so
  the binary does not support an inference of alternating hardware contexts.

Textures reach `0xfaa40(arg=1)` from emitter `0xf4350` and enter sampled/read
vector `+0x5e4`.  Surfaces reach `0xfaa40(arg=0)` from emitter `0xfb920` and
enter render-write vector `+0x5f4`.  The tail of `0xf85c0` queries the class-1
producer value of every sampled object, takes the maximum 64-bit value, and
calls submission slot `+0x6c`.  Implementation `0x25270` emits command index
`43` with the low 32 bits only when the value is newer than its cached value at
`+0x100/+0x104`.

Commit `0xf9b70` obtains one 64-bit submission value through vtable slot
`+0x74`, publishes it through `0xfadc0` to sampled objects with class 0 and
written objects with class 1, then invokes submit slot `+0x94`.  The fence is
therefore published to every job resource before the doorbell.  A later
sampler reads the producer class and emits a GPU-side dependency; the vendor
does not serialize ordinary dependencies with CPU waits.

## Linux retirement and restore ownership

Linux `m9x954` at `.text+0x2d920` obtains the submission value when either task
resource vector is nonempty.  For each typed entry in the first vector, type 0
follows `entry+0x38` to `backing+0x228` and stores the fence at `backing+0x60`.
It decrements the in-flight reference at `entry+0x74`, clears cached aliases at
task offsets `+0x118/+0x140/+0x168`, resets the vector, clears task byte
`+0x78` bit `0x80`, optionally performs the terminal submit through `m9x1039`,
and saves that fence at task `+0x1a8`.  `m9x528` at `.text+0x2dd40` inserts the
current resource at task `+0x110` into the task vector and sets bit `0x80`.
The second vector's exact omission rule is not closed, so the clean-room
driver must fence every typed job BO.

`m9x1160` at `.text+0x37330` rebuilds the restore pool only when its configured
count changes.  `m9x206` at `.text+0x37410` scans it cyclically and only reuses
a slot with zero in-flight references after the backing-availability checks.
`m9x852` at `.text+0x37530` selects a slot, installs it at `+0x38`, copies the
bounds, and tracks it as current at `+0x50`.  Command slots, restore slots,
context BOs, and their pins must consequently live through the output fence.

## Completion and recovery

Completion is the coherent word selected through MMIO `0x106c`, extended
across 32-bit wrap.  Linux `m9x641` at `.text+0xec20` implements a nonblocking
`target > completed` test and rejects `0x8f0 == 0xffffffff`.  Blocking paths
`m9x721` and `m9x946` poll the two fence domains and kick `0x314=1` while
incomplete.  The saved implementations have no timeout.  `m9x1039` pads the
stream, posts the end-exclusive address to `0x404`, waits as needed, and saves
the returned fence.

No callable 3D/global reset is proved in either saved stack.  The clean-room
deadline policy must stop new jobs, snapshot the fault, error queued fences,
perform at most one bounded `0x314` progress kick, and test bounded quiescence.
If the requester cannot be proved stopped, it must clear PCI bus mastering and
quarantine every reachable BO and command/restore slot.  Such storage cannot
be freed, unpinned, or reused in a new generation.

## DRM implementation contract

Use one serialized scheduler queue and one current-context pointer.  Collect
`dma_resv` dependencies before taking the hardware queue lock: a reader waits
for the prior writer, while a writer waits for prior readers and writers.
Reserve and publish one output `dma_fence` on every job BO before the doorbell,
using write usage for render/depth destinations and read usage for sampled
resources.  Append context transition and draw records atomically under the
queue lock.  Do not sleep on a reservation or completion while holding that
lock.

For an ordinary job, preserve the tested `0x31c` transition, execute
`dma_wmb()`, write the end-exclusive address to `0x404`, and perform the
posting readback.  Rewrite `0x400` only when moving to a different retired
primary command slot.  Signal the software fence only when the coherent
completion tag reaches the job target, then release the associated resource,
command-slot, and restore-slot references.
