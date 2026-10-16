<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Retired FAA primary bytes and readback comparison

This analyzes the retained physical `7dd03c5` private FAA attempt in
`tests/m9-faa-companion-20261006/proof`, boot
`6c79ac0e-786d-4dcb-822e-d34a4b13fb3f`. Root read the actual primary allocation
at `0x3a2f000`, `0x50000` bytes, after retirement and before any new allocation
or submission; its lossless archive and raw SHA256 were retained at `30840f49`.
The image oracle failed. This analysis performs no device access.

```sh
python3 tools/m9-faa-primary-readback-analysis.py
```

The script verifies raw buffer lengths and hashes against the retained
receipt, accepts the lossless `.gz` buffers, and reports the comparison as
JSON. An optional argument selects another retained proof directory with the
same filenames and raw receipts.
Plain and compressed inputs produced identical results; changing one primary
byte was rejected by the retained SHA256 check. The resolve source checker
below also passed. These checks replay saved evidence without device access.

The actual primary has **890 nonzero DWORDs**, led by `000000f0` (417),
`00000040` (40) and `41e041e0` (19). Interpreting each raw little-endian primary
DWORD as BGRA gives exactly the PPM's **nonblack RGB histogram**: 853 nonblack
pixels, including 417 blue-240 values and 40 blue-64 values, and zero full-green
pixels. The primary allocation has 5120 more DWORDs than the logical PPM;
the difference in black entries is exactly 5120. The packed-looking values
therefore exist in the actual primary allocation, rather than arising solely
from the CPU readback's RGB extraction. Histogram equality does **not** prove
the spatial pixel ordering or the correctness of a surface layout.

The fragment store has 1041 nonzero DWORDs, including `ff000000` (440) and
`0000ff00` (161). The companion has 1404 nonzero DWORDs. Of the primary's
890 nonzero occurrences, 582 have a value also present in the companion;
none have a value present in the fragment store. This is word-family overlap,
not proof of a shared owner, copy or address alias.

Exact byte comparisons find **zero matching aligned 32-byte records** when
all-zero records are excluded, and **zero matching aligned 128-byte records**
when each candidate must contain at least 16 nonzero bytes. These comparisons
contain 498 primary/249 companion candidates at 32 bytes and 40 primary/69
companion candidates at 128 bytes. They
are allocation-relative and do not establish an undocumented GPU record
layout. They provide no alias proof and cannot rule out arbitrary address
transforms or a hardware mapping error.

The retained actual vendor mode9 resolve audit separately identifies distinct
primary and companion resources: `B0=primary|1`, `B1=cc220140`,
`B8=companion|1`, `B9=7a220140`, `BC=main|1`, `BD=auxiliary|1`, `BA=HZ|1`.
The raw trigger is `45fcf3ec,0,0,00f00140`. Its kernel owner-relative fixups
select caller primary owner1 and companion owner4 separately. The existing
source checker reproduces these fields:

```sh
python3 tools/check-m9-faa-resolve-trigger.py \
  docs/traces/m9-vendor-faa-normal-frame-20261006.json
```

This does not prove hardware resolve semantics or a correct final image.
It also does not establish that the 53 observed relocation-helper fixups
cover every address-dependent word in the stream. The independent pending
clear-generator correction and its subsequent physical image qualification
remain outside this byte-comparison result.
