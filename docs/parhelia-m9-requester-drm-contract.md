# M9 DRM command requester, fence, IRQ, and recovery contract

This document is the implementation boundary for the first M9 DRM render
queue.  It combines static analysis of the saved Linux
KernelLibraries.o_shipped and Windows MxMM.sys with a read-only live M9148
observation made on September 14, 2026.  The live observation resolves the
largest first-render ambiguity:

* ordinary vendor GL uses the primary 0x400/0x404 command interval;
* that interval is in DMA32 System RAM and its start carries tag 2;
* the separate 0x500..0x524 requester descriptor remains dormant throughout
  ordinary GL rendering and a complete X server restart.

The five hardware-owned 0x500 descriptor words still require a forced-wrap
capture before that auxiliary path can be implemented.  They no longer block
a first triangle through 0x400/0x404.

## 1. Evidence boundary

The static evidence is independently reproduced by:

~~~sh
python3 tools/m9-allocation-mode-evidence.py
python3 tools/m9-requester-rollover-evidence.py
python3 tools/m9-fence-record-evidence.py
python3 tools/m9-irq-selector-evidence.py
python3 tools/m9-nonvblank-irq-evidence.py
python3 tools/m9-recovery-surface-evidence.py
~~~

The combined static/live contract test is:

~~~sh
python3 tools/m9-requester-drm-contract-test.py
~~~

Its live fixture is
captures/m9x-requester-live-20260914/ioctl-mmio.csv.  The observer which
created it maps the PCI resource read-only and does not change ioctl arguments,
results, or MMIO:

~~~text
tools/m9-requester-ioctl-observe.c
tools/m9-requester-live-snapshot.c
~~~

The complete three-second glxgears run observed 32,782 ExecuteDma calls and
65,564 pre/post snapshots.  Every command start had low bits 2, every endpoint
was dword aligned, and every command address was inside the host System RAM
range 0x00100000..0x1ffaffff.  The card's VRAM BAR was
0xd0000000..0xdfffffff.  The descriptor image was invariant:

~~~text
500 00000000
504 00000000
508 00000001
50c 00000000
510 00000000
514 00000000
520 90622002
524 1f002e08
~~~

This is direct evidence that the descriptor is idle, not evidence for semantic
names for 520 or 524.  Do not copy those board/runtime values into a new
driver.

For 32,750 race-free post-ioctl snapshots, the interval
MMIO[404] - (MMIO[400] & ~3) was below 64 KiB.  The common lengths were 12,
24, 36, and 48 bytes.  The compact checked-in fixture has 100 complete
before/after pairs and no cross-client race.

## 2. Two command mechanisms

Treat these as different hardware objects.

### 2.1 Primary render command interval

The primary render path is:

~~~text
MMIO 400 = start DMA address | 2
MMIO 404 = exclusive end DMA address
~~~

Linux m9x109 and Windows 0x224c8 program 400.  Linux m9x1039 and Windows
0x224f3 program 404.  The latter computes producer - CPU_start + DMA_base.
There is no length, ring limit, or 0x500 field involved.  The live captures
show this pair changing around every GL ExecuteDma call while 0x500..0x524
does not change.

### 2.2 Auxiliary requester descriptor

The 0x500 descriptor owns a different software stream with its own one-page
allocation, live-tail publication, opcode-1 transition records, opcode-2
wrap-to-base record, quiescence, and save/restore.  Neither the Linux nor the
Windows 0x404 submitter accesses it.

The first DRM render queue must omit this auxiliary stream.  It may be added
when a feature is found which requires it and after the forced-wrap experiment
in section 9.  Leaving an already dormant descriptor untouched is safer than
initializing five unknown hardware-owned words.

## 3. Primary DRM queue

### 3.1 Allocation

At probe, require a 32-bit streaming and coherent DMA mask.  Allocate command
staging from DMA32 coherent memory.  The vendor userspace mapping is 0x8000
bytes, so 32 KiB is a hardware-proven initial slot size.  The executed interval
may begin at any dword-aligned offset inside it.

