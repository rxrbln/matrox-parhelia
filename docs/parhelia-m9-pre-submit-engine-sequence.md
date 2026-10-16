# M9148 pre-submit engine sequence

> **Correction (2026-09-15):** steps 1, 5 and 6 of the "exact `m9x421`
> transition" (the `e6`, `47` and `e6/22` packets) apply only to warm calls
> with a constructed requester (`B+0xc8 != 0`); both cold calls write the
> MMIO windows only.  `T = 1` for the restore tuple.  The per-job `0x314`
> rule is: no strobe once `0x106c` is programmed; one `314=1` after each
> resource publication instead.  See
> `parhelia-m9-cold-transition-submit-decode.md`.

This note turns the vendor service construction, `m9x421` range publisher,
restore-pool construction, and `m9x109` requester arm into one ordered recipe
for the guarded first-triangle path.  It builds on the separate initialization
gate and `132x` association audits.  Recheck the added joins with:

```sh
python3 tools/m9-pre-submit-engine-evidence.py
```

The verifier reads saved files only.  No vendor code executes and no PCI or
DRM device is opened.

## Result

The vendor path has three lifetimes which must not be collapsed into a block
of copied warm register values:

1. board/service construction establishes the service lanes and preserves the
   device-specific parts of `f04` and `f0c`;
2. an address-manager transition publishes allocator ranges and two
   command-stream transition packets;
3. requester construction allocates the completion and restore resources,
   then `m9x109` writes the primary requester registers in a fixed order.

There is no further reset, PLL, memory-controller, or hidden 3D-enable write
between this sequence and userspace sequence 0.  The saved production drivers
also provide no bounded primary-requester reset on timeout.

## One-time service construction

The anonymous Linux service initializer at `.text+0x121d0` executes before the
construction tail in `m9x582`.  Its hardware writes are [BIN]:

```text
f04 = old_f04 & cfffffff
344 = 00000000
348 = 00000000
34c = 00000000
350 = 00000000
354 = 03020100
334 = 00000000
338 = 00000040
33c = 00000080
340 = 000000c0
330 = 00001000
```

The `f04` operation clears only bits 29:28.  The initializer first saves
`old_f04[1:0]` and `old_f04[20:19]` in its software object, so the new driver
must read this register before the masked write.  All other `f04` bits are
preserved.

An `f0c=00008000` write exists only behind three vendor predicates: feature
bit 3 is set, a service-object field is zero, and feature bit 26 is set.  The
saved default M9148 flags `000100e8` do not have bit 26, so this write is not
part of the tested M9148 recipe.  Later, unconditionally, the initializer
reads `f0c`, replaces only bits 9:6 with the low nibble of
`System\\TlbPrefCnt`, and writes it back:

```c
f0c = (old_f0c & 0xfffffc3f) | ((property & 0xf) << 6);
```

The setting construction is exact.  `m9x385` at `.text+0x3b89d` passes group
`System`, name `TlbPrefCnt`, and default `1` to the integer-setting constructor
`m9x1063`.  That constructor stores the default at object `+0x20`; its
`m9x325` getter returns that field.  The `m9x683` wrapper caches and refreshes
the selected setting, and the service initializer calls its selected-value
getter at `.text+0x125a5`.  No packaged configuration overrides this setting.
The canonical M9148 value is consequently:

```text
f0c = (old_f0c & fffffc3f) | 00000040
```

The read-only `f0c=00200000` snapshot was taken after the earlier clean-room
no-op module had cleared bits 9:6.  It is useful for the preserved bits outside
that field, but cannot override the vendor binary's exact default.  Starting
from that snapshot, vendor construction produces `00200040`.

The descriptor constructor `m9x1172` then writes `30c=0`, performs
`3a8 = (old_3a8 & fffcffff) | 00010000`, and publishes its service table
through `m9x115`.  That
table and the non-vblank interrupt workers belong to board/service lifetime.
They are not required to make a polled `106c` completion advance.  A first
clean-room triangle can retain the already installed KMS interrupt policy and
poll its completion word; it must not enable the unresolved service IRQs.

Service construction runs once while taking ownership of the engine.  It is
not repeated for every job or context switch.

