<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Retained FAA main and companion outputs

The retired BAR archives in `tests/m9-faa-companion-20261006/proof` prove
that the main and companion allocations acquired nonzero bytes. They do
not contain a vendor GPU reference image or a source decoder for those
bytes. In particular, the occurrence of `ff000000` and `0000ff00` cannot
alone prove alpha and green color fields or successful fragment shading.

`tools/m9-faa-store-record-audit.py` requires the saved raw SHA256/extent
receipts, then joins the main logical extent to the genuine output-flush
manifest's unique format8/hardware66, 1024-by-310 owner. Its logical extent
is `136080`; the native page-rounded owner capacity is a different quantity.
The existing source bindings select this owner at `BC`, its auxiliary owner
at `BD`, and the distinct format5/hardware3d companion at `B8/B9`.

The main has 1041 nonzero DWORDs. Grouping allocation-relative bytes at a
32-byte analysis stride gives 440 nonzero groups and eleven distinct byte
patterns. Every group has exactly one `ff000000`, in DWORD5 or DWORD6.
DWORD0 through DWORD4 are zero in every group. The numbers of nonzero
DWORDs by group position are `0,0,0,0,0,161,440,440`. The first group starts
at zero and the last at `4920`; the last nonzero DWORD is at `493c`.
These facts do not establish that 32 bytes is a hardware record boundary,
nor that the remaining fields are coverage masks, pointers or colors.

The companion archive has 1404 nonzero DWORDs over its retained `98000`
allocation. Its source logical extent is `97000`, so the full allocation
must not be treated as initialized logical data. The existing primary
readback analysis shows common word values between the companion and the
failed sampled-primary output, but no exact matching nonzero aligned
32-byte groups. This is not an owner alias or a decoded copy contract.

The earlier FAA note incorrectly assigned the name `Pnx::RsFragPtrsBuffer`
to the main output based on a Linux assertion string. The exact
[selector6c constructor proof](parhelia-m9-selector6c-constructor-static-closeout.md)
instead joins that name to a separate cached shader-state helper pool:
Linux device factory `5b2690`, concrete pool constructor `37d130`, initializer
`37cee0`, and publisher `44b550`. Its initial resource is type31,
width80, height1, 32 stored bits, hence `200` bytes. The factory vtable,
allocation arguments, layout computation and selector6c publisher are
independently pinned by `m9-selector6c-constructor-evidence.py`. That source
name cannot supply the missing main-output record schema.

The recovered CPU producers establish distinct live resource bindings,
actual raster/compiler words and program bytes. The resolve audit also
establishes the ordinary output's `c8210140` destination descriptor and
the source `7a220140` companion descriptor. They do not decode these
GPU-produced main/companion bytes or prove that the diagnostic inherited
GPU context equals the vendor initialization. No additional downstream
register value is justified by this archive inspection. The independent,
default-off source-initialization experiment addresses that context
difference; its hardware result remains a separate gate.

Reproduce without device access:

```sh
python3 tools/m9-faa-store-record-audit.py \
  --proof /path/to/tests/m9-faa-companion-20261006/proof \
  --manifest /path/to/normal-faa-output-flush-input-manifest-101f8.json
```

Validation checked all saved receipt hashes and source extent joins, and
recomputed the 440-group pattern. Mutating a saved raw main byte is rejected
by the existing receipt checker. A mismatched source extent is rejected
before group interpretation. The pinned Linux selector6c source checks also
passed against the retained vendor image. No GPU input or completion was
supplied by this analysis.
