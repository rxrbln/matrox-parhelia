# Vendor fine-grained submission, state caches, and fence cadence

This note records what the saved Matrox Linux and Windows binaries actually do
at command-buffer boundaries.  It also defines the deliberately narrow first
implementation of different-state batching in the open driver.  Addresses are
file virtual addresses from the checked-in disassemblies/binaries; the evidence
does not depend on symbol names reconstructed later.

## Sources

* Linux object:
  `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`
* Linux disassembly:
  `scratchpad/mtx_parhelia_ko.asm`
* Windows display driver:
  `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`

The Windows OpenGL ICD has its own API-state cache, described separately in
`parhelia-vendor-state-cache-q3-optimization.md`.  The display-driver evidence
below concerns the lower command/state layer.

## Linux primary-ring record and kick path

Linux routine `mtx1937` starts at `.text+0x32360`.  Its normal path demonstrates
that recording, publishing, and waiting are separate operations:

* `0x323ab..0x323bd` reads the primary write pointer from object offset `+0x60`
  and tests whether `write + 0x14` fits below the limit at `+0x70`.
* When it fits, `0x323ce..0x323e3` records five dwords:

  ```text
  20292820 00000000 start|flags end|flags 00000000
  ```

  This is a secondary-buffer call record, not an MMIO replay of every state
  register.
* `0x323f8..0x3244b` adds the bus-minus-virtual bias stored at `+0x9c` to the new
  primary write pointer and publishes the result once through MMIO `0x408`.
* The normal path does not poll `0x41c`.  Its only pre-submit bus gate is the
  conditional path at `0x32473..0x324f0`, selected when the incoming end/flag
  argument is zero.

Consequently, a vendor command record normally costs one primary-ring append
and one producer update.  Completion polling is not an unconditional part of
every record.

Linux routine `mtx2095` at `0x31fc0` handles ring-storage turnover.  With more
than one buffer it scans the buffer list for a free entry, marks it owned,
records the four-dword switch command beginning with `0x21272425`, changes the
active pointers, and kicks the producer.  The one-buffer fallback writes the
producer and then performs the full bus-master wait.  Thus capacity/wrap is a
flush trigger, while a synchronous wait is a resource-shortage fallback rather
than normal fence cadence.

The primary ring is persistent: the write cursor advances and storage slots are
reused after their ownership/completion condition clears.  Routine `0x328d0`
in the Windows driver shows the corresponding exceptional restart operation:
it polls status register `0x8f0` bit 17, writes the restart address to `0x404`,
then updates `0x400`.  That is evidence against restarting the DMA engine for
each ordinary submission.

## Fence records and cadence

Linux `mtx3144` at `0x31c30` writes this nine-dword synchronization record at
`0x31c67..0x31ceb`:

```text
21c0c0c0 00000307 00000004 00000707 c0afafdd
00000000 tagA|1 tagB|1 saved_DWGCTL
```

The Linux allocator advances its tag values by eight and skips zero.  The
Windows equivalent at `MTXPARD.dll+0x32b60` advances by `0x10`.  These increments
are software tag-allocation policy, not proof of different fence semantics.
Both binaries place the synchronization record at a logical command boundary.

The open async queue uses an opening tag and a terminal tag for each logical
submission.  Its established sequence is:

```text
opening(N) = terminal(N-1) + 8
terminal(N) = opening(N) + 16
```

Combining two logical submissions into one ioctl may coalesce the final primary
producer write.  It must still allocate and emit both logical fence brackets;
otherwise state from the second record can cross the first record's required
EOB/depth drain.

## Linux hardware-state shadowing

Linux routine `mtx1342` at `0x90010` is a dirty constant/state emitter:

* `0x9004e..0x9005a` OR six 32-bit dirty words.  The six words cover 192 bits,
  while their paired backing layout covers the observed 384 constant values.
* The scan emits only dirty indices.  At `0x901f0..0x90218` it groups adjacent
  entries, with a maximum run of 63, into opcode-`0x41` records.
* Six 64-byte redirect records begin at constant-object offset `+0xb0`; the
  corresponding dirty word selects whether the emitter uses the valid backing
  block.

Linux routine `mtx3712` at `0x188f0` supplies the other half of this mechanism.
It derives grouped hardware values, compares them with the device shadow, and
sets dirty state only when the value changes.  One concrete instance at
`0x18c53..0x18c63` compares a calculated pipeline word with the shadow at
`device+0x40+0xb08`; the dirty byte at `+0x18` is set only on mismatch, after
which the shadow is replaced.

