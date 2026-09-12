# P690 asynchronous render submission and completion queue

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope and result

This audit answers one narrow question: how the saved Matrox Linux and Windows
drivers submit more than one rendering command buffer without waiting after
each doorbell, and what must be retained by an open P690 DRM driver to do the
same safely.

The answer is definite on the command/ring and fence mechanisms:

* A submission is asynchronous by default.  The vendor appends a five-dword
  secondary-CALL record to a kernel-owned DMA32 primary ring and advances the
  producer at `0x0408`.  It does not poll `0x041c` on that normal path.
* The authoritative render-completion counter is the monotonic DWGSYNC tag:
  write it through DMA register `0x12bc` and read completion from `0x041c`.
  Linux allocates tags by eight (`mtx3144` at `.text+0x31c30`); the Windows
  display driver allocates by `0x10` (`MTXPARD.dll` `0x32b60`).  In both cases
  zero is skipped and comparison must mask low tag flags.
* The vendor has a real deferred presentation queue.  Its 32-entry flip queue
  checks `0x041c` on vblank and only selects a new scanout START slot after
  the preceding render tag has retired.

It is **not** definite that the P690 supplies a usable render-completion IRQ.
The Windows miniport dispatches `0x00010000` and `0x00040000` IRQ sources, but
neither is named as DWGSYNC completion and no target observation ties either
bit to an `0x041c` transition.  Do not enable either source in the open driver
as a performance change.  Vblank is proven and may protect presentation; it
cannot be the retirement mechanism for unrestricted rendering because it is
only 60--75 Hz on the test displays.

## Exact vendor primary-ring ownership

The Linux binary function `mtx1937` (`mtx_parhelia.o`, `.text+0x32360`) and
the equivalent Windows display-driver writer use a persistent primary-ring
object.  Its relevant Linux layout is:

| ring-object offset | meaning | evidence |
|---:|---|---|
| `+0x60` | kernel virtual producer/write pointer | `mtx1937` advances it by `0x14` before every CALL |
| `+0x68` | last submitted mark | object layout / ring helpers |
| `+0x70` | end of the current primary allocation | `mtx1937` checks `write + 0x14 > limit` |
| `+0x9c` | DMA-bus minus kernel-VA bias | added to the new write pointer for `0x0408` |
| `+0x98` | optional fence-path flags | nonzero diverts to `mtx3144` |

The normal append sequence is recovered instruction-for-instruction:

```text
if (write + 0x14 > limit)
        ring->SwitchToNewBuffer(ring)
write += 0x14
write[-5..-1] = [ 0x20292820, 0,
                  secondary_start | start_flags,
                  secondary_end   | end_flags, 0 ]
wmb / DMA ordering
MMIO[0x0408] = bus_bias + write
```

`0x20292820` is the primary header `{DMAPAD, SECADDRESS, SECEND, DMAPAD}`.
The vendor's `mtx1937` writes `0x0408` immediately after the append when the
optional fence path is off.  There is no `0x041c` read in that path.  The
working Q3 trace has repeated no-argument GL doorbell ioctls before its first
recorded `glFinish`, which independently confirms that a GL command-list
flush does not imply a completion point.

`0x0404` is **not** written per CALL in the vendor.  Linux `mtx2508`
(`.text+0x31880`) and the Windows restart routine (`MTXPARD.dll` `0x328d0`)
only program it when arming/restarting a primary allocation:

```text
wait until primary DMA is idle       (0x08f0 bit 17)
wait until indexed-write FIFO is idle / baseline is restored
MMIO[0x0404] = (ring_bus + ring_write_offset) & ~3
MMIO[0x0400] = ring_limit_bus          /* 32-byte aligned where required */
```

The observed vendor arm used a 50 MiB DMA32 range and a limit one page below
its end.  That proves the producer must never pass the programmed limit.  It
does **not** prove a minimum ring size, the exact wrap handoff protocol, or
that a small independently allocated primary span can be selected by rewriting
`0x0404` while prior work remains.  An open driver must therefore not derive a
multi-slot queue by allocating unrelated primary buffers and reprogramming
`0x0404` for every slot.

The current DRM path does exactly the safe synchronous simplification:
it owns one ~7 KiB primary allocation, writes `0x0404` to its base per submit,
publishes one whole span through `0x0408`, and waits before reusing it.  That
is correct only because it blocks.  Returning early would overwrite that same
primary allocation, `dma_blocks[]`, global writeback page, and pinned
relocations while the engine can still consume them.

## Completion and the vendor queue

The full Linux fence path (`mtx3144`) places a recovered completion block in
the same primary allocation.  Its relevant tail is a register-list write of
the following shape:

```text
[0x21c0c0c0, 0x307, 4, 0x707,
 0xc0afafdd, 0, fence_a | 1, fence_b | 1, saved_dwgctl]
```

The `0xaf` bytes write `0x12bc`; `0x41c` exposes the last retired tag.  The
vendor can request an in-stream blocking form by setting low tag bit 0, but
the open P690 tests show that form is not currently reliable.  A new queue
must use the already working nonblocking tag sequence and test the completed
value with signed modular tag ordering after low flag bits are cleared.