Use multiple slots or pages.  A slot cannot be overwritten or returned until
the fence appended to its submission retires.  Pin every referenced GEM object
for the same lifetime.

Kernel commit `9219f41` implements two coherent 32-KiB slots for the guarded
no-op path.  Each 12-byte published record retains a fourth padding dword, so
one slot accepts 2,730 records.  A live 6,000-submit run crossed slot 0 to 1 at
submit 2,731 and slot 1 back to 0 at submit 5,461, then completed at fence
`0x17700`.  The module unloaded without a warning or timeout and left the PCI
function unbound with bus mastering disabled.  The immutable log and boundary
model are in `m9-primary-slot-live-evidence.py` [HW].

The live primary command address class is closed:

~~~text
address domain: DMA32 host System RAM
start tag:      2
endpoint tag:   none
alignment:      4 bytes
~~~

The four-entry auxiliary requester table {2,3,0,3} is a separate namespace.
The live primary tag 2 must not be inferred from a public GEM placement enum or
from the auxiliary descriptor's selector-2/tag-0 first-use allocation.

On the tested M9148, kernel commit `d545438` also requires warm controls
`44c/450/454` to equal `00280800/1f3f2707/00070703` before it enables bus
mastering.  This is the fallback tuple independently recovered from the static
packer and observed in the saved and current unbound-target snapshots.  The
driver preserves those values and rejects a different tuple; support for
another board requires its own property or packed-control evidence.

### 3.2 Submission

Userspace supplies validated commands and GEM relocations through the new DRM
UAPI.  It never supplies bus addresses or raw 400/404 values.  The kernel
copies commands into its coherent staging slot, validates every packet, and
applies relocations.

The initial run-job operation is:

~~~c
start = slot_dma + slot_offset;
end = start + command_bytes;

require(start <= U32_MAX && end <= U32_MAX);
require(!(start & 3) && !(end & 3));
require(end >= start && end <= slot_dma + slot_size);

append_kernel_fence_record();
dma_wmb();
writel(lower_32_bits(start) | 2, mmio + 0x400);
writel(lower_32_bits(end),       mmio + 0x404);
~~~

The end is exclusive.  Never OR tag 2 into 404.  Never derive 404 from a
packet count.  The saved Windows submitter clears 31c before posting, but the
new driver should program 31c only through the proved initialization state and
must not use states 4 or 5 as ordinary submit flags.

Use one hardware queue initially.  A spinlock protects staging-slot allocation,
target assignment, and 400/404 publication.  DRM scheduler dependencies and
the slot's fence provide ordering across contexts.

## 4. Completion fence

Allocate a coherent DMA32 page for writeback.  The live gltri footer binds
index 1b, hardware register 106c, to 0x13861002.  That address is also in
System RAM and has tag 2.  This closes the initial fence-address domain.

The exact minimal completion record, present in both kernel stacks, is:

~~~text
4545e3e6
00000003
target | 1
~~~

Target advances by 0x10.  The live GL footer uses the expanded form:

~~~text
1b = fence DMA address | 2
e6 = 3
e6 = 2
e3 = target | 1
43 = target
~~~

Use the live expanded footer for first hardware bring-up and keep the minimal
record as the no-op fence test.  Only the kernel appends either form.

The GPU-written low word is read as:

~~~c
raw = READ_ONCE(*fence_cpu);
dma_rmb();
low = raw & 0xfffffff0;
if (low < last_low)
        epoch++;
last_low = low;
completed = ((u64)epoch << 32) | low;
~~~

The unsigned regression rule is the exact Linux vendor algorithm.  It assumes
no more than one unseen 32-bit wrap, which is trivially satisfied with bounded
queue depth.  Keep the public dma_fence sequence independent from the encoded
target if that simplifies comparison.

