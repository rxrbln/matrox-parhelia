# Public resolve pool snapshot chronology

The qualified public mode9 prefix retires1d0; the separate whole returned
aa750 prefix retires1f0. Their archived1408-byte pool records have different
FNVs (ad9f2d65/f97d2d71), but the ten selected source-qualified saved cells
below are identical. This is a snapshot comparison, not decoded fragment data.

| Source index | Pool offset | Both observed values |
| --- | --- | --- |
|A1|284|24a|
|A3|28c|1b|
|A4|290|80000488|
|A8|2a0|1f|
|A9|2a4|08ffff00|
|AA|2a8|0|
|AB|2ac|0|
|AC|2b0|0|
|AD|2b4|9b04|
|AF|2bc|0|

The actual24f20 preparation emits literalA1 at24ffe, derives its64-byte
interval at25039, and joins typed pool+284/+2c4 range operands. Each profile
contains the same two76-byte preparation images. This bounds the selected
register correspondence; it is not a generic ABI for the remaining pool.
Actual fbe8c reads AAshadow+14, fbedf reads+20 and fbee8 reads+24.
The public mode9 continuation emits1f/0/0, matching the saved A8/AB/AC cells.
The earlier qualified in-place copy instead saved1d/ffff/0, matching its
different actual abc20 policy. These observations distinguish the public
policy from the explicit rectangle path; they do not interpret those bits as
a pixel mask or authorize replacing them.

The returned cleanup emits A8=0 and AB/AC=ffffffff, then the actual fab30
reset packet. Crucially, that append contains no new kind2b pool binding,
24f20 preparation or fcb2f40008000 save control. It leaves the earlier two
preparation images exact. The final pool therefore retains an earlier saved
state image: its A8=1f/AB0 and A1=24a are **not live post-reset readback**.
Their unchanged values cannot establish that clear/reset failed or that no
new command executed. This agrees with the unchanged selected cells while
the source emits a different clear policy after the snapshot.

The65 other changed DWORD offsets are retained as uninterpreted differences.
Sparse raw bytes cannot attribute their changes to GPU stores rather than
unknown backing contents across different boots. F0's saved valuecc010001
does not equal the source-emitted e0000400, and no conversion contract is
proved here; it is excluded from the literal-state comparison. Main, AA and
sampled-primary raw records remain opaque and are not inputs to this checker.

The checker verifies the archived compressed pool SHA, uncompressed SHA/FNV/
counts against each strict retained report, source plan/log hashes, actual
vendor instruction pins and chronological source writes. Reproduce:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-faa-public-pool-snapshots.py \
 --archive-root /home/ai/src/parhelia/xf86-video-parhelia \
 --out /tmp/m9-public-pool-snapshots-20261007.json
```

The exact source/plan/log/raw hashes are in
[the bounded proof](../tests/m9-faa-public-pool-snapshots-20261007/proof.json).
Hardware archives are the existing
tests/m9-faa-public-resolve-hardware-20261007 and
tests/m9-faa-public-returned-hardware-20261007; they are not copied or replayed.
No GPU input, status reply, register mutation or rendered-pixel claim follows.