The warm-state preservation set is therefore exact: retain every `f04` bit
except 29:28, replace `f0c[9:6]` with `1`, and retain every other `f0c` bit;
retain every `3a8` bit except 17:16.
Leave `1320..1328` unchanged when there is no companion extent.  The fixed
service lanes and M9148 requester controls listed here are intentional
replacements; unrelated KMS registers remain outside this operation.

## Exact `m9x421` transition

The tested Linux and Windows defaults select the direct-MMIO range transport:

```c
indexed = (MmFlags & BIT(9)) ||
          ((MmFlags & BIT(3)) && (MmFlags & BIT(24)));
```

For M9148 `MmFlags=000100e8`, so `indexed` is false.  The direct transition is
ordered as follows [BIN]:

1. When bit 3 is active, submit the small address-class packet used by the
   selected command-builder variant: either `e6=2`, or `e6=0,e6=2`.  The
   one-value form has header `454545e6`; the two-value form has header
   `4545e6e6`.  Do not emit both variants.
2. Write the mandatory selected-primary range:

   ```text
   132c = primary_base
   1330 = primary_base
   1334 = primary_base + primary_size - 1000
   ```

3. Only if `companion_size != 0`, write `1320`, `1324`, `1328` as companion
   base, base, and inclusive last page.  The first M9148 triangle has no
   validated companion extent, so it leaves this triple untouched.
4. When the caller's publish argument is one, write the remaining direct
   ranges in this exact order:

   ```text
   360 = range360_base & fffff00c                 # only if its size != 0
   364 = range360_base + range360_size - 1000 + auxiliary_offset
   f64 = aperture_base
   f68 = aperture_base + aperture_size - 1000
   f74 = primary_base
   f78 = primary_base
   f7c = primary_base + primary_size - 1000
   ```

5. Append `47=000011a4,47=00001308` to the command stream and reserve its
   synchronization token.
6. Read MMIO `1088`, append `e6=40000000,22=(read_1088|20000000)`, and publish
   the stream.  The packet headers are `45454747` and `454522e6`.

For Linux `MmFlags=000100e8` and `V=B=10000000`, `m9x858` closes every input
to these writes.  The exact direct-MMIO values are:

```text
132c=00801000 1330=00801000 1334=00860000
360 =00861000 364 =008e0000
f64 =008e1000 f68 =00940000
f74 =00801000 f78 =00801000 f7c =00860000
```

The primary tuple comes from descriptor `C18/C1c`, the `360` tuple from
`C28/C2c` plus auxiliary size `C34`, and the `f64` tuple from `C38/C3c`.
The separate `m9x953` local-resource allocator interval at
`10000000..1fffffff` is not an input to these manager windows.  Earlier notes
which assigned that local interval to `132c/f74` conflated the two address
domains.

The final two packets are part of `m9x421`, not sequence 0.  Direct MMIO range
writes alone omit an observed vendor transition.

## Restore pool and primary requester arm

`m9x582` fixes the surrounding order [BIN]:

```text
m9x1187 -> m9x421 address-manager transition
m9x865           allocate two completion domains
m9x949 -> m9x182 construct command resources and restore pool
                  -> m9x109 arm the primary requester
m9x926
m9x301
m9x1141
m9x115
```

The final `f0c` replacement belongs after the manager constructed by
`m9x92 -> m9x53`.  In that manager, `m9x421` completes its transition,
`m9x328` publishes the compact table, `30c=1000` is written, and the terminal
`31c=c,1` list is consumed before control returns to the service initializer.
Clearing `f0c[9:6]` during the earlier fixed-lane phase both programs the wrong
value and reverses this vendor order.

Within `m9x182`, `m9x1207` creates the restore-pool descriptor, and
`m9x1160(...,0x400)` constructs 1024 entries before `m9x109` is called.  Its
backing allocation size is exact:

```c
bytes = ALIGN((3 * 0x400 + 1) * 0x200, 0x1000); /* 0x181000 */
```

For restore backing GPU base `R` and low two-bit memory tag `T`, `m9x109`
derives:

```text
restore_address = (R + 200) & ffffffe0
10ec = restore_address | T
1094 = restore_address
1098 = ((R + 3*400*200 - 400) & ffffffe0) | 6
```

