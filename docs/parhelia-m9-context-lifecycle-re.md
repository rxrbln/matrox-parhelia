# Matrox M9 ("Phoenix") requester and context lifecycle

This note records the static context-switch and completion contract recovered
from the saved Linux `KernelLibraries.o_shipped` and Windows `MxMD.dll` /
`MxMM.sys`. Addresses are binary virtual addresses for Windows and symbol plus
offsets for the Linux relocatable object. `[BIN]` means an operation is visible
directly in disassembly; `[DER]` is the narrow interpretation supported by
those operations. No hardware was used for this analysis.

## Requester descriptor ownership

The complete direct-register reference sets in both saved kernel components
put a useful limit on `0x50c..0x524` [BIN]:

| operation | Linux | Windows | registers |
|---|---|---|---|
| initialize | `m9x759` @ `0x2b290` | `0x145a5` | writes `500,508,504` |
| publish live pointer | `m9x620` @ `0x285d0` | `0x146bc` | writes `504` |
| quiesce | `m9x1016` @ `0x28960` | `0x146ee` | reads `508,500,50c` |
| save | `m9x399` @ `0x2b390` | `0x14821` | reads `500,504,508,50c,510,514,520,524` |
| restore | `m9x714` @ `0x2b5d0` | `0x1490c` | writes the saved eight-word image |

There is no software producer for `0x50c`, `0x510`, `0x514`, `0x520`, or
`0x524` outside restoration in either binary. The drivers synthesize only the
initial `0x500/0x504/0x508` values and subsequently preserve the other five
words as opaque hardware state [DER]. This excludes several unsafe driver
designs: those five words must not be initialized from guessed ring pointers,
recomputed at wrap, or repurposed as software fences.

The observable relationships remain exact:

- `0x50c` participates with `0x500` in the two-sample quiescence stability
  test.
- Windows compares `(saved_50c ^ saved_510) & ~0xf`; only a difference above
  the low tag nibble causes the saved `50c,514,510` triple to be restored.
- `0x520,0x524` are restored first and unconditionally.
- `0x500,0x508,0x504` are restored last, after the conditional triple.

Static code therefore cannot supply defensible producer/consumer names for
the five hardware-owned words or their rollover relationship to the `0x404`
submit limit. A rollover capture is required. The implementation-safe ABI is
an eight-dword opaque requester snapshot with the vendor save/restore ordering,
not five invented fields.

## Windows TaskMgr switch ownership

The `MxMD.dll` source-path string identifies the implementation as
`phoenix/taskmgr/tktaskmgr.cpp`; its vtable is at `0x1f48e8`. Initialization
at `0xf8120` and switching at `0xf8fd0` establish these roles [BIN]:

| field | established role |
|---:|---|
| `TaskMgr+0x04` | command-buffer/submission object used by every state writer |
| `TaskMgr+0x5b8` | current live context pointer |
| `context+0xa8` | backlink to the TaskMgr while that context is current |
| `TaskMgr+0x5c0` | accumulated state-dirty bit mask, not a context pointer |
| `TaskMgr+0x5e4..0x5ec` | first vector of referenced synchronization objects |
| `TaskMgr+0x5f4..0x5fc` | second vector, handled with access-class argument 1 |

When the incoming context differs from `+0x5b8`, `0xf8fd0` performs this
ordered transition [BIN]:

1. If an old context exists, pass its state object's `+0x58/+0x5c` pair to
   `0xfc4b0`, serialize any `old->c4` dirty block through `0xfce50`, and clear
   `old+0xa8`.
2. Set `incoming+0xa8 = TaskMgr`, prepare it through `0xad170`, propagate one
   saved dirty relation when `incoming+0xb0` is set, and store incoming at
   `TaskMgr+0x5b8`.
3. If incoming state `+0xc4` has bit 2 set, serialize its corresponding block
   through `0xfcb60` and clear that bit.

