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

Measured first slice implemented in57a7867: exact0528/64x32 tile layout,
owned distinct render allocations, cpp4 formats32/36, equal pipeformat,
level0 single-sample positive unscaled/unflipped in-bounds copies.
Integrated fullbuild/selftests and four native1024x768 checks pass:
colour partial draw, sampling copied destination, depth/stencil copy,
and subsequent depth-tested draw, each786432pixels/zero mismatches.
Real BITBLT trace and source/destination rectangle retained in
`captures/gpu-gaps-20261004/native-copy-integrated-proof.log`.
Isolatedmain SHA4bd13acb5bdf05f55f1adf044bfe87516401380607a0678e4ef2969a33983149,
actualkernel0f7d564 unchanged. Other classes/chips/layouts and scaling
retain fallback. No performance gain is inferred from these pixel checks.
