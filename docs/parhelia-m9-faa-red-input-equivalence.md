<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Current ordinary red / full180 FAA source inputs

No causal missing shader state was found by this bounded comparison. The
passing ordinary SYSTEM red control and the current full180 FAA source plan
have identical complete CPU-known database, metadata, program, viewport,
vertices and constants. This does not prove that the GPU fetched them or
that FAA raster state, opaque pool data or phase ordering is equivalent.

`tools/check-m9-faa-red-input-equivalence.py` compares eight actual exported
spans without extending their known extents. The complete 240B red program
hash is `46e77ecc6215fd403a354d0c8a9ace2c16868e434d87f6fb02efae2a6614fe55`;
the 115160B database hash is
`af36245806515bd7b0e38a986586d33611a84ed76dae845a42ce8776ba89f174`.
These are the current red inputs, not the older green program. Both 1024B
texture-parent exports, metadata128/32, viewport136 and triangle/constants188
also match byte for byte. The checked report retains every hash and owner.

The entire 604B scratch image matches after replacing only actual typed
fixups and two independently recorded `2557b` completion stores. Fixups must
retain corresponding owner roles, source offsets, encoder PCs, tags and
types. Preprocessing selects program-relative offset32 / entry4; raster
selects offset0 / entry0. Both database bindings select offset0. The scratch
and raster masks/classes are respectively `0300bf00/fe201e92` and
`0000fff0/fe207ff8`; both raster D7 values are `20005ca3`.

The actual FAA stream contains an additional 60B counted interval
`[25c,298)` before the 692B triangle interval `[298,54c)`. It includes E6=3;
the current full plan preserves it. FAA raster F0/EF are
`c4008600/f8004055`, versus ordinary `c4008400/18000055`. This checker
records that difference and does not equate the two raster modes.

The distinct pool interval `[284,2c4)` is derived by source `24ffe`'s
literal A1 and `25039`'s 64B end increment. Source `fc99b` calls that
producer before `fc9c4/fca5b/fcae1` derive sentinel destinations8/c/10 for
the A1/A3/A4 stores. Nine shipping instruction pins retain those consumers.
The saved values are not established fragment counters or a decoded final
resolve filter. Current full180's zero pool/sentinel cells cannot alone
prove absence of fragments. See the separate pool-live-state and
draw-resolve-view audits for actual retained owner joins and the final
ordinary destination's absent sentinel range.

Reproduce on the host:

```sh
python3 tools/check-m9-faa-red-input-equivalence.py \
  --image /path/to/shipping/MxMD.dll \
  --out /tmp/faa-red-input-equivalence-check.json
```

The committed report is
`captures/m9128-device-inputs-20261006/faa-red-input-equivalence-check.json`.
Six negative controls reject changed program, metadata, geometry/constants,
scratch grammar, program owner and program offset, including mutations with
updated blob checksums. No command, address or GPU response is replayed.

For distinction, the earlier actual prefix run retained at
`/tmp/m9-faa-prefix-stage-20261007/proof` retired real140 and remained black.
Its log SHA256 is
`2021f281f8e8db9307b3bf97d72cceb1403800cbdc2aeaf89b9b2e6a532148db`;
its actual PPM SHA256 is
`12c810bd25efe1a7484387cd3d5a8503ce7cc341d61768b99a85c39a0ecca884`.
That historical prefix receipt is not a hardware qualification of the
current full180 source comparison. No new physical test was performed for
this audit. Opaque GPU pool acquisition, source phase boundaries and public
sampled-resource lifecycle remain independent investigations.
