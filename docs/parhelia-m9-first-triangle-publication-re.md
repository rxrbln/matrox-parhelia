# M9148 first-triangle publication and retirement

> **Correction (2026-09-15):** the range table and patch offsets below
> predate the inline P00/S28 expansion (current framed ends
> `0xbc8/0x1774/0x17a0`, 22 relocation sites).  Every vendor `ExecuteDma`
> also appends `4545421a <TC[0]|tag> <token|1>` after the `2c/2d` records
> (twelve dwords for three ranges); a buffer switch is the in-stream
> `453e421a` record, never a `0x400` rewrite; `31c=0` is written only for a
> class-0 primary; the vendor wait has no timeout and strobes `0x314` only
> while `0x106c` is unprogrammed.  `S` is a class-1 compact GPUVA.  See
> `parhelia-m9-cold-transition-submit-decode.md` section 5.

This note closes the command-transport step which follows construction of the
fresh selector-`6c` record.  It joins the successful Linux triangle capture,
the Linux `PostDOSBuffer` implementation, the proprietary Linux kernel
requester, the Windows display and miniport implementations, and the live
primary-requester trace.  It does not access hardware.

Recheck all binary slices, captured bytes, private boundaries, primary records,
target allocation, and live interval sizes with:

```sh
python3 tools/m9-first-triangle-publication-evidence.py
```

## Result

The current kernel's count-free `118/113/8` dword arrays are validation views;
they are not the proven primary-requester image.  The vendor submits the three
top-level `ExecuteDma` ranges through three 12-byte primary indirection records:

```text
45452d2c                 indices 2c,2d,stop,stop
secondary_range_start | 1 index 2c, BmBuffer start/access-class tag
secondary_range_end | 2  index 2d, end-exclusive with class/tag 2
```

The secondary buffer retains the private outer framing byte for byte.  Counted
records keep their count dword, and H03 draws remain raw three-dword records.
The 37/37/3 private records do not become 77 primary records, and their payloads
must not be flattened into one primary interval.

For an aligned secondary DMA base `S`, the captured triangle is:

| range | logical bytes | secondary bytes | primary descriptor |
|---:|---:|---:|---|
| preprocess | `[000,268)` | `[S+008,S+270)` | `{45452d2c,(S+008)|1,(S+270)|2}` |
| raster | `[268,4bc)` | `[S+270,S+4c4)` | `{45452d2c,(S+270)|1,(S+4c4)|2}` |
| completion | `[4bc,4e8)` | `[S+4c4,S+4f0)` | `{45452d2c,(S+4c4)|1,(S+4f0)|2}` |

The eight-byte displacement is part of the saved BmBuffer layout.  The
`ExecuteDma` wrapper `m9x840` dispatches vtable slot `+0x80` to kernel routine
`m9x830`.  That routine loops the descriptor array, computes each start from
the buffer's current GPU address plus eight plus the logical start, ORs the
buffer address-class field, clamps the logical end to `extent-8`, and ORs two
into the resulting end.  It emits literal header `0x45452d2c` for every range,
then calls requester flush and submit exactly once after the loop **[BIN]**.

Linux userspace `m9x22842` independently constructs the same record for its
single-buffer `PostDOSBuffer` path, and Windows emits the same header and
start/end fields.  This is a hardware secondary-range operation, rather than
an editorial boundary in the capture decoder **[BIN]**.

## Exact private framing

The complete framed secondary ranges have these properties:

| range | bytes | outer records | counted | raw H03 | SHA-256 |
|---:|---:|---:|---:|---:|---|
| 0 | `268` | 37 | 36 | 1 | `0473c1ccebea87d334fb4ab7cb72d76c1871587e8adb1e4d22e77884693bf0c6` |
| 1 | `254` | 37 | 36 | 1 | `f03cd8b7b7ac28d53fca947d152d07b81ab8654ef710ddc0d5fb1a4708bfe8b5` |
| 2 | `02c` | 3 | 3 | 0 | `e63d578b57c96295bdcef682e4236cfc57109f30fe1f49dff2d1c98405770463` |

The verifier pins every cumulative outer-record end.  It also maps the
count-free relocation and transition offsets back to physical dword offsets
in `bm-12.bin`.  A kernel implementation should copy the framed image, patch
the physical operands corresponding to the already audited semantic sites,
and validate the count-free view without changing the framing.

The address and ordering operands used by the final job lie at these physical
byte offsets in that image:

```text
014 e3=t0      050 e3=t1
27c e3=t2      298 e3=t3      2a4 43=t3
2e0 e3=t4      4cc 1b=fence-address|2
4e0 e3=t5|1    4ec 43=t5
```

The 24 resource relocation sites map similarly and remain pinned by
`m9-first-triangle-stream-evidence.py`.  The full secondary allocation and
every referenced resource must remain DMA-address-stable until `t5` retires.

## Primary publication

