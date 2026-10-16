# Public clear and receipt-qualified preprocessing

The real preclear prefix retires c0 on M9128 (kernel 7a270ae, module
5aeca397, boot 1c9a1824). The archived receipt is
`tests/m9-faa-preclear-hardware-20261007/proof/dmesg-after.txt`:
common+00 is c1/0; +20/+40/+60 are each 10/0. Native/source timeline
origins are both 10. The source continuation supplies each observed value
only to the same live common owner/channel and only when its masked point
covers the actual requested deadline. It does not supply c0 to +20.

An observer installed before public execution corrects the earlier scope
prose: added CLEAR42 goes df9d0 → dfa13 (75730 returned 1) → dfa2f →
dfa32 → dfa52 (actual renderer 13de20) → dfa54 (returned 1) → dfa61.
**All occur before the first query boundary.** The clear handler completed
on the CPU; the whole public DP2 request remains unreturned because its
subsequent original triangle has not completed. Historical signed capture
bytes/hashes are retained; its contrary prose was wrong.

The complete matched prefix, owners, PFNs, known bytes and first genuine
23b80 finish are checked before restoring the original interrupted CPU
stack. Only four covered reads execute: 4e2fc/4e32d at +20 with actual10,
and 4e1d1/4e203 at +00 with actualc1, all requested0/0. Source then produces
one genuine 17e480 preprocessing launch and stops **before** 252b9 reads
common+00 for deadline110/0. No110 reply or GPU pixels are supplied.

The wait caller fa6eb..fa738 compares the old format's fe000000 bits to
f0000000 and the newly selected format, then waits via queue vtable+6c.
Static fa73a..fa865 clears renderer+5dc, emits 454545e6/42000000 and
publishes renderer+610..63c before returning. That immediate continuation
reads CPU renderer/queue state, not GPU-produced allocation bytes. This
is a format-transition boundary, not established resolve completion.
Any later GPU memory/status input remains strictly unread until observed.

A second independent genuine23b80 finish qualifies a new whole profile,
`preclear-draw-wait`. It produces a final120 deadline and stops before
23bc3's status load. Its121 E3 token lives in counted secondary owner29
at offset294. Primary exports secondary ranges [0,604) and [604,664)
through four source-proven direct endpoint stores. The last12 primary
bytes are the second2c/2d export, not a fabricated inline E3 footer.

| Whole profile | Value |
|---|---:|
| Retained owners | 30 |
| Actual typed operands | 51 |
| Non-null/direct native address sites | 20 |
| Timeline sites | 20 |
| Primary bytes | 5068 |
| Known input bytes | 123704 |
| Pending original deadline / independent finish | 110 / 120 |
| Ordinary output owner | absent |

The earlier source-known primary prefix [0,4980) is unchanged and only
appended. Constants [0,136), geometry [256,444), and secondary [0,648)
are newly known. Every previously known LOCAL input remains byte-exact.
The sentinel's CPU backing deadline field+30 changes0→e0 through actual
669c0; its20 GPU-known initializer bytes remain unchanged. CPU lifetime
metadata is not uploaded as opaque GPU memory. All9 earlier snapshot
comparisons reject changed bytes before flattening this bounded profile.

Both existing live-owner clear segments remain structural:
`{4,97000,11d8,1224}` and `{1,50000,1328,1354}`. Their native bases,
page masks/counts and variable lengths must be regenerated from retained
owners. New source/header/blob/plan live under
`captures/m9128-device-inputs-20261006/normal-faa-preclear-draw-wait-*`.
The header selects M9_FAA_PRECLEAR_DRAW_CONTROL, MULTI_CLEAR and
PHASE_CONTROL/HAS_OUTPUT0. Firmware is separately named by the guarded
kernel profile. This is a diagnostic with no FAA pixel claim.

`tools/m9emu/check-faa-preclear-receipt-continuation.py` proves the receipt,
original-stack boundary and early clear return. The whole manifest checker
is `tools/m9emu/check-faa-preclear-draw-manifest.py`; generate through
`tools/m9-native-faa-phase-template.py`. The offline fixture
`tools/check-m9-faa-preclear-draw-prefix.py` checks128 placements using
actual vendor arithmetic, sparse unknown-byte preservation and ten
source/receipt refusals. Native C and hardware require independent proof.
The init-only, post-triangle and preclear-wait plans/blobs/headers regenerate
byte-exactly. The unchanged strict default full-output profile stays gated.

The next hardware qualification must retain real120 retirement, all four
common counter channels and the existing role-derived owner observations.
A full retained preprocessing-owner dump (logical owner23), plus sentinel
and pool, would permit a later source memory-input join if required. No
such bytes are presently supplied or claimed necessary by fa73a..fa865.
