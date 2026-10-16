# FAA allocation dimensions

Original M9128 allocation producers now close the sampled-primary and FAA
graph extents at 1024×768. The CPU capture executes `a3870` for sampled color,
then `ac7e0/45eb0` for the graph, and stops before `b4fe0` clears HZ. It supplies
no GPU completion, physical backing or pixel input. Each shape uses its own
actual allocator graph; this does not establish command-prefix equivalence
with the qualified 320×240 graph.

The artifact is `captures/m9-faa-dimensions-20261008/allocation.json`, SHA256
`c9980292358f31c18c97cba44a19f31a8baac93a11c8e753d3a139bfbd9a03de`.
It retains eight shapes: 1×1, 64×32, 64×64, 65×65, 320×240, 321×241,
1024×768 and 2048×2048. Producer self-hash and the original DLL hash are
retained in the artifact. The unchanged vendor image hash is
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

| 1024×768 owner | Raw layout bytes | Resource extent | Source backing bytes |
| --- | ---: | ---: | ---: |
| Sampled primary | rounded by color layout | `0x301000` | `0x308000` |
| Main fragment store | `0xc6a020` | `0xc6a080` | `0xc6a080` |
| Auxiliary store | `0xc6020` | `0xc6080` | `0xc6080` |
| Companion | `0x600020` | `0x601000` | `0x608000` |
| FAA HZ | `0x19000` | `0x19000` | `0x20000` |

The source backing extent is distinct from GEM page rounding. In particular,
the main and auxiliary source extents subsequently need full pages in a
native BO. Backing padding is neither part of a clear range nor known initial
data.

`46197` selects backing alignment from the actual layout table; `546ea`
copies it into the allocation descriptor; `a17d1` rounds the requested
extent; `665de` publishes the resulting capacity. The retained resource,
layout and allocator-descriptor pointers join every row before comparing
these values. Sampled color uses 32 KiB backing alignment. Companion clear
alignment is 1 KiB in its width≤64,height≤32 family and 4 KiB otherwise;
FAA HZ clear alignment is 1 KiB. Both use 32 KiB backing alignment.

FAA HZ geometry follows the original flag-`0x40` category-3 branch:
`ceil(W/8) × ceil(H/4)` visible metadata becomes
`align(ceil(W/8),8) × align(ceil(H/4)+8,8)` allocation, with four bytes per
element and 128-byte raw rounding. See the instruction closure in
`parhelia-m9-hierarchical-z-static-closure.md`. At 1024×768 the eight extra
metadata rows produce `0x19000`, whereas the ordinary draw HZ helper produces
`0x18000`. FAA therefore needs a separate layout helper; the ordinary draw
policy remains unchanged.

Run from an RE checkout:

```
/home/ai/tmp/faaemu/venv/bin/python tests/m9-faa-dimensions-source-20261008/check.py
```

The checker pins the artifact, producer, PCI input and actual instruction
bytes, validates all eight allocation joins and rejects nine altered proofs.
The Mesa graph test compares its actual primary/companion/HZ and store
helpers with these independently captured values, including both small-size
category boundaries and refusal without output publication.

## Remaining native joins

Allocation parity is a prerequisite for 1024 testing; it does not enable a
1024 FAA draw. The remaining work must move together:

* Mesa's sampled-primary BO request must use the separately derived backing
  capacity. Its current request is the logical extent. PREPARE/INIT must then
  validate live six-owner capacities and descriptors, replacing the current
  320×240 admission and private INIT owner specifications.
* INIT and FIRST clear producers must accept the live clear extents and
  variable page-run lengths. At 1024 the primary resource spans 769 pages and
  companion clear spans 1537 pages, versus 80 and 151 at 320. Fixed scratch
  capacities, post endpoints and secondary relative links must be computed
  from emitted lengths. No unknown backing padding may be cleared.
* Bound-depth profiles must carry actual visible, allocated and storage
  dimensions. Existing exact 320×240 profile comparisons, attachment clear
  capacities and FIRST/later profile length checks still refuse larger
  surfaces.
* Raw color headers, viewport/clip constants and raster extents must derive
  from the same live dimensions. Existing 320×240 frame and shader-upload
  guards must remain until that packet closure is verified.
* Normal Lock, F021 view and surface-access windows need the source-derived
  1024 descriptor, pitch, extent and aperture offset policy. Current native
  Lock/readback and output-offset functions are bounded to 320×240. GPU
  presentation work must retain dimensions as typed inputs across Unlock.

No GPU validation or pixel result is claimed for dimensions beyond the
existing qualified 320×240 path.

The original 1024×768 INIT-only capture is retained in
`captures/m9-faa-dimensions-20261008/init-1024`, with compressed unchanged
source/manifest/address/startup reports, the derived plan/blob, and the exact
historical helper sources named by the startup receipt. Its caller stops at
the INIT-only CPU boundary, then invokes the existing genuine independent
queue finish; it supplies no GPU execution or completion.
`tests/m9-faa-dimensions-source-20261008/check-init.py` verifies those immutable
pins and compares against the qualified 320×240 INIT plan/blob.

All seven non-primary known spans are byte-identical. After typed address
and timeline normalization and deletion of the companion page-run, exactly
five primary words change: HZ allocation height/storage width at `0x454`,
`0x1170`, `0x11b0`, and HZ reader descriptors at `0x474`, `0x1188`.
`0x00480028` becomes `0x00c80080`; `0xf0000028` becomes `0xf0000080`.
The companion clear grows from 19 to 193 source page words, moving the full
primary from 4880 to 5576 bytes. The four typed address sites and all thirteen
timeline sites retain their producer/owner/epoch semantics; later sites move
by 696 bytes. The final source point remains `0xb0`.
Private source owner6 follows the primary backing capacity `0x308000`, and
owner7 follows HZ backing capacity `0x20000`; other private allocation extents
are unchanged. This closes INIT command construction for these two dimensions,
with live owner relocation and live page alignment. It does not qualify larger
FRAME clears, a normal Lock window, presentation, or physical 1024 rendering.

The original allocation and selector-1 CPU control is retained in
`captures/m9-faa-dimensions-20261008/selectors.json` and checked by
`tests/m9-faa-dimensions-source-20261008/check-selectors.py`. It calls the
original selector producer on genuinely allocated layouts and stops before
HZ clear, without a GPU reply. At 1024x768 the primary selector is F0220400 /
00010300: the readable formatted window is 0x300000 bytes, the logical clear
extent is 0x301000, and the backing capacity is 0x308000. The seven spare
backing pages do not extend the formatted view height. The independent
allocation-height/width producer is pinned for all five resource layouts;
this control does not claim a returned 1024x768 normal Lock or pixel proof.
