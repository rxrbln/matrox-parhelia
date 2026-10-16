# Returned public draw and pending output transition

The exact, physically qualified `preclear-draw-wait` prefix retires 0x120.
Its archived counter receipts are +00=0x121/0 and +20/+40/+60=0x10/0.
The new continuation restores the original 252b9 caller stack and answers
only its actual +00 read requesting 0x110. The public DP2 request returns
EAX=1 with HRESULT=0, emitting its second genuine 17e480 launch. This closes
the returned public black-clear/triangle CPU request; it is not a pixel proof.

`m9_faa_preclear_draw_receipt_continuation.py` guards all mapped MDL guest
reads against actual prior/current guest CPU-written byte sets. Its aperture
read wrapper follows the selected CPUroot child/PTE to a unique retained local
owner and invalidates initial CPU knownness for GPU-owned bytes. Such a read
requires an exact raw receipt, or a new guest CPU write. Five real archived
raw roles are available and checked by full extent/SHA. This continuation
uses none of them: no opaque BAR/GPU buffer read occurred. Initializers never
serve as implied post-GPU replies; supplied receipt bytes never become
CPU-known upload spans. Unknown status/OS mapping reads retain the established
strict host boundaries.

The same retained graph then receives the explicit internal `ac1f0` output
request, with the original helper's actual graph triple and 320×240 rectangle.
It allocates a real kind1/logical-format6/hardware-format0x64 SYSTEM temporary
and enters `abc20`. It stops before another 252b9 +00 read requesting 0x150.
The existing 0x120 receipt cannot answer it. This is an actual format-transition
ordering boundary, before the ordinary-output resolve returns. Public Lock
admission remains separate; no source field, return, completion or pixels are
fabricated to reach this internal caller.

The distinct `preclear-output-wait` profile independently calls the real
23b80 producer at that retained boundary, stopping before its 23bc3 GPU read:

| Field | Source-produced value |
| --- | --- |
| Primary source bytes | 5224 |
| Independent final request | 0x160 |
| Original pending request | 0x150 |
| Retained owners | 31 |
| Typed relocation calls | 76 |
| Non-null/direct native fixups | 46 |
| Timeline fixups | 24 |
| Direct counted-secondary endpoints | 6 |
| Pending SYSTEM output | owner27, 80 pages, flags0x30/PTE low0xb |
| Primary / secondary | owners28 / 30 |

Those owner indices identify this source profile, not a hardware allocation.
Native pointers, PFNs, descriptors, page runs and timelines derive from live
retained owners. The output's actual scatter producer gives metadata
[0,80,0] with all bytes source-known; its 327680 pixel bytes remain unknown.
The passive scatter observer's retained row list continues observing the same
source call stack; publishing that list changes no vendor memory.

The earlier primary [0,5068), all earlier secondary/constants/geometry and
all eight LOCAL known input images are checked unchanged before flattening.
The original draw appends primary [5068,5080), which exports the new counted
secondary [664,1356). Early output state adds primary [5080,5212). The real
final request appends **inline primary** E3/selector1/token0x161 at
[5212,5224). This differs from the earlier prefix's counted-secondary footer;
no footer is reused. Both original variable page-clear segments remain
source-proven: companion151 pages and sampled-primary80 pages.

The header declares `M9_FAA_PRECLEAR_OUTPUT_CONTROL`, `M9_FAA_PHASE_CONTROL`,
`M9_FAA_HAS_OUTPUT=1`, and `M9_FAA_OUTPUT_PENDING=1`. Hardware qualification
must use `--phase-control`, retain the output allocation, and refuse image
reads until the original post-0x150 output stack establishes completion.
An allocated output owner alone does not qualify rendered pixels.

Reproduce from this repository root using
`tests/m9-faa-preclear-output-source-20261007/capture.sh`. The established
interpreter is `/home/ai/tmp/faaemu/venv/bin/python`; no installation is needed.
Run `tools/m9-native-faa-phase-template.py` with the seven artifact paths
recorded in the test script's companion `check.sh`. The source checker pins
the old entire prefix/receipt, original return, unknown-memory guards,
new output's live MDL/PFN/PTE/metadata, actual callbacks, six direct endpoints,
24 source timeline stores, and independent finish. The host proof runs 128
placements/alignment pairs against actual vendor page-clear instructions,
checks sparse unknown preservation and unchanged patched suffixes, and rejects
seven source/owner/pending-output corruptions. All four previous phase
plans/blobs/headers regenerate byte-exact. Native C and hardware proofs remain
separate; this profile has not yet been physically qualified.
