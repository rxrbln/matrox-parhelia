# Comparing retained FAA phase stores

`tools/check-m9-faa-retained-stores.py` reads local archived observations and
optional raw dumps. It accepts one complete fresh qualification log and its
whole source plan. It joins each before/retired owner index to the generated
allocation extent, captured identity and BAR/DMA domain; all expected owned
buffers must be present. It requires a positive actual private retirement
point and the output-owner presence/absence appropriate to that profile.
Native owner addresses remain recorded observations, never replay parameters.

The main and companion roles follow actual source graph labels. Pool22 is the
previously proved context factory/publisher role, additionally checked against
the actual typed-kind2b fixup in every profile containing a draw. Init-only has
no such operand and preserves the retained source pool owner. Main/AA/pool
observed extents are respectively `136080`, `98000`, and `580` bytes.
The report also includes AUX, both HZ owners, both sentinels and preprocessing
owner23. This can locate a new observed difference without assuming the data's
record layout or classifying word patterns as colors.

For a new phase, run:

```sh
python3 tools/check-m9-faa-retained-stores.py \
  --plan captures/m9128-device-inputs-20261006/normal-faa-init-only-native-plan.json \
  --dmesg /path/to/fresh-phase/proof/dmesg-after.txt \
  --out /tmp/faa-init-stores.json
```

Use the post-triangle plan for that distinct fresh test and add
`--reference /tmp/faa-init-stores.json`. The report compares retired FNV/counts
by role and exact extent, not by equality of native addresses across boots.
Hash equality is explicitly weaker than byte equality. A zero observed
nonzero-byte count establishes that the observed domain was all zero; it does
not establish DRAM coherence independently of that read domain or add CPU
initialization to source-unknown owners.

If a phase hash changes, retain retired raw main/AA/pool dumps covering the
full observed extents from their actual retained owned allocations. If its
before observation is already nonzero, retain the matching before dumps too
for exact changed-byte comparisons. Keep the boot/module/plan, owner identity,
retirement and dump provenance together. Additional HZ/sentinel/preprocessing
dumps are useful when their own observations differ; they are not substitutes
for the three primary stores.

Supply a same-test dump with `--raw main=/path/to/fragment-store.bin.gz`,
`--raw aa_companion=/path/to/aa-companion.bin.gz`, or
`--raw pool=/path/to/pool.bin`. Every supplied dump must match that test's
observed size, full FNV, nonzero-byte count and nonzero-DWORD count. The report
retains SHA256 and allocation-relative nonzero ranges. When both reports have
raw dumps, it additionally compares the complete bytes. No pixel decoder,
fragment ABI, GPU counter value or source-unknown memory is manufactured.

Validation used existing normal source-init and constant-green hardware logs:
main remained `1041` nonzero DWORDs / `02b5ad25`, AA `1404` / `a86ed979`, while
pool counts changed from `202` to `200`. Three older same-test raw archives
(main/AA/pool) matched their own observations; changing one raw byte was
rejected. The earlier partial BAR-only observer log was rejected because it
lacks complete SYSTEM-owner observations. This framework has not supplied or
collected a new physical observation; the target owner performs qualification.