At initialization, choose target 0x10 and seed the word so its masked value is
the preceding target.  Across natural wrap, retain the completed
0xfffffff0 value until the GPU writes target zero; clearing the word before
that submission would create a false wrap completion.  After recovery,
reinitialize last_low, epoch, the next target, and the memory sentinel as one
locked operation.

Poll coherent memory from a worker while jobs are pending.  A pending wait may
write 1 to 314 once as the statically proved progress kick.  Render completion
does not consume an IRQ and must not use 8f0 idle as a substitute.

## 5. Auxiliary 0x500 requester

This section is an exact deferred implementation contract.

### 5.1 First use and address classes

Windows first use allocates one 0x1000-byte block with internal allocation
selector 2.  The requester tag table is:

| address class | low tag |
| ---: | ---: |
| 0 | 2 |
| 1 | 3 |
| 2 | 0 |
| 3 | 3 |

The descriptor writes are ordered:

~~~text
500 = translated base | tag[class]
508 = 0
504 = translated CPU producer
~~~

Only 500 and transition records carry the tag.  The live tail at 504 is a raw
byte address.  Do not expose these classes as GEM domains until a constructor
or live capture proves the mapping.

### 5.2 Software bounds and publication

For resource R and reserve count N, the Linux object computes:

~~~text
backed = R[6c] != 0 || R[70] != 0
cpu_base = backed ? R[50] : 0
translation = backed ? R[58] : 0

start = cpu_base
producer = cpu_base
end_guard = (backed ? cpu_base + R[40] : 0) - 4*N
previous = backed ? cpu_base - 4 : -4
~~~

Before publishing new commands, issue dma_wmb(), translate the producer, and
write its raw address to 504.  The normal transition begins with opcode 1,
tagged old pointer, and raw new pointer.  The complete 16-byte and bus-master
32-byte payload meanings remain open, so this transition must not be emitted
by the first driver.

### 5.3 Wrap

The closed wrap sequence is:

1. publish the current producer;
2. reach the bounded stable quiescence predicate below;
3. append the two dwords {2, allocation_gpu_base};
4. reset start/producer/end_guard to the allocation's CPU mapping;
5. publish the reset raw producer through 504;
6. quiesce again.

Opcode 2 is not an indexed packet and does not write 404.

### 5.4 Quiescence

Use an absolute time deadline.  A candidate snapshot is quiescent only when:

1. two successive 508 samples both have bit 0 set;
2. two successive pairs of 500 and 50c are identical;
3. a final 508 sample still has bit 0 set.

Restart the observation if either live pair changes or the final bit clears.
Abort as device removal if 8f0 reads 0xffffffff.  Pointer equality with 504,
8f0 busy bits, or a completed render fence cannot replace this predicate.

### 5.5 Save and restore

Software initializes only 500, 504, and 508.  It has no producer for
50c/510/514/520/524 outside restore.  Preserve all eight words as an opaque
snapshot.

The Linux save path publishes and quiesces, writes 508 = old | 2, waits again,
and reads all eight words.  Restore ordering is:

1. 520, 524 unconditionally;
2. 50c, 514, 510 when saved 50c != saved 510;
3. 500, 508, 504 last.

Windows narrows the middle predicate to
((saved_50c ^ saved_510) & ~0xf) != 0.  A new Linux driver may follow the
saved Linux full-dword predicate.  It must not initialize, rename, recompute,
or validate those five hardware-owned words.

## 6. IRQ contract

The interrupt banks are:

| bank | status/W1C | mask |
| --- | ---: | ---: |
| high | 8d8 | 8dc |
| low | 8e4 | 8e8 |
| auxiliary | 8f8 | 8fc |

At initialization, mask all sources, snapshot and acknowledge stale status,
then enable only the needed vblank bits.  Physical display controllers map to
low-bank bits 1, 2, 0x100, and 0x200.

The shared handler reads raw status and masks, computes owned active bits,
acknowledges only those W1C bits, and returns IRQ_NONE when none are active.
Treat all-ones reads as device removal, not as asserted interrupts.

