# Native copy performance gap

2026-10-05, compact Mesa d883e77 (deployed5461138).

Inspection of `ph_blit`, `ph_copy_texture_box`, and
`ph_resource_resolve_linear` distinguishes functional API coverage from
hardware acceleration. Equal-size colour copies download transfer shadows,
wait for pending async rendering, allocate CPU staging storage, copy each
texel and upload the destination. Depth and conversion blits use
`ph_blit_cpu`. Imported-resource type2-to-type1 resolves already submit
GPU BITBLT via `parhelia_batch_blit_format`.

The GL2 profile measured the screen-depth CopyTextureSubImage stack at
about30% of samples. App e7321f0 avoids that copy when neither SSAO nor
per-view sunlight consumes it; requested effects still require working
depth transfers. This app optimisation does not close the driver gap.

A native copy implementation must distinguish render-target surface
layout from texture-upload layout. A resource handle alone is not proof
that the BO can be addressed with BITBLT surface type2. The first coherent
slice is equal-format, unflipped, unscaled, level0 render-resource copying
with actual surface-format/type and pitch compatibility, owned BO bounds,
ordered fences and explicit destination shadow invalidation. Keep existing
CPU paths for other representations and overlapping/self-copy cases.

Before enabling it, prove the real native packet and pixels for colour
and depth separately; do not infer depth-tile compatibility from colour
resolve success. Tests must include rendering into the destination after
the copy and reading/sampling it, since stale CPU shadows can overwrite a
correct GPU result. Scaling, format conversion and mip/layer copies remain
separate hardware work. Prefer native copies over further optimisation of
CPU texel loops.