The optional form in `m9x109` writes zero to all three registers only when no
restore backing exists.  The vendor first-render construction does allocate
the 1024-entry backing, so zero is a reduced transport diagnostic and not the
full first-triangle setup.

Once all addresses are fixed and CPU initialization is visible, require PCI
bus mastering to be enabled and issue the exact `m9x109` MMIO order [BIN]:

```text
400  = tagged primary command start
109c = 00080000
448  = 00000000
1068 = secondary completion address | its tag
106c = polled completion address | its tag
10ec = tagged restore address
1094 = restore address
1098 = restore inclusive/control value
44c  = 00280800
450  = 1f3f2707
454  = 00070703
```

The three requester controls are the exact fallback for property tuples
`{8,32,24,32}` and `{4,8,8}` and match three warm readbacks on the tested
`102b:0540`, subsystem `102b:2240`, revision 1.  On this board the driver may
write them or require the preserved values to match.  Other M9 products need
their property tuples; they must not silently inherit this profile.

Write `400` only after its command allocation is pinned, but before any `404`
doorbell.  Read it back.  A `dma_wmb()` must publish the command, restore, and
fence CPU writes before `404` is written.

## Per-job MMIO boundary

The service, window, completion-address, restore-address, and requester-control
registers above are construction state.  A normal job does not rewrite them.
After filling the primary and secondary DMA memory, its MMIO sequence is:

1. if and only if the queue moves to a different retired primary slot, write
   `400 = tagged_primary_start` and read it back;
2. clear `31c` through the tested requester path;
3. issue `dma_wmb()` after all command and fence writes;
4. write the single untagged, end-exclusive primary endpoint to `404`, then
   read `404` back as the posting barrier;
5. wait on the coherent word whose address was installed in `106c`.

Thus `31c` and `404` are the normal per-job MMIO writes; `400` is additionally
written only when switching primary slots.  `314=1` is a single allowed
no-progress kick during a bounded wait, not normal submission state.  Neither
`m9x421` nor `m9x109` runs per job.

## Completion gates before the triangle

Use one queue lock and one monotonically extended completion generation:

1. after window and requester publication, submit a fresh three-dword no-op
   completion and require the word bound at `106c` to reach its target;
2. submit sequence 0 using its original three framed ranges and require its
   final completion;
3. publish the first triangle's original framed secondary ranges through the
   three `2c/2d` primary records, and require its completion.

Use an absolute deadline for each gate.  A `314=1` progress kick may be issued
once after observing no progress.  Repeated kicks, `31c=0`, or
`8f0&00030000==0` do not prove completion.  Only the coherent word selected by
`106c` does.

## Save, restore, and timeout

Before the diagnostic changes state, snapshot every register it owns:

```text
30c 330 334 338 33c 340 344 348 34c 350 354 360 364 3a8
400 404 448 44c 450 454
f04 f0c f64 f68 f74 f78 f7c
1068 106c 1088 1094 1098 109c 10ec
1320 1324 1328 132c 1330 1334
```

The saved binaries define an exact save/restore order only for the dormant
auxiliary `500..524` requester.  They do not define a primary `400/404`
timeout reset or an address-window rollback.  Therefore:

* after a successful final fence and with no outstanding BO references, a
  diagnostic may restore ordinary saved configuration registers; never write
  a stale saved `404` endpoint or replay `314` as restoration;
* on timeout, snapshot hardware and both completion words first, stop new
  jobs, mark the generation failed, and do not free any reachable BO;
* abort immediately if `8f0==ffffffff`;
* do not restore windows or requester addresses while command fetch may still
  be active.  If no proven quiescence exists, clear PCI bus mastering, keep
  rendering wedged, quarantine the BOs, and preserve KMS scanout;
* do not use the auxiliary `508` idle bit as proof that the primary requester
  stopped.  It describes a separate requester.

The smallest remaining capture for a complete cold, from-scratch first
triangle is the manager input tuple for `360/364` and `f64/f68`, plus the
command-builder choice between the one- and two-value initial `e6` packet.
Everything after those inputs through the `m9x109` write order is statically
implementable.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
