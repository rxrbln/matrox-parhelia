# Original FAA output write post after the qualified160 prefix

This bounded source experiment resumes the retained original `ac1f0` stack,
not a new resolve caller. The actual160 receipt comes from
`tests/m9-faa-preclear-output-hardware-20261007`: common+00 is161/0 and
+20/+40/+60 are10/0. The full source manifest, prior live owners, MDL/PFN
joins, stack and already-consumed CPU source snapshots match before resuming.
The only supplied GPU values are three recorded counter reads:252b9(+00,
requested150),4e2fc(+20,requested0),4e32d(+20,requested0). No pixel or opaque
GPU buffer input is supplied.

The real `abc20` returns1 through `ac139` and `ac2d9`. Original `afbe0`
prepares its retained temporary output: ECX+4 names that resource, while
ECX+38 is a separate copy field. `aff90` enters `2cb40` with actual arguments
(resource,0,1). This is allocationkind1/logicalformat6/hardwareformat64,
320x240,extent50000,descriptor9=[liveGPU|1,c8210140,100f0]. Its flags30,
80-page SYSTEM backing and unknown pixel bytes are preserved.

Original write preparation itself calls25370 twice,24d60,24530 and2461e.
It appends500 source bytes to the earlier5224B stream and posts the complete
5724B primary, whose own last12B are E3/1/181. There is no independent finish
or fabricated footer. Execution stops before the actual `4e1d1` common+00
read requesting180: the160 receipt cannot satisfy it.

The current live kind1 mapper/vtable/arguments and exact mapper/writewait
instructions join the previously qualified
`normal-faa-output-receipt-continuation-public-red.json`. That original
postwait source path has no new command-owner writes and preserves every
command-owner hash. Its real next GPU action is2cba8 MMIO314=1 before kind1
CPU mapping/pixelcopy; later31c writes are actual cleanup/publication.
The existing native private output reader already retains that314 ordering.
This join permits OUTPUT_PENDING=0 in the independent native180 image test;
it does **not** claim this new source call has returned, that its180 wait has
completed, or that FAA pixels are correct.

The complete profile has31 owners,90 typed operands,52 non-null/direct
fixups,27 timelines,six direct endpoint fields and125052 source-known upload
bytes. Both source clear segments retain actual151 and80 logical pages;
native encoders regenerate their variable words from live owners. All prior
consumed LOCAL and SYSTEM known spans are unchanged across the160 boundary,
so this profile preserves their temporal source inputs rather than merging
changed snapshots. Output pixels stay entirely unknown/unuploaded.

`m9_faa_retained_read_guard.py` keys new CPU-known BAR writes by live owner
index and owner-relative offset, so changing a CPUroot mapping cannot carry
knownness to a different owner. Every opaque GPU-owned BAR read is refused,
even when a prior raw receipt exists; interpreting such a receipt would need
consumer ABI and pointer-domain translation. A prior receipt does not qualify
later GPU writes. The continued flow executes zero such reads, and rejects
unknown mapped MDL bytes. No raw pool pointer is transported to the emulator.

Reproduce from the RE worktree:

```sh
sh tests/m9-faa-preclear-output-write-source-20261007/capture.sh
sh tests/m9-faa-preclear-output-write-source-20261007/check.sh
```

The source checker joins all CPU producer writes, PFNs, domains, timelines,
original post and previous receipts. The host proof exercises actual vendor
clear instructions at two placements and all8×8 clear alignments, preserves
all unpatched sparse bytes and unknown output, and rejects seven malformed
source/count/owner/retirement controls. Native C and physical pixels are
separate qualifications. Existing init/post/preclear profiles remain strict.

## Actual hardware result

Kernel65206bc on the M9128 completes the whole original resolve at180/raw181,
queue empty/wedged0 immediately and two seconds later. The actual image is
all black. CPU mapping positive control passes76800 pixels; neither main nor
AA companion receives color. Nine source-named BAR buffers and logs are in
[the hardware archive](../tests/m9-faa-preclear-output-write-hardware-20261007/README.md).
The public pixel shader writes constant red despite green vertex records.
The initial helper defaulted to green; corrected red evaluation of the same
archived image still fails with zero coverage and8326 bad interior pixels.
FAA rendering remains unqualified and broken for this bounded diagnostic.
## Actual180 receipt and raster/color audit

The root-owned physical test retires180/raw181 but remains all black. Main
and AA buffers stay zero; preprocessing remains the earlier qualified
transformed/varying output. The intended final color is **red**, independent
of the green vertex varying: genuine CREATEPS54 `mov oC0,c0`,SETCONST57
`c0=(1,0,0,1)` and SETPS56 precede the public clear/draw. The complete active
240B program matches the passing ordinary SYSTEM red program byte for byte;
the current136B constant image also matches, including that exact red c0.
A green readback expectation for this profile is incorrect. Zero output
fails both expectations and remains an actual FAA failure.

`check-m9-faa-preclear-raster-contract.py` verifies the actual second
17e480 emitter writes `[23000001,0,3]` at secondary540 via17e495. Its full
second source range `[664,1356)` is referenced by the live-owner native
primary at offsets5072/5076, within the posted stream. This establishes
source-issued raster dispatch, not successful hardware raster execution.
Selected packed indexed controls22,A8/A9/AA,F5/F6/C4/C5,88/89/8a/C1 and
EC/F4/EA/EB agree with the independently passing ordinary red source.
F0/EF/D7 intentionally differ by the actual FAA shadows; graph/pool/HZ
bindings differ too. Equality of packed controls does not prove identical
GPU interpretation or establish which FAA prerequisite rejects fragments.

The actual180 receipt continuation supplies only the observed181 at
4e1d1/4e203 on the original write-wait stack. It reaches2cba8 MMIO314=1
then8b390, where it stops before unknown pixels. It emits zero command-MDL
writes and consumes zero opaque BAR data. Thus this prefix omits no new
postwait raster/resolve command stream; original pixelcopy/return still
remains unexecuted. Root already tested the real314 ordering on hardware,
so emulation of that transition is not a claim that it materializes pixels.
The nine retained raw BAR roles are read-only evidence, not portable pool
pointers or emulated GPU replies.

## Bounded saved pool field joins

The nine-role raw receipt is now joined to source-selected pool cells by
`check-m9-faa-pool-live-state.py`. It verifies the original24f20 transition,
including24ffe's literalA1, original25039's64B interval, and native typed
range284..2c4. Those offsets are exactly4*A1..4*B1. Within that source-defined
interval, pool+A1*4 agrees with actual sentinel+8, and A3/A4 likewise agree
with sentinel+c/+10: all three saved pointer values are zero in this test.
The pool itself is not zero and is not a list of proved colored fragments.

Selected other cells match individually source-emitted indexed fields and
live native owners:00/01/02 contain the retained preprocessing view pointers,
24 names the retained pool,69 names the uploaded RS database, andB0/B8/BA/
BC/BD name actual output/companion/HZ/main/aux owners. CorrespondingB1/B9/BB
values match the source output/companion/HZ descriptors. A8 andAD agree with
raster control/logical footprint; EF/D7 agree with final resolve state. The
successful joins establish bounded saved-state correspondence and that
these pointer-bearing cells refer to current owners. They do not establish
a universal pool ABI, successful render-target memory writes or semantic
meaning for unlisted opaque cells. No pool data is transported as emulator
GPU input. Old RED A1=24a and new public-preclear A1=0 remain separately
qualified profiles, not interchangeable receipts or a guessed reset fix.
