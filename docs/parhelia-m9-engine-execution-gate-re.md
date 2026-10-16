# M9148 cold/warm engine-to-execution gate

> **Correction (2026-09-15):** `0x400` is written first in `m9x109`, not
> "last" as the plan below says.  `m9x1172 -> m9x115` publishes the 20-slot
> buffer window table at MMIO `0x000..0x13f` (all-invalid `0x0000ffff` on the
> first-render path); `m9x865` allocates the two completion dwords with tag
> `mode ? 0 : 2`; `m9x926` has no direct MMIO but its four mode-0
> allocations publish identity PTEs and `31c=f,1,0` through
> `m9x1030 -> m9x887 -> m9x328`; `m9x301` has no MMIO. See
> `parhelia-m9-cold-transition-submit-decode.md` section 4.

This note closes the initialization ordering immediately before the captured
sequence-0 setup stream.  It joins the saved Linux X driver, the proprietary
Linux kernel core, the Windows miniport, and the modern target's read-only warm
snapshot.  It does not claim a new global-reset register or make an
electrically cold board generic across M9 products.

Recheck the binary slices, call relocations, register order, Windows
corroboration, warm-state boundary, and sequence-0 fixture with:

```sh
python3 tools/m9-engine-execution-gate-evidence.py
```

The verifier reads files only.  It never loads either vendor driver or opens a
PCI device.

## Result

There is no hidden per-3D reset or clock sequence between the already recovered
Phoenix board initialization and sequence 0.  The vendor separates these
phases:

```text
uninitialized board only:
  reset/config protocol -> display clock holds/blank -> IRQ mask/ack
  -> system PLL + memory-controller programming -> destructive VRAM test

every render-service construction:
  PCI bus master -> service/address-window construction
  -> job-derived window publication -> fence allocation
  -> primary requester construction/arm -> sequence 0 -> render stream
```

The first line is a board and memory transaction.  It is not a 3D-engine
reset.  The second line is the exact execution gate relevant to the new DRM
driver.  A firmware-POSTed target must use the second line and must not rerun
the destructive memory transaction merely because render windows are empty.

## Cold board phase

Linux X routine `m9x25201` at `0x2b5650` reads the global initialized state and
calls `m9x21171` only when it is zero [BIN].  Its M9148 loop is [BIN]:

1. `m9x4358`: save PCI/MMIO state, toggle the exact `0x358` reset/configuration
   protocol, wait 5000 microseconds, then set `0x318` bits 28 and 29;
2. `m9x7139(board, 4)`: disable/reset the VGA/display route;
3. `m9x15067`: hold controller clocks and blank the physical outputs;
4. `m9x2399`: mask/ack interrupt state and install common defaults;
5. `m9x31786`: dispatch device `0x0540` to the Phoenix PLL, memory-controller,
   and VRAM-test transaction.

The detailed PInS-patched PLL and MC values are pinned in
`parhelia-m9-cold-init-re.md`.  Failure causes the vendor to retry the whole
transaction up to 2000 times.  A clean-room driver should retain the exact
ordering but use bounded polls and attempts, preserve failure, and keep the
requester disabled.  The vendor's final unconditional initialized flag after
failure is not a safe behavior to reproduce.

Neither this Linux cold routine nor the Windows global PLL callback at
`MxMM.sys` `0x179a2` publishes `0x400/0x404`, fence addresses, address windows,
or a 3D context.  The Windows callback's table-indirect owner still prevents a
static claim about the exact Windows cold/warm branch, but its masked-IRQ,
PLL-lock, and delay sequence independently confirms that it is global clock
setup rather than a render requester reset [BIN].

## Bus-master boundary

The Linux X enable orchestrator `m9x30894` at `0x294670` prepares the bridge,
reads PCI configuration selector 4 through vtable slot `+0x68`, sets bit 2,
and writes selector 4 through slot `+0x58` [BIN]:

```text
PCI config 0x04 |= 0x0004
```

It does this before its later display/service construction.  This is the
`PCI_COMMAND_MASTER` prerequisite.  No primary endpoint is posted before the
bit is set.

## Service registers are not a reset

The anonymous proprietary-core initializer at `.text+0x121d0` creates a
service descriptor and writes this fixed register sequence [BIN]:

```text
read  f04
write f04 = old & cfffffff
344=0  348=0  34c=0  350=0
354=03020100
334=0  338=40  33c=80  340=c0  330=1000
conditionally update f0c with 8000 and device-property bits 9:6
```

Its descriptor publisher `m9x1172` also clears `0x30c`, updates only bits
17:16 in `0x3a8`, and publishes a service table.  The requester timeout and
restart paths never call it.  These writes configure service lanes and address
translation; they are not evidence for a GPU reset [BIN][DER].

The modern M9148 warm snapshot is useful here.  After the clean-room module was
unloaded, `0x338/33c/340/354` retained `40/80/c0/03020100`, while `0x330` was
zero.  Thus the fixed lanes can survive a client, but their presence is not a
render-ready predicate.

## Exact proprietary-core order

The two service-owner implementations are identical around their construction
tail.  `m9x582` executes [BIN]:

```text
m9x1187 -> m9x421       publish address windows with argument 1
m9x865                  allocate and initialize the two completion domains
m9x949 -> m9x182        construct primary command resources and restore pool
              -> m9x109 arm the primary requester after those resources exist
m9x926
m9x301
m9x1141
m9x115
```

