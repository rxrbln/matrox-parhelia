# M9128 miniport reset-operation admission

The separate miniport route to the `3c0/3c4` transaction is an explicit
0540 operation and a timeout recovery branch. It does not establish a missing
normal FAA initialization step. No native reset or register change follows
from this checkpoint.

All PCs name the MSerX `MxMM.sys` image, SHA256
`bf743961b4846e90f32cbdf868d69240d7c403fcbd0a8d9cd6d8783ef9bd62c6`.
The checker pins the complete relevant function spans and executes original
operation admission on synthetic CPU ABI owners. It stops before any reset
instruction or MMIO helper. No hardware register values, status replies,
return from the reset body, or real miniport construction are supplied.

## Explicit operation and family admission

Public miniport wrapper `11d52` loads the live instance through owner `+4`,
profile `+1ec`, then forwards the caller's operation and size to `2709c` at
`11d6a`. The dispatch decrements the actual operation before indexing the
16-entry table at `2725c`. Operation six selects `27119`, which checks the
instance's retained profile chip WORD `+14` against `0540`. Only equality
calls `3434b` at `27129`. There is no automatic operation-six call in that
wrapper or dispatch.

The checker executes both the dispatch and original wrapper for chip IDs
`0540`, `2539`, and `0525`. Only `0540` reaches the strict entry stop at
`3434b`. Other IDs return through the actual dispatch without touching any
hardware helper. This fixture proves the ABI and family gate, not that normal
device enable requests operation six. The examined device head-discovery
caller `5e855` explicitly requests operations four, nine, and ten at
`5e89c/5e8ca/5e8e9`; it supplies no operation-six request.

## Timeout caller

The second direct caller of `3434b` is `341bb`. After checking CPU reference
counts at the object returned by `3458d`, this helper derives the request by
clearing bit 28 or 29 of live `318`, according to its caller argument. It then
polls live `3b0` bit zero or one. Success takes `342af` and increments the CPU
reference count without entering `3434b`. Only after 2000 unsuccessful reads
does `34270` call the transaction; it subsequently reads and writes `3b0` at
`3427c/34297`. This is a source-defined recovery branch, not an unconditional
render enable. The previously trapped real cold prefix at `34248` is inside
this read-dependent request, before its success/timeout outcome is known.

## Exact transaction boundary

`3434b` saves live `8c8/8dc`, writes zero to those masks, and acknowledges
`8d8/8c4` with all bits set. It reads `3c0`, ORs `00110000`, and waits for
live `3c4` bit 31, with up to 1000 calls of delay helper `2a148(1000)`.
On a grant it performs the source `3c0` transitions: OR bit 31, delay 15000,
OR bit 21, OR bit 31, delay 15000, then AND `ffeeffff`. It restores the saved
masks only on that granted path. Exhausted grant polling jumps directly
from `343f9` to `344a2`, bypassing mask restoration. The callback body
`179a2` published at table `+88` has the same bounded grant/mask dependency,
but also has its own initial `3c0` bit-31 write. These bodies cannot be treated
as interchangeable literal write lists.

The failed-grant branch is a concrete reason to retain the complete request,
polling, and lifetime contract when investigating this route. Neither healthy
FAA retirement nor the current source prefix proves the required grant or
normal invocation. Calling this recovery operation speculatively would not
close the rendering prerequisite question.

## Check and remaining scope

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-miniport-recovery-admission.py
```

The open cold/global question remains the actual family-specific transition
that admits ordinary device use before the MD service graph. This checkpoint
narrows the candidate mechanism: the newly found direct transaction is
conditional recovery, while actual `HwInitialize` and the published callback
invocation remain unjoined to completed M9128 cold admission. It does not
attribute the observed zero FAA output to reset, PLL, or an unknown register.
