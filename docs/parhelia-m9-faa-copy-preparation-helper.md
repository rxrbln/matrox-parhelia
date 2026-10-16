# Source conversion-wrapper preparation with the existing allocator

The fresh helper fixture executes the shipping MD `2f899..2f8b8` loop after the
original format publishers and `86660` constructor. All three PDEV `d0/d4/d8`
wrappers and their backing resources finish with allocation association `+1e4`
nonzero and readiness `+234=1`. This is CPU resource initialization, not GPU
completion or rendered-pixel proof. It does not complete public mode admission,
normal Lock, or the original FAA caller.

## Corrected service prerequisite

The conditional allocator fixture already calls genuine `19420` through
`m9_shared_allocator.initialize`. Hardware `+35f8` and PDEV `+6984/+6988/+698c`
are therefore live before this constructor. The earlier missing-service
hypothesis was wrong. This helper checks and reuses that existing service; it
never calls `19420` or `19560` again.

`19420` is not a safe repeated initializer for the primary circular-device
case: `19432/19436/19439` takes `19444` when the hardware root and its `+28`
link both equal this PDEV, even if a service already exists. Failure at `1944b`
then calls cleanup `194a0`. A disposable repeat experiment failed and cleared
the service. It did not change product code, retained captures, or hardware.

## Real producers and owned allocation replies

The original loop loads wrapper virtual slot `+30` and calls it with `(PDEV,1)`
three times. `86a20` obtains the existing PDEV `+6988` allocator and calls
wrapper `+28=86a60`. `86a60` first calls backing `+28=48950`; the latter
publishes allocation validity at `48968`, or calls real `455c0` and publishes
its result at `4897b`. Backing association is stored by `4dfec`. Real `48a40`
then builds the wrapper view; `4e7c4` associates its backing allocation and
`48968` sets its readiness. All six associations/readiness writes are recorded
from original instructions, without manual field writes.

The backing extents are `80000,200000,200000` bytes. The existing bounded OS
host admits contiguous requests only up to one MiB. The two mode-one backings
make genuine `232284` requests for `201000` bytes each, with request flags one;
MM asks `MmAllocateContiguousMemory(201000,ffffffff)` and produces complete
24-byte replies. This helper adds exactly two such OS allocations, preserving
page alignment, disjoint physical/virtual owners, and the existing separate
common-buffer namespace beginning at `02000000`. The returned physical and
virtual addresses are synthetic owned OS RAM, not captured GPU addresses.
Initial contents are unspecified; none are treated as rendered data. The
ordinary host implementation and its default bound are unchanged.

The denied-allocation control keeps the original one-MiB bound. It fails on the
first larger request, leaves d0 and its backing ready, and leaves both d4/d8
wrappers/backings unready. No failed allocation is replaced by success.

## Mapping timing and remaining consumer scope

A strict aperture observer corrects the earlier prototype's MMIO-free claim.
During **d0 local-backing preparation**, original `5b3c0` and `5b750` consume
the same source-produced eleven-word CPU-stack descriptor: eight zero words,
first GPU page `e99`, page count `80` (128), and zero. The real `59b82` stores
emit `31c=f,0,1,5,0,2`. No MMIO read or GPU reply is supplied. Both publishers
and every command occur before either new SYSTEM allocation request.

This matches the established compact/secondary publication grammar in
[secondary-page-view](parhelia-m9-faa-secondary-page-view.md) and
[output-system-admission](parhelia-m9-faa-output-system-admission.md). It is not
an FAA enable sequence to replay. The `e99` page is an allocator output of this
CPU experiment, not a native input. The source descriptor and command timing
are bound to the actual d0 backing owner in the retained proof.

The two contiguous SYSTEM requests do **not** call these compact/secondary
publishers during this preparation loop. Their request flag one is an OS
allocation contract, not evidence for the flags30/B GPU PTE policy established
for the different ordinary output surface. Any later conversion GPU consumers,
page policies and lifetimes must be joined through the new whole source fixture.
Native `m9_address.c` already derives compact/secondary publication for admitted
owners; this proof adds no native MMIO replay.

## Reproduction and fresh-fixture boundary

```sh
PY=/home/ai/tmp/faaemu/venv/bin/python
$PY tools/m9-vendor-faa-copy-preparation-helper.py --out /tmp/preparation.json
$PY tools/m9-vendor-faa-copy-preparation-helper.py --deny-conversion-ram \
  --out /tmp/preparation-denied.json  # expected exit 1
$PY tools/check-m9-faa-copy-preparation-helper.py \
  --proof docs/traces/m9-faa-copy-preparation-helper-20261007.json \
  --denied-proof docs/traces/m9-faa-copy-preparation-denied-20261007.json
```

The checker pins eight instruction spans, image/input/implementation hashes,
three backing/wrapper ownership pairs, six readiness producers, exact request
and reply fields, retained OS extents, mapping ownership/timing and nine
contradictory controls. The reusable initializer is
`tools/faaemu/m9_pdev_copy_preparation_helper.py`; it must run after the genuine
constructor, before queue/draw initialization, using the existing service.

Adding constructor/preparation to a FAA capture changes earlier allocation
ownership and produces mapping commands. A **new complete source fixture and
new owner/address/layout qualification** are required. No previous physical
receipt may be attached without whole-prefix equivalence. This result is an
initialization prerequisite for the real conversion path, not an explanation
or fix for the currently black FAA image.

<!-- SPDX-License-Identifier: GPL-2.0-only -->
