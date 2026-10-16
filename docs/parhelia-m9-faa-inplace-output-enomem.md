# Genuine temporary allocation failure selects the sampled primary

The bounded internal `ac1f0` request has a real fallback destination. The
successful branch resolves to a newly allocated ordinary SYSTEM temporary;
when that allocation fails, it selects the retained sampled primary instead.
This is a diagnostic source control below the still-unjoined public Lock
admission. It is not a product fallback or a working FAA image claim.

The control keeps the original public constant-red shader, black CLEAR42,
triangle, sampled graph, owners and CPU-produced state. It resumes the original
triangle only with the independently qualified120 receipt. It then supplies
one legal OS ENOMEM result: `MmAllocatePagesForMdl` returns NULL solely for the
actual internal temporary request of327680 bytes. All other OS allocations
use the existing handler. No driver field, driver return value, GPU counter,
GPU-owned pixel or instruction is patched.

## Actual source propagation and rectangle contract

Pinned `MxMM.sys`72098..720b1 calls the OS allocator and tests its result. The
NULL branch720f1 returns error8. The actual MD allocation at51870 returns0;
ac2a0 preserves that in EBX and ac2a4 takes ac408. The observed sequence is
ac1f0→51870→ac2a0→ac408→47720→ac4bd→abc20. No Afbe0 or pixel copy is reached.

The caller requests source RECT `[128,96,192,160]`, destination RECT
`[0,0,64,64]`, stride256 and an owned16384-byte CPU buffer. These are caller
buffers, not resource state assignments. The temporary still requests320×240
because ac24d..ac25f derives its dimensions from the companion resource.

The failure branch computes64×64 tiles at ac408..ac480. Actual47720 reads the
companion layout and returns tile origin `[128,64]` and byte offset0x28400;
the input coordinates remain `[128,96]`. At ac4bd..ac4e0 the source subtracts
that tile origin from the rectangle bounds. The actual abc20 arguments are
`(primary, companion, main, 0,32,64,96)`. The last four values are local
left/top/right/bottom bounds, not x/y/width/height. The pointer roles come from
actual graph+4/+8/+c at ac4e3..ac4fd. Neither the computed byte offset nor a
captured pointer is independently transplanted into native state.

The separate allocation-failure continuation after abc20 uses graph+4 as the
Afbe0 source at ac585/ac588. That part is statically decoded, not executed in
this capture: actual252b9 first requires a new160 completion. The original
stack, rectangle launch and CPU copy remain pending.

## Whole prefix and native admission

Artifacts use the stem
`captures/m9128-device-inputs-20261006/normal-faa-preclear-inplace-output-wait-`.
The source, input manifest and direct-address audit are captured from this
whole changed flow; they do not reuse a successful-temporary tail.

The independent real23b80 queue finish posts5304 source bytes and requests170,
stopping before23bc3 loads status. The plan contains30 retained owners,
48 live-owner fixups,25 timeline fixups, six direct secondary endpoints and
124632 source-known bytes. Its two clear segments retain their actual logical
extents,97000 and50000. Native addresses, clear masks/counts, encoded references
and final stream lengths remain derived from live owners.

The generated header defines `M9_FAA_PRECLEAR_INPLACE_OUTPUT_CONTROL=1`,
`M9_FAA_HAS_OUTPUT=0` and `M9_FAA_OUTPUT_PENDING=1`. The sampled destination
remains the borrowed color owner; there is no ordinary output owner to index
or expose. Its pixel bytes remain unknown. The source-qualified one-active
color target metadata exports inactive mask normalization without replacing
raw source residue in the blob.

The prefix checker joins c0/120 owners by actual resource backing objects or
retained MDL/page-prefix identities, then compares all encoder fields,
source addends, tags, timeline values, both clear segments and every other
prior known byte. Both prefixes match. The native CPU fixture passes two
placements×eight companion alignments×eight primary alignments with ASAN and
UBSAN, suffix/other-owner preservation, borrowed BO ownership and rejection
checks. All six existing phase plan/blob/header outputs remain unchanged.

Run `sh tests/m9-faa-inplace-output-source-20261007/check.sh` for source and
prefix verification; the fixture command is in that directory's README.
Five negative controls reject a driver-return override, wrong OS extent,
wrong tile origin, invented next receipt and a false completed output.

## Remaining exact dependency

The original160 wait and independent170 finish have no hardware receipt for
this changed prefix. The old successful-temporary160 receipt cannot qualify
it. Qualification must preserve source phase ordering c0→120→170 and retained
live owners. Only after real170, with per-channel identity/timeline joins, may
its original160 stack continue until the next unknown GPU input. This prefix
alone has not completed the fallback rectangle or tested an in-place image.