This is a value-aware hardware cache.  It is safe only when the software shadow
is known to describe the state that will precede the new record.  Reset, engine
reinitialization, failed/aborted submission, or execution by another owner must
invalidate it or replace it from the new owner's complete state.

Routines `mtx3333` (`0x328e0`), `mtx3257` (`0x32f20`), and `mtx398`
(`0x32920`) cover an observed class transition.  The discriminator is device
field `+0xd8`, with observed classes 1 and 3.  On a change, `mtx3257` calls
`mtx398`, which emits a full default state surrounded by fence/drain sequences.
This is firm evidence for engine/class invalidation.  It is not, by itself,
proof that the same routine schedules arbitrary OpenGL contexts.  The registry
string `FastContextSwitching` is likewise insufficient evidence: it is
registered near `0x10ae6a` and stored at configuration offset `+0x138`, but no
functional consumer has yet been established.

## Windows dirty groups and state arm

`MTXPARD!ValidateState` starts at `0xc9ac0` and dispatches dirty groups through
calls at `0xc9ad1`, `0xc9add`, `0xc9ae9`, `0xc9afc`, `0xc9b08`, `0xc9b15`, and
`0xc9b22`.  The surface/arm group at `0xc98b0..0xc9ab0` is especially useful:

* Surface groups are emitted conditionally for dirty bits `4`, `8`, `0x10`,
  `0x20`, and `2`.
* The high/sign dirty class advances the shared tag counter by `0x10` at
  `0xc9a17..0xc9a1d`.
* `0xc9a37..0xc9a5c` emits the synchronization/state-arm sequence containing
  count `4`, tag `0xacafafc0`, selector `0x307`, `tag|1`, `tag`, and
  `MemLevels`.
* `0xc9a65..0xc9aa3` conditionally emits the real DWGCTL packet with tag
  `0x212121c0`.
* `0xc9aa6..0xc9aae` clears the handled bits with
  `dirty &= 0xffffff40`.

Clean groups are therefore omitted, while the state-arm and synchronization
cadence remains a distinct operation.  Omitting repeated register values does
not authorize removing the logical EOB/DWGSYNC boundary.

The command interpreter at `0x91840` reinforces this distinction.  Ordinary
writes are cached at `[context+0x10+register/4]`; indexed writes are drained
after more than 16 queued entries.  Register `0x1300` operations with low
nibble 6 or 7 bypass the ordinary shadow.  Writes to `0x12bc` are masked to bit
zero, and bit zero requests a wait until MMIO `0x41c` equals the tag.  Indexed
ports and synchronization registers are ordering operations, not removable
value assignments.

## Open-driver implementation contract

The first different-state batching feature is intentionally limited to two
logical batches.  Mesa concatenates both validated packet streams and inserts
one `PARHELIA_RELOC_BATCH_BOUNDARY` relocation naming the first packet of the
second stream.  The relocation has zero dword, handle, delta, and pad fields.
The kernel advertises support through `PARHELIA_PARAM_STATEFUL_SUBMIT`; Mesa
also requires the explicit `PARHELIA_BATCH_DIFFERENT_STATE` environment opt-in.

The kernel must reject malformed markers and accept them only on the async
path.  The marker packet index must be greater than zero and less than the
packet count; markers must be strictly increasing and unique.  The initial
implementation accepts at most one marker so its fence and watchdog behavior
remain bounded.

For a marker before packet `B`, call-plan order is:

```text
opening0, packets [0,B), EOB0, drain0,
opening1, packets [B,N), EOB1, drain1
```

Each range finds and replaces its own opening marker and computes the DWGCTL
value at that logical range's end.  The first terminal fence becomes the next
opening fence's predecessor; the ioctl returns the second terminal fence and
only the final primary producer pointer needs to be published.  Draw packets,
including the depth-interlocked one-triangle-per-packet path, remain separate.

State filtering may walk the two ranges in order and retain its predicted
register values across the boundary.  It must never omit EOB, fence, draw,
WARP, stream/fetch, constant, surface, or combiner packets.  Its prediction must
be invalidated after reset, failed execution, engine/class transition, or any
path on which another owner can alter hardware without updating the shadow.

This design preserves the synchronization behavior shown by both vendor
drivers while saving one userspace ioctl and one final producer update.  It
does not claim hardware validation until the guarded capability has passed the
existing no-vsync `glxgears` correctness/performance regression and Q3 testing
on the target.
