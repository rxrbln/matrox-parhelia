# M9 fixed-function transform and lighting live closure

The saved Linux M9 ICD now has visible, context-isolated result evidence for
the fixed-function transform and lighting states needed by an OpenGL 2.0
compatibility implementation.  Evidence lives in
`captures/m9x-linux-ff-tnl-20260914`; verify it with:

```sh
python3 tools/m9-ff-tnl-evidence.py --self-test
```

## Experiment design

Each case is one externally bounded process.  It creates two direct contexts
with independent texture objects, renders the test as B1, switches to a
matched control context A, then switches back and renders B2.  The fixture
reads the complete 96 by 96 RGBA result plus five fixed probe pixels.  This
tests the visible operation and context state restoration together.

Every B1 hash equals B2 exactly.  Every B hash differs from its A control,
including at least one retained probe pixel.  All 57 draws and reads report
`GL_NO_ERROR`; every B1 reports direct rendering and
`Matrox ICD for M-Series`.

The inputs deliberately avoid symmetry: matrix scales and rotations differ
per axis, light and material channels are unrelated, positions and normals
have unrelated components, texture planes contain four distinct values, and
the texture has different data for both coordinates and every channel.

## Visible results

| case | state isolated by B/A | B1 = B2 hash | A-control hash |
|---:|---|---:|---:|
| 0 | directional light versus unlit | `4341b47e5a7d641c` | `a9e3b10fef47723e` |
| 1 | positional light plus C/L/Q attenuation versus directional | `866b961a69323e4e` | `4341b47e5a7d641c` |
| 2 | spot cutoff/exponent/direction versus positional light | `c40b337fc7a2d38b` | `3e55b3321511fe80` |
| 3 | independent front diffuse material | `68d51a45f3c8e9fb` | `4341b47e5a7d641c` |
| 4 | local viewer | `1141fb77b912c9cd` | `3e55b3321511fe80` |
| 5 | normalize after a nonuniform model-view scale | `9a4b54542f381500` | `46294366c7b3d0ad` |
| 6 | rescale normal after a uniform scale | `0f6e58523a139a77` | `99946f22a17029f5` |
| 7 | front ambient-and-diffuse color material | `d0ae3b1aa247fce9` | `4341b47e5a7d641c` |
| 8 | two-sided lighting with distinct back material and CW face | `4e2d117582c15622` | `0f853cf337b48e1b` |
| 9 | separate specular color after texture modulation | `0a4cf49f866f58a7` | `8905be58e1a91777` |
| 10 | flat last-vertex provoking color versus smooth | `1e55c60ea48b2b75` | `a9e3b10fef47723e` |
| 11 | non-axis-aligned user clip plane | `ccb606073597327c` | `a9e3b10fef47723e` |
| 12 | object-linear S/T texgen with distinct planes | `e9662f748363e3a4` | `4b86b2ec47b48569` |
| 13 | eye-linear S/T texgen with distinct planes | `a5c88e4c23a6b244` | `4b86b2ec47b48569` |
| 14 | sphere-map texgen | `0a3c9a748e92416e` | `4b86b2ec47b48569` |
| 15 | normal-map texgen | `29b479bffc7be7b1` | `4b86b2ec47b48569` |
| 16 | reflection-map texgen | `fbbd97a02581154b` | `4b86b2ec47b48569` |
| 17 | translate/rotate/nonuniform texture matrix | `2293fab425c3eb10` | `4b86b2ec47b48569` |
| 18 | inverse-transpose normal behavior under rotate/nonuniform scale | `a6d8e04e30708fff` | `4341b47e5a7d641c` |

The five texgen modes produce five distinct full-frame hashes.  Flat shading
produces the last vertex's `78,35,f7,b5` color at all four covered probes.
The clip case exposes the exact clear color `07,0d,18,54` at a probe which was
covered in the control.  The clockwise two-sided case changes the back-face
probe from `0f,18,10,a1` to `a6,17,1f,ff`.

## Directional-light command and resource join

The normalized raw case-00 draw is ExecuteDma sequence 55: extent `0xf20`,
three descriptors, packet counts `183/37/3`, and two numeric draw records.
Its final resource state is:

| index | final value | role |
|---:|---:|---|
| `69` | `11e42000` | external fixed-fragment-database binding; address translation unresolved |
| `6a` | `25010000` | paired numeric resource control |
| `6b` | `00001030` | tagged system-record companion control |
| `6c` | `40c3b601` | system resource base `40c3a000` plus record `0x1600`, tag 1 |

Two stage spans are selected by `8c/8d/8e`:

| control | start | end |
|---:|---:|---:|
| `00503700` | `40c3ae91` | `40c3b049` |
| `00500200` | `40c3ae81` | `40c3ae91` |

The selected 0x21000-byte system resource is unchanged by submission.  Its
record at offset `0x1600` begins with the two exact vertex constant tuples
`{48,-48,.5,0}` and `{48,48,.5,0}`.  This reinforces the neutral resource
interpretation of index 6c: it selects a structured record containing geometry
and generated-state data; it is not simply a native program pointer.

The draw also uploads 80 native instruction pairs through indices `8f..9f`.
Together with the two draw records and the visible directional result, this
proves that this ordinary fixed lighting case reaches M9 generated GPU code.
Sequence 57 is the following readback setup, has extent `0x8cc`, two
descriptors and no draw record, so it is not mistaken for the lighting draw.

## What is closed and what remains low-level

The result semantics and state isolation are live-closed for all 19 cases.
Case 0 additionally has a complete command, generated instruction and selected
resource join.  Static evidence already maps the compatibility-state
dictionary and generated compiler inputs for the remaining cases, so a Mesa
implementation can lower these operations without inventing fixed packet
fields.

Per-case native/resource differentials remain missing for cases 1 through 18.
They would distinguish the precise generated program keys and constant record
offsets, and would prove that no individual vendor case chose its documented
software-T&L fallback.  Direct rendering plus the successful visible results
alone cannot prove that boundary for each case.

The old ICD wedged during the repeated selective tracing batch after all
result-only cases had completed.  One earlier map-heavy run timed out from
observer I/O while leaving the host healthy; the subsequent per-process batch
then hard-wedged during its second case.  Capture stopped at the repeated
failure as required.  The raw archive preserves both successful case-00 runs
and the failed batch remnants.  NanoKVM reset recovered the host, and the
default vendor X stack was restarted and verified direct afterward.

The remaining fixed-function live families are the complete texture
environment result matrix, polygon offset values, low-bit dithering, stipple,
and pixel/evaluator/software semantic cases.  Bump-environment state is not
exposed by this Linux GL extension set; its final live result needs a Windows
Direct3D capture or a narrower internal hook rather than a nonexistent GL
entry point.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
