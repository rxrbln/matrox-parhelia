# Two vertex fetches: sampler identity and result banks

The installed glmark2 terrain shader has two direct-UV vertex fetches and
five consumed outputs. Mesa currently refuses its second fetch before native
compilation. The earlier original-vendor two-fetch controls establish public
admission and multiple launch shapes, but their summed output cannot distinguish
the two fetched values. This checkpoint closes that specific source ambiguity.

The new caller creates two live profile-12 textures, publishes their handles,
and uses genuine public CREATEVS/SETVS/DRAW with explicit LOD zero. Their sizes
are 64x64 and 128x64. Three one-output controls differ only in the actual public
combining operation. A fourth consumes all five outputs. All texture contents
and GPU-written results remain unknown; no GPU completion or sampled value is
supplied.

| Public expression | Descriptor sampler order | Actual native source pairs |
|---|---|---|
| sample0 + sample1 | 1, 0 | r06+r02; r04+r00 |
| sample0 - sample1 | 1, 0 | r06-r02; r04-r00 |
| sample1 - sample0 | 0, 1 | r06-r02; r04-r00 |
| Five outputs, sample0 - sample1 | 0, 1 | r00-r04; r02-r06 |

The SUB and reverse-SUB native programs are byte-identical. Their logical
sampler descriptors change order. The result bank therefore belongs to the
sequential hardware unit: unit 0 supplies r00/r02, unit 1 supplies r04/r06 in
these original compiler controls. Logical sampler enumeration is independent.
The subtraction interpretation uses the existing measured native `a-c` form
and its `0xc0` negate bits, also used by Mesa `emit_add`; this checkpoint does
not emulate GPU arithmetic or claim a sampled pixel result.

The passive constructor observer joins each descriptor pointer to its exact
compiled record and sequential unit. Original `187860` stores EDX into the
working object at +0 and backend at +0xc (`18787e`, `187886`), then selects the
sampler row from descriptor +8 low five bits (`18789e`..`1878b2`). The actual
`18b4e0` lowerer joins that sampler to its published resource wrapper and live
GPU address. The checker requires equal constructor/lowerer/descriptor counts,
unique code owners and per-row pointer/resource/unit joins. It does not infer
result registers from the descriptor's address-looking first DWORD.

The one-output controls retain 2/28 compiled slots and two launches. The
five-output subtraction control retains 19/4/8/8/26 slots and five launches;
only the third variant samples. It first preserves all four fetched pair
registers and then subtracts the live pairs. Its generic input exporter still
refuses the source-relative command-patch branch `248f2`, as the earlier
five-output control did. The separate original relative-patch chronology is
documented in `parhelia-m9-vertex-coordinate-producer.md`; this checkpoint
does not silently substitute final address-only metadata for that chronology.

The four whole source reports are retained under
`captures/m9x-vertex-texture-routing-20261007`. The actual MxMD SHA256 is
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
Reports include exact public request bytes, original instructions, actual
native code owners, texture constructor/lowerer snapshots, module source
hashes and PCI/resource/DC input hashes. The prior ADD native code is unchanged
byte for byte. The strict checker rejects twelve mutations, including missing
and extra constructor rows, wrong unit/descriptor/resource, false known texels,
wrong ALU source and a false GPU result.

```sh
<faaemu-python> tools/check-m9-vertex-texture-routing.py \
  captures/m9x-vertex-texture-routing-20261007
```

To reproduce each source report, run
`tools/m9-vendor-vertex-texture-routing.py` with the same pinned bootstrap,
runtime and actual PCI/resource/DC input paths as the existing public texture
caller, using `--varyings 1 --combine add`, `sub`, and `reverse-sub`, then
`--varyings 5 --combine sub`. No target operations are performed.

The next native slice can preserve sampled SSA definitions from the bank
assigned to each live hardware unit, rather than assuming a logical sampler
number defines that bank. It must also retain both sampler/resource owners in
the job key and pending-job lifetime. Terrain additionally needs its five
outputs carried across stage changes; the current one-fetch fused-output gate
must remain until that routing is implemented and qualified. Neither async
ownership, implicit-LOD header policy nor a second-fetch hardware image is
proved by these explicit-LOD source controls. No Mesa cap or admission change
is included here.
