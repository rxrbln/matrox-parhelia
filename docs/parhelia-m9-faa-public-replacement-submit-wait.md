# Original replacement DRAW: finite second format-transition wait

This bounded source profile continues the same original public replacement DRAW,
without inserting a Lock or another draw. The first surface's aa750 flush already
returned in the qualified earlier prefix. Its rendering and the replacement
request remain separate. The replacement request still has not returned.

Run from the repository root with the installed Unicorn/pefile interpreter:

```
sh tests/m9-faa-public-replacement-submit-source-20261007/capture.sh
sh tests/m9-faa-public-replacement-submit-source-20261007/check.sh
```

The capture reconstructs the exact 12900-byte/230 replacement-preparation source
and joins its distinct hardware receipt in
`tests/m9-faa-public-replacement-hardware-20261007`. Only the original 252b9
requested220 read executes, with actual archived common+00 value231. No opaque
GPU data, GPU status or pixels enter the emulator. Original252b9 requested240
remains unread; the previous230 receipt does not qualify it.

## Finite caller, with explicit diagnostic timeline effects

The actual instruction/event sequence after the covered220 wait is fa6cb,
fa6eb, fa724, fa9e0, 25370, 253eb, 2557e, faa09, 25270. The first wait is the
fa6b5 query/fa6c9 wait. Its returnfa6cb clears renderer+5dc. The separate
fa6eb..fa70e format check compares old renderer+618=f0000400 against new
stack-local format=c4008600. It admits the second fa724 query/fa738 wait, whose
return address is fa73a. The pinned source has no backedge between these two
checks. fa9e0 itself calls queue+68 once and has a straight epilogue. This is a
second finite format-transition barrier, not repeated polling of220.

The independent genuine23b80 finish is an intentional diagnostic caller. The
continuation restores saved registers and stack, but retains its CPU queue
side effects. It advances shared timeline+8 from220 to230. Therefore the second
original query emits240 rather than the uninstrumented numbering230. The
capture is an instrumented source flow, not an assertion that an uninterrupted
vendor call has identical counters.

The observer compares the whole waiting queue180-byte image and the entire
renderer640-byte image before/after the independent finish. Renderer bytes are
unchanged (SHA2564e6e6ff010d5ca5378a44c1d4b6b46296d09f8348ff4c9daee054d9e14f02505).
Changed queue DWORD offsets are exactly4,8,18,c4,d8,dc,108,110: command append,
post cursors and timeline bookkeeping. Queue+94 remains1; cached observed
counter remains1d0 before the real231 receipt is read. No resource release is
executed because23b80 stops before its first23bc3 GPU load. Thus the diagnostic
finish changes numbering and post bookkeeping without changing the renderer
conditions that select the finite second barrier.

## Whole source prefix

New CPU-known source bytes are exactly one secondary span at1992..2008,
produced by actual2554c/25562/2556c/2557b:
`[2,4545e3e6,1,241]`. No new allocation, relocation callback or MMIO action
occurs during this original continuation. All earlier consumed LOCAL and
SYSTEM spans are compared byte-for-byte before flattening source images.
No receipt bytes become uploads. The sampled primary, its staging object,
selector9 descriptor and +3d0 requirement remain unchanged; a CPU pixel view
is not qualified.

A genuine separate23b80 finish emits251 and posts the complete12912-byte
primary, stopping before its own250 completion input. Whole source contains
129 typed callbacks,44 timeline fields and12 direct endpoint sites. This
retains both clear segments and all earlier phases. The new whole-prefix
manifest, source and address audit use stem
`normal-faa-public-shared-depth-replacement-submit-wait-*-101f8.json`.
Native owners, endpoints, clear encodings and counters must still be derived
from their live domains; these source addresses are not portable native values.

The focused checker rejects an invented240 reply, a false public return,
changed earlier inputs, hidden renderer mutation and a wrong wait caller.
The peer strict whole-flow checker/native generator separately validates all
source fields, owners and native placements. New actual250 qualification is
required before resuming the original240 wait. No FAA image claim is made.