Vendor client command buffers and completion state are explicitly decoupled:

* The Linux shared locked status page carries the primary limit, next fence,
  producer value and outstanding-submit counter (`+0x40`, `+0x44`, `+0x70`,
  `+0x74`, `+0x78`).  The ICD maps that page and polls it; it has no completion
  ioctl.
* The Windows driver builds the same nine-dword fence block for a submit with
  fence flags, advances `0x408`, and its blocking helper polls `0x41c` only
  when the caller requests completion.
* The Linux vendor page-flip escape queues up to 32 flips.  On a vblank it
  reads `0x41c & ~7`, rejects a flip whose render tag is newer, and only then
  writes the selected START-bank entry and controller slot.  This is concrete
  proof that the hardware supports render work outstanding across one or more
  vblanks.

This is also why a display-vblank IRQ cannot solve the observed Q3 submit
stall.  It is an independently correct event source for scanout.  The current
open module already handles head-0 bit 1 with W1C ACK at `0x8f8`; Q3 stayed
near one frame per second while that counter advanced near 60 Hz.

## Required open-driver design

The exact evidence supports the following design, but no narrower unsafe
shortcut:

1. Allocate one persistent, contiguous DMA32 primary ring.  Initialize
   `0x0404` and `0x0400` only while no primary work is outstanding.  Allocate
   monotonically increasing, non-overlapping primary spans within it, append
   CALLs and a nonblocking terminal fence to each span, then advance `0x0408`
   to that span's end.  Do not reset `0x0404` or wrap the allocation until all
   earlier spans have retired and the primary-DMA idle condition is true.
2. Give every in-flight slot separate coherent secondary blocks, a separate
   writeback/status word if it is used, copied vertex upload storage, command
   metadata, GEM references and VRAM pins.  A slot records its terminal
   DWGSYNC tag and primary-ring `[start,end)` byte range.  Two slots are the
   minimum; three are preferable because the Q3 two-texture path may create a
   producer/consumer dependency while a Present copy is pending.
3. Add an explicit `PARHELIA_SUBMIT_ASYNC` ABI flag and a wait operation
   returning/accepting the generated tag.  Existing submit behavior remains
   synchronous.  On an async submit, return after the `dma_wmb()` and `0x408`
   doorbell; do not drop relocations or reuse its storage.
4. Retirement reads `0x041c & ~7`, applies signed sequence ordering, releases
   only the completed slots in submission order, unpins their BOs, drops GEM
   references, and wakes waiters.  Until an engine IRQ is proven this can run
   from a bounded delayed work item plus the explicit WAIT ioctl; it must not
   spin under `submit_lock`.  A sleeping wait reduces CPU burn, while multiple
   retained slots removes the artificial producer/consumer serialization.
5. Flush, CPU maps, resource destruction, render-to-texture dependencies and
   KMS/Present scanout must wait for the appropriate slot tag.  Attach the
   fence to every written GEM reservation before a scanout-capable object can
   be selected.  Vblank still gates the scanout change after render retirement.

No direct IRQ completion patch is justified yet.  Enabling bit 16 from the
Windows trace would mix an unidentified shared interrupt source with the
existing vblank handler; the current target evidence is insufficient to
acknowledge, mask, or wake a fence based on it.

## Validation gates

Before deployment, validate each step with the staged-only kernel module:

1. Run one known texture/depth draw synchronously and through a single async
   slot, then wait for its exact `0x41c` tag and byte-compare the render target.
2. Alternate two slots for at least 10,000 submissions.  Assert primary ranges
   never overlap before terminal-tag retirement; verify all pins and GEM refs
   return to baseline, no timeout/wedge occurs, and `0x0404` is never changed
   while a slot is busy.
3. Stress Q3 and an X Present copy together.  Verify that a scanout update
   waits for the producing tag, all vblank events remain monotonic, and images
   match the synchronous reference.
4. Measure Q3 frame rate and `/proc/PID/stat` user/system deltas separately.
   Success requires both image identity and the disappearance of the current
   almost-all-kernel-time submit loop; uncapped `glxgears` alone is not enough.

## Sources

* `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`: `mtx1937`
  `.text+0x32360`, `mtx2508` `.text+0x31880`, `mtx3144` `.text+0x31c30`,
  `mtx1415` `.text+0x3adb0`, and `mtx4025` `.text+0x3a590`.
* `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`: primary restart
  `0x328d0`, fence block `0x32b60`, and fence waits `0x91b10`/`0x91b4b`.
* `captures/ioq3-p690-working-draw.txt` for repeated command doorbells before
  explicit GL completion.
* `parhelia-kernel-abi.md` §§6--8,
  `parhelia-engine-and-context-init-complete.md` §§1.3, 2.6 and 3,
  `parhelia-rendertarget.md` §§6.2--6.5, and
  `parhelia-q3-submit-performance-audit.md`.
