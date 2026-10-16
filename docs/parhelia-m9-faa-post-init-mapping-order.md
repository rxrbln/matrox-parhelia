# Retained graph first-draw mapping order

The sealed first-draw source contains one six-write GPU mapping activation
before program access: `31c=f,0,1,5,0,2`, then the actual `314=1` access kick,
CPU-root mirror `31c=f,0,c`, and release `31c=0`. These are
`draw.json:mmio_events[0..10]`. Program stores remain after the CPU mirror
and before release and first END, as the signed first-draw checker proves.

Constants owner28 and secondary owner29 are new **consumed plan entries**;
their OS allocations and GPU bindings already exist in the retained source
INIT graph. Each uniquely joins the whole initial manifest's flags30,
64-page mapping, identical live MDL, CPU backing and PFN prefix. No source
allocation or GPU re-publication of these two ranges occurs after the INIT
receipt. The source plan excludes them before first use because their bytes
were not then consumed by the submitted INIT command stream.

The inspected native late-owner preparation allocates both anew and calls
`m9x_address_map_faa_system` for each, followed by local program preparation.
With the manager already active, each new SYSTEM bind emits six31c writes
and a314 kick; the program local publication emits another six31c writes.
This makes18pre-access31c writes and twoextra314 kicks before the genuine
program access kick. It differs from source ordering. The native new ranges
are reserved independently; `publish_binding` changes their compact and
secondary records only, and skips already published bindings. It does not
rewrite existing CPU-root leaves. No wedge or image cause is established by
this difference.

The closest source pool lifetime is to allocate/map/retain these two unused
buffers during INIT, and upload their source-known spans only after the INIT
receipt. Introducing them earlier requires a separately checked native INIT
owner inventory. Alternatively a bounded publication helper could prevalidate
all new compact and secondary ranges, stage compact leaves, activate once,
copy all secondary records under one5pre-list and finish once with0/2; it must
preserve every earlier mapping and the later single program314 kick. The
existing per-resource mapper cannot simply be called in a loop and described
as that batching operation.

Run `python3 tests/m9-faa-graph-post-init-20261007/check-mapping-order.py`.
It validates the captured source ownership/order and rejects extra kicks,
missing initial MDLs, changed flags and changed PFNs. This is source evidence
and host review, with no new GPU receipt or hardware result.
