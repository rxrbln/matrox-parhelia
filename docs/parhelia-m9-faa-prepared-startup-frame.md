# Fresh prepared-conversion FAA source prefix

This separate whole source fixture runs the genuine conversion constructor and
preparation **before queue creation**, then executes the normal sampled-surface
FAA initialization and public constant-red/preclear request. It stops at the
first real GPU input: original `4e2fc`, common-buffer offset `20`, before that
read executes. The original public CLEAR/DRAW caller remains pending.

An independent genuine queue finish exports the complete initial command
prefix: 4980 primary bytes, 36 typed operands, 14 timeline stores and actual
retirement target `c0`. That finish stops before original `23bc3` reads the GPU
completion counter. No old receipt, GPU data or pixel reply is supplied.

The retained source artifacts are
`captures/m9-faa-prepared-startup-20261007/{startup,source,manifest,addresses}.json`.
They form a new allocation graph and cannot be substituted for the old source
profile by copying counters or pointers. Whole-prefix equivalence or fresh
native owner/address/layout qualification is still required.

## Initialization ownership and source constants

The new controller leaves all existing hashed baseline modules unchanged. Its
allocator wrapper retains the original returned frame. At the original queue
initializer boundary, it executes the separately proved
[constructor](parhelia-m9-faa-copy-constructor-helper.md) and
[preparation](parhelia-m9-faa-copy-preparation-helper.md), using the **existing**
source-created service. It never repeats `19420`/`19560` or sets ready fields.
The original queue and complete public-prefix producers then run normally.

The original source constructor publishes three distinct PDEV `d0/d4/d8`
wrappers. Original preparation associates all three backings and wrappers and
sets all six resource-validity markers to one. Two complete `232284` replies
retain separate `201000`-byte contiguous OS allocations. Source mapping
commands `31c=f,0,1,5,0,2` belong to the d0 local backing's compact/secondary
publication before either SYSTEM request; they are not replayed as an FAA
switch. Later conversion consumers and SYSTEM mapping policy remain separate
proof obligations.

The constructor's explicit controlled GDI input is 1024×768, 32-bpp, 60 Hz.
Original `105f0` converts it to format code `40` hex, and original format
publishers derive storage bytes four and hardware format `66` hex. These are
bounded helper inputs and source outputs, not full display-mode admission.
The source mode-query/caller initialization remains bypassed; this fixture
must not be described as a completed Windows startup. The FAA surface and
public clear remain the explicitly requested 320×240 test surface.

The baseline frame initializer (`tools/faaemu/m9_faa_frame.py`, lines 19–20)
previously proceeded from allocator/I/O initialization to queue construction
without these wrappers. That omission is a proven **normal Lock initialization
prerequisite gap**, not proof of the cause of black FAA rendering. This new
startup establishes the earlier objects needed for later original normal
Lock/staging consumer execution. It does not itself enter Lock or prove pixels.

## Bounded MDL namespace guard

The old OS host's contiguous allocator checks the separate common-buffer
physical namespace at `nt-host.py:114`; its MDL allocator at lines 99–107 does
not apply that upper bound. Current fresh source allocations do not overlap:
34 live MDL owners end at `01c58000`, below common-buffer namespace `02000000`,
and remain disjoint from both new contiguous owners.

The **new controller only** wraps original `MmAllocatePagesForMdl` requests.
Otherwise-successful allocations crossing `02000000` are refused before any
allocation or state change. Safe calls delegate the exact original arguments;
requests already excluded by their OS highest-address bound retain the original
failure result. The baseline host and its historical receipts are unchanged.
Six controls cover the final valid page, zero length, OS-bound failure, and
three crossings without delegating or changing the allocator's next address.

A source recapture with this guard preserves every current source-known MDL
write/span, retired-footer write/span, typed operand, resource snapshot,
payload encoding, DMA owner, relocation and code event byte-for-byte against
the immediately preceding unguarded prototype. This is host allocation
noninterference, not GPU equivalence or hardware rendering proof.

## Verification and handoff

```sh
sh tests/m9-faa-prepared-startup-source-20261007/capture.sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-prepared-frame.py \
  --capture-dir captures/m9-faa-prepared-startup-20261007
```

The checker reuses the complete public-prefix owner/PFN/known-byte validator,
then joins actual startup constructor/preparation evidence and hashes. It
checks readiness producers, the unchanged live service, both new OS replies,
disjoint physical owners, namespace controls and five contradictory snapshots.
The initial source prefix passes the same 36-operand/14-token/4980-byte grammar
as the old preclear shape, but that shape equality is insufficient for receipt
reuse. No native plan is silently imported or hardware qualification claimed.

Inputs owns independent normalized whole-prefix equivalence/new native plan
review. Render owns the later original public DRAW return and normal Lock
request on this **new** initialized graph. Further source continuations require
new qualified receipts or rigorously established prefix equivalence first.

<!-- SPDX-License-Identifier: GPL-2.0-only -->