The live Linux GL trace sees only final before/after requester snapshots, and
its primary spans are exactly zero, 12, 24, or 36 bytes.  Kernel `m9x830`
constructs all descriptors before its single requester submit.  The faithful
initial implementation is consequently:

1. Reserve one secondary slot holding the complete framed image through
   `S+0x4f0`; patch it and publish all CPU writes.
2. Reserve 36 contiguous bytes in an unretired primary slot.  If the current
   slot cannot fit them, switch only to a slot whose fence has retired, write
   `0x400 = primary_dma | 2`, and read it back before the first endpoint.
3. Write all three `{0x45452d2c,start|1,end|2}` records and issue
   `dma_wmb()`.
4. Clear `0x31c` through the already hardware-tested requester path, then
   publish only `0x404 = primary_dma + 36`.  This value is an untagged,
   end-exclusive primary DMA address.  Read it back as a PCI posting barrier
   before beginning the completion wait.
5. Do not rewrite `0x400` between these records, initialize the dormant
   `0x500..0x524` requester, or put a secondary tag on the `0x404` value.

Do not publish intermediate `+12` or `+24` endpoints.  The saved ExecuteDma
producer performs one flush and one submit after the descriptor loop.  The
completion in range 2 retires all three ranges before the slot can be reused.

## Transition and completion timeline

Reserve six consecutive values atomically from the same 32-bit timeline:

```text
t0, t1, t2, t3, t4, t5 = base + {0,10,20,30,40,50} modulo 2^32
next = base + 60 modulo 2^32
```

Patch every `e3` and `43` occurrence as one set.  The final framed range binds
the coherent address through index `1b`, emits `e6=3`, `e6=2`, writes
`e3=t5|1`, then writes `43=t5`.  Seed the coherent dword with a value which
cannot equal `t5`, and accept retirement only after `dma_rmb()` observes its
low-nibble-masked value at or beyond `t5` in the wrap-extended timeline.

An equality-only test is adequate for the current synchronous diagnostic but
is too narrow for a scheduler: a later completion may legitimately overtake a
worker which sampled late.  Keep fewer than `2^31` outstanding target steps so
the modular ordering remains unambiguous.  Incrementing the existing
`next_fence` only once is wrong for this operation; it consumes six values.

Completion is a coherent memory writeback.  Neither saved stack consumes an
IRQ completion counter, and the recovered IRQ sources do not include a generic
3D-completion bit.  Poll from the scheduler worker initially.  `0x8f0`
front-end state and the dormant auxiliary `0x508` idle bit are diagnostic
observations, not substitutes for the fence word.

## Lifetime and bounded failure

The one-queue implementation must retain references and stable DMA addresses
for the primary slot, framed secondary slot, selector-`6c` system allocation,
vertex source, transient surfaces, color, depth, auxiliary depth, resource
`69`, common `c2`, `be`, and completion page through `t5`.  Attach the same job
`dma_fence` to every writable resource.  Context destruction, CPU access,
sampling, presentation, and slot reuse wait on that fence.

Use one absolute deadline for final memory completion.  A pending job may
receive one `0x314=1` progress kick before the deadline.  On expiry, snapshot
the command interval, secondary ranges, fence words, `0x31c`, `0x400/0x404`,
`0x448`, `0x8f0`, `0x1068/0x106c`, and IRQ banks; reject new jobs; increment
the engine generation; and signal every old-generation fence with an error.
There is no recovered primary/3D reset sequence.  Keep possibly reachable BOs
pinned, keep KMS scanout running, and clear PCI bus mastering before terminal
teardown releases quarantined DMA memory.

## Secondary start tag and remaining hardware uncertainty

Static evidence closes the three primary records, exact secondary bytes,
endpoint order, six-target allocation, completion, and lifetimes.  The live
clean-room driver has executed direct three-dword primary fence packets, but it
has not yet executed index-`2c/2d` secondary indirection.  The first guarded
triangle must therefore validate this transport in stages: first a framed
secondary no-op with the same primary descriptor, then the three-range setup,
then the triangle after selector-`6c` serialization.  Any failure wedges only
the opt-in render generation and must not trigger a guessed reset.

The exact `m9x830` start formula ORs the secondary resource's field at `+0x34`
into every index-`2c` start address.  A later audit of the preserved raw ioctl
archive found the field in the `ExecuteDma` output after all: kernel `m9x83`
copies the next BmBuffer's `+0x34` to ioctl tail `+0x10`, which is complete
ioctl offset `0x828`.  All 15 complete saved output records contain `1` there.
The canonical clean DMA32 system-memory implementation must therefore use
start tag `|1`; the zero assumption in the original version of this note is
superseded.  The primary requester's `|2` base tag remains a separate register
contract.  See `parhelia-m9-secondary-start-tag-re.md` for the complete static
and capture join.  The index-`2d` end tag `|2` remains independently exact.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
