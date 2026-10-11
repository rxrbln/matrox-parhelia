# Q3 CPU alpha-cutout selector audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This is a source-and-target-log audit of the narrow, opt-in
`PARHELIA_CPU_ALPHA_CUTOUT` selector in the current Mesa Parhelia driver.  It
does not modify Mesa or a target.  Its purpose is to distinguish a real
selector bug from an inference based on an older trace.

## What the existing Q3 trace proves

`/tmp/q3-alpha-fingerprint.log` on the P690 target records two alpha-tested
world draws:

| image | resource state | stable identity |
|---|---|---|
| 8 x 16 `PIPE_FORMAT_R8G8B8A8_UNORM` | one 2D sampler, linear min/mag, repeat S/T, `MODULATE`, alpha `GEQUAL 128/255`, depth-write `LEQUAL`, no blend, full RGBA mask | `0x16a69725e9886957` |
| 32 x 64 `PIPE_FORMAT_R8G8B8A8_UNORM` | same | `0xe936630e9679a61c` |

The target trace's raw `filter=1/1/0` must be decoded with Gallium's actual
enum order:

```
PIPE_TEX_MIPFILTER_NEAREST = 0
PIPE_TEX_MIPFILTER_LINEAR  = 1
PIPE_TEX_MIPFILTER_NONE    = 2
PIPE_TEX_FILTER_NEAREST    = 0
PIPE_TEX_FILTER_LINEAR     = 1
```

It therefore means **linear within a level, nearest between levels**.  It is
not the no-mip state.  The CPU selector's
`min_mip_filter == PIPE_TEX_MIPFILTER_NEAREST` requirement is correct for the
captured draws.  Changing it to `NONE` would reject both fingerprints and is
not a fix.

The trace also proves the resources are sampled from tiled layout at eventual
submission and that their hashes are the two hard-coded identities.  It does
not record all the inputs of `ph_use_cpu_alpha_cutout()` at primitive staging.

## Why the old trace cannot establish a selector rejection

The selector has two groups of checks:

1. state and resource preconditions before the hash: P690 PCI ID, bound
   sampler object, fragment `uses_color`, sampler mask and texenv, both mapped
   resources, stencil state, rasterizer state, and exact depth/stencil format;
2. tiled upload followed by one of the two hashes above.

The old alpha trace prints only the fragment sampler mask, texenv, sampler
numeric filters, blend state and resource formats.  Its `samplers=0x1` field
is `ctx->fs->sampler_mask`; it is **not** proof that
`ctx->num_samplers != 0` or `ctx->samplers[0] != NULL`.  It also omits
`ctx->fs->uses_color`, the two stencil enables, and the rasterizer's
`flatshade`/polygon-stipple flags.  Thus an absence of a CPU submission in
that trace cannot identify which pre-hash condition rejected the primitive.

Current source adds `parhelia: cpu-cutout stage hash=...` immediately after
all pre-hash conditions and tiled upload.  The target's isolated
`libgallium-26.2.2.so` has the same SHA-256 as the locally built library that
contains this diagnostic, but the target alpha trace was written before that
library was installed.  It contains no `cpu-cutout stage hash` line for that
reason.  There is no current-target evidence that either fingerprint fails the
current selector.

## Staging-flow review

`ph_stage_tri()` evaluates the selector before it chooses and copies the
vertex layout.  A selected primitive is isolated from ordinary batches,
written as ten floats per vertex (`XYZ`, post-draw `W`, RGBA, ST), and routed
to `ph_cpu_alpha_cutout()` before the normal packet state builder.  The same
selector calls `ph_resource_upload_tiled()` before hashing, while the CPU path
samples the tiled map.  Consequently there is no layout or hash-order mismatch
between selection and execution for these two resources.

The submission-side alpha trace is deliberately later than this decision; it
cannot retroactively prove a stage decision.  The CPU path must remain opt-in
and image-fingerprinted until an off/on target comparison validates its
rasterization, depth/stencil preservation, and presentation result.

## Minimal correct next action

There is no evidence-supported functional predicate relaxation.  The minimal
correct action is to run the current isolated library once with both
`PARHELIA_CPU_ALPHA_CUTOUT=1` and `PARHELIA_TRACE_ALPHA_TEST=1`, then inspect:

```
parhelia: cpu-cutout stage hash=0x16a69725e9886957
parhelia: cpu-cutout stage hash=0xe936630e9679a61c
parhelia: CPU alpha cutout triangles=... image=...
```

The first kind of line proves every pre-hash predicate passed; the second
proves selection reached the CPU executor.  If the first line is absent, add
a bounded one-line rejection bitmap to `ph_use_cpu_alpha_cutout()` before
loosening anything.  It must report the individual predicate fields rather
than overload the existing `samplers` trace label.  Only the observed failed
condition may then be corrected, and only if the Q3 state itself supports the
broader rule.

Do not change the mip-filter requirement, remove the image hashes, infer a
selection failure from the pre-install trace, or substitute the vendor alpha
context hypothesis for this CPU-selector diagnosis.  The vendor evidence in
`parhelia-p690-texture-alpha-context-re.md` remains relevant to a future
hardware route, but does not participate in this software fallback's staging
decision.

## Follow-up target result (2026-09-10)

The prescribed current-library run was completed on the P690 target.  It
identified two real selector differences from the earlier narrow model:

* Q3's color resource is `B8G8R8X8`, whose mapped BGR byte order is identical
  to the supported `B8G8R8A8` path; both are now admitted.
* The initial CPU executor called `ph_resource_upload_tiled(color)` before
  touching the mapped target.  That could copy a stale linear shadow over the
  preceding GPU scene.  The CPU branch now bypasses that upload and writes the
  coherent tiled P690 target directly.

With `PARHELIA_CPU_ALPHA_CUTOUT=1`, the target repeatedly logged only:

```
CPU alpha cutout triangles=32 image=0x16a69725e9886957
CPU alpha cutout triangles=12 image=0xe936630e9679a61c
```

The X11 frame capture after the preservation fix retained the surrounding GPU
scene and showed the cutout geometry, with no Parhelia DRM timeout, error, or
wedge in the target kernel log.  It is still an opt-in implementation: a
matched software or vendor reference image is required before promoting this
path beyond these two exact Q3 resources.
