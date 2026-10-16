# M9128 FAA mapping operations between retained phases

A passive observer repeats the unchanged original180 source producer and its
existing receipt-qualified continuations. All nine emitted draw/output/write
manifests, source captures and address audits remain byte-identical to the
retained originals. Original receipt/unknown-read guards are unchanged; no new
counter reply, GPU record or pixel input is supplied. Generated files are
shadowed under the same logical paths and saved separately, never overwritten
in the retained source tree.

Two distinct operations occur around the first preprocessing launch:

* Before the first original program-access wait, its newly allocated local C2
  resource has already published two compact leaves and two12-known-byte C24
  records. Actual5b750 receives `[0]*8+[GPU>>12,2,0]`; PCs5bad1/5bae6/5baed
  write each local PTE/0/0. No fourth DWORD is initialized by this local branch.
* After resuming the original program-access wait,2cb40/59410 copies those two
  existing leaves from the GPU compact child into a different CPU aperture
  child. Actual594a2 writes both copies. The source child physical base is
 401000 and CPU child915000 in this101f8 profile. The destination indices are
  CPU view aliases, distinct from the GPU page indices. The prior destination
  words differ; the GPU source leaves are preserved. The314 kick and packed
  flush tokens0/30000c8f precede CPU access;2c840 releases that CPU view and
  writes31c0. This is not a newly installed GPU-local mapping.
* After the120 preprocessing prefix, genuineac1f0 allocates the new ordinary
  kind6 SYSTEM output. Its actual5b750 descriptor has flags30 and80 owned PFNs.
  Each of its80 C24 records is written by5ba25/27/2d/33 as packedPTEb/0/80/0,
  matching its actual CPU-known compact PTEs. The actual publication tokens are
 0/3000018f/10000005/20000028. This mapping did not exist in the earlier prefix.

Native C2 input upload uses the retained physical BAR owner rather than the
vendor's temporary CPU aperture alias. Its GPU identity leaves and known bytes
are separately checked. Therefore the missing CPU alias itself is not a
missing GPU shader translation. This does not establish equivalence of the
source314/31c timing or hardware effects.

The default native path reserves and publishes the later SYSTEM output with every
other owner before initial GPU execution. Its final B leaves and four-word
records match the actual per-owner grammar, but the source publishes that
output after preprocessing. Matching contents do not prove that moving these
publication operations before GPU execution is equivalent:31c's temporal
hardware effects remain unqualified.

A narrow diagnostic in kernel d76d07d tests later mapping publication by reserving
only the later output node early, leaving its binding unstaged across manager
initialization, then staging/committing that live flags30 binding after genuine
120 retirement and before the160 endpoint. Existing validated compact/C24
publication must derive words from the live node/DMA owner. All other mappings
remain unchanged; failure after an earlier post must retain the ordinary
quarantine path. This is a source-shaped experiment, not a replay of captured
register values. Its
[fresh-boot hardware run](../tests/m9-faa-late-output-hardware-20261007/README.md)
confirms the120 publication point but still produces black output, with
unchanged main/companion hashes compared with early publication. This timing
change alone is insufficient. CPU DMA allocation remains early.

The original program-access counter deadline is0 and original draw-format
counter deadline110; c0/120 are the independently retained prefix endpoints.
The stronger native phase waits must not be described as identical to those
original CPU waits. No hardware equivalence or rendered pixels is established
by these CPU observations.

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-faa-phase-map-observe.py \
  --source-tree /path/to/original/source/tree \
  --out-dir /tmp/m9-faa-phase-mapping
python3 tools/check-m9-faa-phase-mappings.py \
  --observation tests/m9-faa-phase-mapping-20261007/mapping-observations.json.gz \
  --captures captures/m9128-device-inputs-20261006
```

The source tree must retain the original helper path identities and hashes;
those strict producer checks are not relaxed by this observer. The checker
joins source output ownership, compact leaves, actual C24 writes and all nine
reproduced hashes. The complete compressed observation also retains earlier
startup mapping rows and actual instruction spans.
