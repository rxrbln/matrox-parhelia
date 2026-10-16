# Normal FAA source-data relocation audit

The concrete normal surface-owned frame's CPU-known source bytes remain
unchanged when actual resource addresses move. Command operands require their
typed relocation and direct-submit handling; the database and uploaded program
must not be patched merely because their packed words resemble addresses.

`tools/m9-vendor-faa-source-data.py` passively observes real `MxMD.dll` uploads
through the actual normal frame and stops before the retirement counter read.
It executes a second frame with one additional genuine1024x768 color resource
allocated before the normal primary. No allocation result, source shader,
GPU status or completion is supplied. The result is retained in
`docs/traces/m9-vendor-faa-source-data-20261006.json`.

The115160-byte RS database is exactly260 sequential copies from immutable PE
sections. Every copy comes from the real flattener's call at12097b (return PC
120980). Their source-byte hashes and concatenation match the complete known
database. All28790 observed destination writes come from actual memcpy PCs
1e5383/1e5478/1e5480. There is no runtime pointer injection in this concrete
database upload. Its SHA256 is
`af36245806515bd7b0e38a986586d33611a84ed76dae845a42ce8776ba89f174`.

The128-byte metadata table is exactly DLL payload1ef950..1ef990 twice. Real
1209ec..120a05 sets ESI=1ef950 and performs two16-DWORD REP MOVSD copies to
offsets0/40. All32 destination stores have PCs1209f6/120a05. Its packed values
are immutable table data, including values that numerically overlap DMA/GPU
owners. They are not relocation sites.

The32-byte metadata block is a real copy at120a69 (return PC120a6e) from the
constructed fragment-list object's+394 payload. Its four8-byte bundles are
`ffc0291d/e29fff1f`, `ffc1291e/a29fff1f`, and two zero pairs. The240-byte
program comes from two generated CPU instruction vectors copied at132b9e
(return PC132ba3),32 bytes at offset0 and208 at offset20. The source observer
retains each copy's actual CPU source and hash. These are generated data;
the immutable-database proof is not substituted for their compiler provenance.

The address perturbation preserves all eight known source exports exactly:

| Source | Baseline GPU | Perturbed GPU | Known bytes |
| --- | --- | --- | ---: |
| RS database |1152000|145a000|115160|
| Metadata table |1170000|1478000|128|
| Metadata bundles |1172000|147a000|32|
| Texture parent0 |1181000|1489000|1024|
| Texture parent1 |1191000|1499000|1024|
| Program |1b52000|1e5a000|240|
| Two sentinel initializers |1119000/11e0000|1421000/14e8000|20 each|

Identical sentinel contents do not identify their owner: the proof retains
both matching resources. Program SHA256 remains
`2efd8e0bb9c16ca8bdc67ddec0990bf995b7ef329d09ae8741b6a76324cb66ec`.
The same30 slots and source entries0/4 remain appropriate for this concrete
frame. The finite experiment establishes address independence of these
known exports under this allocation change, not a universal shader-variant
or hardware fetch-bound theorem. The checker compares the baseline hashes
to the committed `normal-faa-input-bytes.json` source manifest.

The passive pool observer also joins normal producer order. First fce50 sees
null cached sentinel/pool; the scratch launch precedes fcb60's first restore
from the genuine current sentinel, then the FAA triangle launches. During
resolve, old-owner fce50 sees current==cached and a nonnull genuine pool6;
24f20 emits its76-byte pool transition, then pointer stores save A1/A3/A4 to
sentinel slots8/c/10. The new shadow's cached sentinel is null when fcb60
restores that same current resource, before the mode9 output compiler f8e36.
Together with the existing92-operand manifest and pool-lifecycle capture,
this fixes the CPU-produced order and references. It does not prove the
contents the GPU acquires in pool6. No pool zero fill or earlier transition
may be inferred from absent CPU writes.

Reproduce the source audit with explicit `--runtime-tools` for the canonical
FAA helpers, `--bootstrap-tools` for its m9emu helpers, retained M9128 PCI/DC
input files, and `--out`. Validate without replay:

```sh
python3 tools/check-m9-faa-source-data.py \
  docs/traces/m9-vendor-faa-source-data-20261006.json \
  captures/m9128-device-inputs-20261006/normal-faa-input-bytes.json
```

Remaining first-image gates are the complete private owner/init/draw/resolve
execution route, exact source fetch bounds, opaque GPU pool-state acquisition,
actual retirement and a rendered FAA target image. This audit supplies no
hardware execution or completion.