No recovered source is generic render completion or a defensible 3D fault.
Windows high-bank bit 8 is narrowly a rectangle-transfer callback.  Other
non-vblank sources reach display status, endpoint repost, or auxiliary service
work, or are W1C-only.  Keep them masked.  Complete DRM jobs only from the
memory fence.

## 7. Timeout and fail-closed recovery

Use separate absolute deadlines for front-end observation, auxiliary
quiescence, and fence completion.  Success in one domain does not prove health
in another.

On a primary render-fence timeout:

1. stop scheduler admission and serialize recovery;
2. record the job, generation, command interval, fence CPU/DMA address and
   contents, 8f0, 31c, 400/404/448, all eight descriptor words, and raw/masked
   IRQ banks before acknowledging or rewriting anything;
3. issue at most one 314 progress kick and wait to the final deadline;
4. if the fence still does not advance, error every old-generation fence and
   mark rendering wedged while leaving KMS scanout running.

Do not use 508 bit 0 to recover a 400/404 timeout.  The live evidence proves
that the auxiliary descriptor can be quiescent and dormant while the primary
render path is active.  Likewise, the 31c 5/0/2 aperture restore sequence is
not a global 3D reset.

If a later auxiliary requester times out, one requester-local save/restore is
allowed only after its stable quiescence predicate succeeds.  Restore its
opaque image in the order above, restore the proved address windows, kick 314,
and require a fresh no-op fence.  If any step fails, wedge rendering.

No saved production component exposes a reusable 3D/global reset, and PCI
function reset is not proven safe for this display device.  Never guess either
operation.  Do not free a command slot or GEM object which may still be
reachable by a wedged engine.  On terminal teardown, stop bus mastering before
quarantined DMA storage can be released.

## 8. First-render implementation sequence

1. Keep M9 rendering opt-in.
2. Set the DMA mask and allocate 32 KiB coherent command slots plus a coherent
   fence page below 4 GiB.
3. Initialize only the statically closed engine/address-window state required
   by 400/404; leave the dormant 500 descriptor untouched.
4. Submit the kernel-built minimal fence interval with 400=start|2 and
   404=exclusive_end.
5. Require the writeback word to advance under a bounded wait.
6. Add validated clear and captured two-pass triangle packets.
7. Alternate contexts and verify shared BO fence lifetime, then test the
   independently remaining 32-bit fence rollover.
8. Add the auxiliary requester only when a demonstrated feature needs it.

The module remains in a temporary target directory during bring-up.  Do not
install it into the target's normal module tree.

## 9. Exact remaining experiment

Static analysis is exhausted for 50c/510/514/520/524: the Linux and Windows
kernel binaries have no software producer for them outside restore.  Ordinary
GL and X restart leave the auxiliary descriptor dormant.  The smallest
remaining experiment must deliberately exercise the vendor auxiliary path:

1. enable mmiotrace before the first vendor register mapping, or add a one-shot
   read-only snapshot at the existing kernel requester callback;
2. record the auxiliary allocation's CPU base, DMA base, size, address class,
   software start/producer/end_guard, and every 504 publication;
3. force exactly one vendor m9x1200 wrap;
4. snapshot 400/404/448 and all eight descriptor words before publication,
   immediately after publication, after {2, allocation_gpu_base}, and after a
   following fence retires;
5. save and restore once, preserving the exact MMIO order;
6. record whether 50c or 510 moves, which values bound the old/new page, and
   whether 514/520/524 change with the transition.

Do not trigger this by guessing an ioctl or writing requester MMIO.  If 50c
and 510 have equal upper 28 bits, repeat once with allocations in different
upper-28-bit regions so the Windows restore predicate is exercised.

Remaining unknowns after this audit are limited to the auxiliary opcode-1
payload fields, public names/constraints for its four address classes, the
five hardware-owned descriptor semantics, and a real 3D fault/reset mechanism.
None is required for the opt-in primary 0x400/0x404 first-render path.