The verifier checks the ELF relocations at each call site, not just function
names.  This fixes two ordering rules for the new kernel:

1. window bases and inclusive last-page limits come from live, pinned job
   resources and are published before the requester is armed;
2. the command and fence addresses used by `m9x109` must already have backing
   storage and stable GPU addresses.

### Address-window publisher

`m9x421` receives one as its third argument from `m9x1187`, so it executes the
main MMIO window group [BIN]:

| register | exact construction |
|---:|---|
| `360` | `base & fffff00c` |
| `364` | `base + size - 1000 + auxiliary_offset` |
| `f64` | selected aperture base |
| `f68` | selected aperture `base + size - 1000` |
| `f74`,`f78` | second selected base |
| `f7c` | second selected `base + size - 1000` |

The `1320/1324/1328` and `132c/1330/1334` triples are not one unconditional
fixed preamble.  They lie on separate capability/resource branches.  Each
active triple is `{base, base, base + size - 0x1000}`.  On one branch the first
base also feeds indexed mirror `cb/cc/cd`; the other mirror uses `c8/c9/ca`.
A new driver must select the family from its validated resource role.  Writing
all six registers from a copied capture would erase the distinction the vendor
preserves.

The existing warm target snapshot found `f64..f7c` and all `1320..1334`
registers zero.  A successful three-dword no-op fence therefore proves command
fetch and completion only.  It does not prove that a color, depth, scratch, or
linked-record address is reachable by the render engines [HW].

### Primary requester arm

After command, restore, and fence resources exist, `m9x182` calls `m9x109`.
The direct MMIO order is exact [BIN]:

```text
400   = tagged primary command start
109c  = 00080000
448   = 00000000
1068  = first completion address and tag
106c  = second completion address and tag
10ec  = optional aligned context address and memory tag
1094  = optional aligned context address
1098  = optional context limit/control
44c   = packed partition control 0
450   = packed partition control 1
454   = packed partition control 2
```

There is no `0x1070` write in the function.  The tested M9148's three controls
are `00280800/1f3f2707/00070703`; this is both the recovered fallback tuple and
the stable warm readback.  Other products require their property tuple or a
separate verified readback.

Windows independently separates arm, post, and completion.  `MxMM.sys`
`0x224c8` writes the primary base at `0x400`; `0x224f3` writes the exclusive
endpoint to `0x404`; and `0x22533` emits and polls the completion record through
the address bound at `0x106c` [BIN].  None calls the global PLL callback.

## Sequence-0 boundary

Only after the service and requester phases does userspace submit the captured
three-range sequence-0 envelope.  That stream contains no reset, clock,
requester-base, PCI-master, or MMIO window operation.  It installs default 3D
state, the invariant 80-slot native image, scratch transitions, and a final
completion.  Its exact 220 private-record boundaries and 17 typed patch sites
are pinned in `parhelia-m9-sequence0-inline-boundary.md`.

This yields a precise warm first-render rule:

```text
require accessible POSTed M9148 and tested requester controls
enable PCI bus mastering
allocate/pin every command, fence, system, VRAM, and scratch object
publish only job-derived address windows through the proper family
arm the complete m9x109 primary state
run a bounded no-op completion
run sequence 0 and require its final completion
submit the linked-record triangle and require its completion
```

The no-op may be retained before window publication as a transport diagnostic,
but it cannot replace the post-publication completion check.

## Guarded kernel implementation plan

The narrow implementation belongs behind the existing M9148 diagnostic module
parameter and should use one serialized queue:

1. Add an `engine_generation` state with `OFF`, `TRANSPORT_OK`,
   `WINDOWS_READY`, `SEQ0_READY`, and `FAULTED` states.  Never expose executable
   user submission before `SEQ0_READY`.
2. After the first-triangle allocator fixes every GPU address, validate that
   each base and inclusive last-page limit is 32-bit, page aligned where the
   vendor requires it, and contained in its pinned BO.  Reject overlapping
   roles unless the captured alias is explicit.
3. Under the queue lock, save the service/context/window registers touched by
   the diagnostic.  Program the fixed `330..354` service image, then the exact
   `360/364`, `f64..f7c`, and selected `132x` window families.  Do not copy the
   warm `360/364/f04/f0c` values.
4. Fill the remaining `m9x109` fields from the kernel command, context, and
   fence BOs.  Preserve the verified partition controls.  Publish CPU writes,
   enable bus mastering, write `0x400` last, and read it back before `0x404`.
5. Require a new bounded no-op completion after window publication.  Then post
   sequence 0 using its original three ranges and private-record boundaries;
   require the range-2 completion before declaring `SEQ0_READY`.
6. Submit the first triangle only after its selector-`6c` record is serialized
   into the system BO.  Fence every referenced BO through the same completion
   generation.
7. On failure, stop submissions, snapshot the registers and both completion
   words, disable bus mastering after the bounded requester stop, restore the
   saved diagnostic registers where safe, and enter `FAULTED`.  Do not invoke
   the cold PLL/MC transaction or the auxiliary service descriptor as a guessed
   3D reset.

The ordering, fixed service image, main window formulas, conditional surface
families, requester arm, and absence of an additional reset are statically
closed.  The exact association between the kernel diagnostic's individual BOs
and the conditional window families must come from the separately audited
VRAM/render-address join.  Selector `6c` construction remains a program-record
problem, not an engine-initialization problem.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
