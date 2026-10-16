# M9148 first-submit transition

> **Superseded (2026-09-15):** the packet half of `m9x421` is gated on
> `B+0xc8 != 0` (`.text+0x16ab0`), a refcount that `m9x182` increments only
> after both cold transitions.  Both cold calls are MMIO-only; no
> `e6/47/e3/43/22` packet precedes sequence 0 on the M9148, and the
> post-publication packet path in `m9x887` is disabled by `MmFlags` bit 24.
> Those streams describe only the gated warm half; kernel commit `b788cc4`
> removes them from cold construction.  See
> `parhelia-m9-cold-transition-submit-decode.md` section 1.

This note retains the packet decode, but corrects its scope.  It supplements
`parhelia-m9-pre-submit-engine-sequence.md` and does not replace the exact
address-window or requester recipes there.

## The packet half is a warm-path operation

Linux `m9x421` can emit three packet groups through its command builder in two
distinct submissions:

```text
454545e6 00000002
45454747 000011a4 00001308
454522e6 40000000 (MMIO[1088] | 20000000)
```

When `B+0xc8 != 0`, the call at `.text+0x1660c` submits the first `e6` packet
through vtable slot `+0x100`.  Only after that call does `m9x421` publish the
address windows.  At the tail it appends the `47` packet, reserves a
synchronization value through slot `+0xe0`, reads `0x1088`, appends the
`e6/22` packet, and submits that work through slot `+0x100` at
`.text+0x16a0a`.  Slot `+0x100` resolves to `m9x954`; with argument one it
reaches `m9x1039`, the primary requester doorbell path.  Thus `+0x100` is a
real flush boundary on the warm path, not a builder-only state update.

The slot `+0xe0` call is also observable command content.  It resolves to
`m9x81(-1)`, which calls slot `+0xd8` = `m9x604`.  That routine advances the
common submission target by `0x10`, emits `e3 = target | 1`, and returns the
low-bit-clear target.  `m9x81` immediately emits `43 = target`.  The later
`m9x954` flush advances the target by another `0x10` for its terminal
completion.  Captured streams have this exact pattern:

```text
... e3 = 0000eb40
    43 = 0000eb40
... 22 = 0008000e
... e3 = 0000eb50
```

Although packet indices `e6`, `47`, and `22` correspond arithmetically to
MMIO offsets `1398`, `111c`, and `1088`, they must remain packets whenever
the gated warm path executes.  This says nothing about the cold calls, which
skip this entire half and use only `m9x421`'s direct window writes.

## Cold construction publishes the windows twice

The cold `m9x582` constructor reaches `m9x421` twice.  The first call is
nested in initializer-table entry `m9x1042`: `m9x1042` calls the service
initializer at `.text+0x121d0`, which calls `m9x92`, then `m9x53`, and
`m9x53` calls `m9x421(..., publish=1)` at `.text+0x187aa`.  At this point the
requester object's `B+0xc8` refcount is zero, so the call publishes the direct
MMIO windows and returns before `m9x53` calls compact-table publisher
`m9x328` at `.text+0x1891b`.

After `m9x1042` and the three remaining initializer-table entries return,
`m9x582` calls `m9x1187` at `.text+0x2efd0`.  `m9x1187` calls
`m9x421(..., publish=1)` again at `.text+0x121bc`, still before `m9x182`
increments `B+0xc8`.  The exact cold logical order is therefore:

```text
MMIO window publication #1
compact PTE CPU mirror and 31c invalidation
MMIO window publication #2
completion/restore allocation and primary requester setup
```

Neither cold call reads `0x1088` or emits index `22`.  The earlier warm-unbound
sample `0x1088=0` remains useful only for the separately gated warm packet
path.

## Construction order versus execution order

The tail of the `m9x582` construction order is:

```text
m9x1187 -> m9x421       publish MMIO windows #2
m9x865                  completion resources
m9x949 -> m9x182
              -> m9x109 arm primary requester
```

`m9x182` increments the requester refcount only after both calls, then
`m9x109` installs the stable primary command, fence, and restore addresses.
There is no retained cold packet stream to execute after that arm.  A
clean-room driver preserves the effective hardware order by:

1. publishing the first direct-MMIO window tuple;
2. building the root table and activating it with `31c=f,1,0`;
3. publishing the second direct-MMIO window tuple;
4. allocating and publishing restore/completion and later mode-1 resources;
5. arming the primary requester once in exact `m9x109` register order;
6. starting sequence 0 as the first command stream.

The corresponding clean-room cold-transition command streams are empty.

## Initial E6 choice

Linux tests one runtime bit and has two encodings:

```text
bit clear: 454545e6 00000002
bit set:   4545e6e6 00000000 00000002
```

Both finish at `e6=2`.  The saved Windows driver selects the analogous
single-write form for the same M9148 default profile because its selector,
`MmFlags` bit 0, is clear in `0x000101f8`.  No saved Linux command image
contains the transient packet.  These encodings remain evidence for a warm
call only; the clean-room cold path emits neither form.

## Interpretation of the current failure

The post-`b788cc4` clean-boot test used MMIO-only cold transitions, restore tag
one and one `314=1` after each compact publication.  A direct primary no-op
retired at target `0x10`.  The canonical class-1 secondary at GPUVA
`0x40024000` then timed out at target `0x20` with its expected compact leaf
present.  This excludes the removed cold packet streams as a required gate and
continues to isolate the failure to compact translation or its selector state.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