The main state method `0xf85c0` compares its incoming context with `+0x5b8`,
calls the switch method when they differ, and writes all subsequent restore and
render-state records through `TaskMgr+0x04`. Thus the TaskMgr submission
object owns restore emission; a context supplies saved values and carries the
temporary backlink, but it does not submit an independent restore buffer
[DER].

This also gives the static alternating-context rule. Switching A to B emits
A's pending state serialization before B becomes current, then emits B's
restore/state into the same ordered command stream. The binary does not use
`TaskMgr+0x5bc` as an alternate current-context slot: it is zeroed by the
constructor and initialization cleanup, and no live producer for it exists in
this build. Completion between alternating contexts must therefore use stream
ordering and the synchronization objects below, not a guessed A/B pointer
toggle [DER].

## Submission fences and resource lifetime

Windows method `0xf9b70` first obtains a 64-bit submission value from
`TaskMgr+0x04` vtable slot `+0x74`. It associates that same value with every
object in the first reference vector by calling `0xfadc0(..., class=0)`, and
with every object in the second vector using `class=1`, before invoking the
submission object's `+0x94` method [BIN]. At the tail of `0xf85c0`, the driver
finds the maximum 64-bit dependency returned by objects in the first vector
and passes it to submission-object slot `+0x6c` [BIN]. These are ordered
resource dependencies and retirement values; neither operation reads an IRQ
completion counter [DER].

Linux independently exposes the same ownership pattern [BIN]:

- `m9x954` calls its submission vtable slot `+0xd0` and retains the returned
  value while walking the referenced-resource vector at `+0xf8..+0x100`.
- For each type-zero resource it follows `resource+0x38`, then `+0x228`, and
  stores that submission value at backing object `+0x60`. It decrements every
  resource's in-flight reference count at `+0x74` and clears cached aliases at
  `+0x118`, `+0x140`, and `+0x168` when they refer to the retired vector entry.
- Optional final submission through `m9x1039` stores the returned fence at
  command object `+0x1a8`. Completion waits compare the GPU-written memory
  tag, with 32-bit wrap extension, as documented in `parhelia-m9-dma.md`.

The restore pool follows that lifetime rather than alternating blindly.
`m9x1160` rebuilds it when the configured count changes; construction requests
`0x400` entries. `m9x206` scans entries, admits an entry only through its
availability/refcount checks, invokes its acquisition method, and increments
the in-flight count. `m9x852` then installs the selected backing and its
bounds. A new driver should similarly keep a restore slot unavailable until
the fence attached to its backing object has retired.

## Query boundary

The Windows configuration reader at `0xb9386..0xb93a4` reads the literal
`Queries`, supplies default value `3`, and stores the result at configuration
object `+0x54` [BIN]. That proves a configurable query pool/count exists in
this build. It does not identify an occlusion-result record, query opcode, or
a relationship between query state and the two TaskMgr synchronization
vectors. The saved Linux WGL strings `PreRestore`, `Restore`, `PostRestore`,
and `EqualizeDMATag` also resolve only to assertion stubs in this build and do
not add a callable query/context contract.

Consequently, ordinary context isolation, ordered restore emission, resource
fencing, and restore-slot reuse can be implemented from static evidence.
Exposing hardware queries still requires a command capture that joins begin,
end, result address, and readiness semantics. Software emulation or a disabled
query capability is safer than treating a requester descriptor word as query
state.

## Driver requirements derived from this evidence

1. Keep one kernel-owned current-context pointer per TaskMgr/engine queue and
   serialize outgoing state before installing incoming state.
2. Emit save/restore and ordinary rendering through the same command queue so
   their order is intrinsic.
3. Attach the resulting memory-tag fence to every referenced backing object;
   release object references and recycle restore slots only after retirement.
4. Preserve `0x50c..0x524` as an opaque hardware snapshot and use the exact
   vendor restore order.
5. Validate A/B/A switching with distinct render state and fence words before
   enabling multiple hardware contexts. Validate query commands separately.
