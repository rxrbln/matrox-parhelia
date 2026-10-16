# Retained in-place resolve and sampled CPU preparation

This extends the genuine scoped temporary-allocation failure control, without
changing the vendor return, resource fields or caller stack. The exact new
`170` hardware receipt is joined to the original `252b9` consumer requesting
`160`: common channel0 is observed `171/0`, channels20/40/60 are observed
`10/0`. The source and native timeline origins are both10. The hardware receipt
is in `tests/m9-faa-inplace-prefix-hardware-20261007` in canonical RE.

The original `abc20` tile resolve returns through `ac139 → ac503`, then
`afbe0` selects the retained sampled primary. Its copy request has read flag1,
caller pitch256, destination0/0 and source rectangle0/32/64/96. The caller's
original source rectangle128/96/192/160 was translated by actual47720 using
companion tile origin128/64. The source resource remains kind0, class2,
hardware format66,320×240,extent50000, selector9 `cc220140/100f0`.
Only backing+30 changes from timeline150 to190; this is not a format change.

`afbe0` first calls `4d2b0(resource,2)`, including actual wrapper preparation
`4d36e → a5ae0 → a7ce0`. It reaches a new `252b9` wait requesting190 before
that preparation returns. No `b0f30`, ordinary mapper `2cb40`, `55550` or
pixel entry `8b390` has executed in this continuation. The later kind0 tiled
copy remains pending. Therefore neither the older kind1 SYSTEM-output
postwait contract nor a guessed F022 read qualifies these caller pixels.

The original160 read alone received the verified170 receipt. No later counter,
status, GPU-owned BAR input, mapped-MDL unknown byte or pixel was supplied.
Opaque pointer-bearing GPU records remain refused even when a raw hardware
archive exists. CPU-known BAR writes are tracked by live owner and offset,
not aperture location. Exact earlier source-known LOCAL and SYSTEM spans
remain unchanged; the continuation appends command bytes.

An independent genuine23b80 finish emits the complete5936-byte source prefix
and new deadline1a0, stopped before23bc3. The plan contains30 live owners,
98 typed relocation callbacks,60 non-null/direct fixups,29 timeline fields,
six direct secondary endpoints and125264 known input bytes. Both original
clear segments are retained and regenerated from live native placements.
There is no ordinary output owner; `M9_FAA_OUTPUT_PENDING=1` and
`M9_FAA_HAS_OUTPUT=0` prohibit image publication.

Earlier c0/120/170 prefixes match by actual resource backing/MDL identity,
encoder, source offset, address tag, timeline and all other known bytes.
This is a source/CPU proof, not a GPU result for the appended prefix.

Reproduce from the worktree root with:

```sh
sh tests/m9-faa-inplace-copy-source-20261007/capture.sh
sh tests/m9-faa-inplace-copy-source-20261007/check.sh
```

The standalone actual-native C fixture also passes128 placements/alignment
combinations under ASAN/UBSAN, including borrowed color BO admission,
semantic inactive raster masks, exact suffix/other-owner preservation and
corruption/extent/capacity refusals. Wrong counter input, opaque result input,
wrong source rectangle and false completed-pixel claims are rejected.

A real new1a0 receipt is still required before continuing the sampled CPU
preparation. No FAA image success is claimed. This checkpoint executes no hardware action.
