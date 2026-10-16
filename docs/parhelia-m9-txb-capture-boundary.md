# M9 texture bias: captured route, missing varying mip-selection proof

## What the retained vendor stream proves

`python3 tools/m9-txb-evidence.py --audit-existing` verifies the original
manifest hashes, Matrox M-Series renderer, linked shader and saved C2 image,
and decodes the native commands for three independent GLSL cases. The
checked result is retained in
`captures/m9-txb-route-audit-20261006/evidence.json`.

| Saved case | Consumer index 70 | Bits 11:10 | Coordinate record 79 | Route 74 |
| --- | --- | --- | --- | --- |
| `fs-tex2d-swizzle` | `0004111c` | 0 | `ee090002` | `00ff0110` |
| `fs-tex2d-proj3` | `0004151c` | 1 | `ee190204` | `01000310` |
| `fs-tex2d-bias` | `00041d1c` | 3 | `ee190204` | `01000310` |

The comparison joins another field in the unit selector: the literal-bias
route sets bits 11:10 to 3, projection sets them to 1, and a plain route
leaves them zero. This is **CAPTURED**, not a new shader opcode. The producer
writes four float dwords, `(s, bias, s, t)` for the bias case; the consumer
starts at dword 2 and its modifier comes from route component 3, dword 1.
The route's resource/descriptor form and the unit's sampling mode both matter.
The current `m9_draw.c` route emitter changes only the route selector and
bit 12; adding a third record operand alone would still miss the bias mode.

The saved GLSL bias source is exactly
`texture2D(s0, gl_TexCoord[0].xy, 1.0)`. It uses a **literal**, not a
varying bias. Its application sampler has `7c=02409200`: neither nearest-
nor linear-mip filtering is enabled. Thus its image cannot demonstrate
which mip a changing bias selects. Earlier notes describing per-fragment
TXB as already captured overstated this boundary.

What stays open is the actual vendor path for a varying or fragment-computed
bias, including original-coordinate derivatives, sampler bias addition,
LOD bounds and mip interpolation. A record descriptor and a successful
non-mip image do not establish those behaviours. No runtime TXB route is
implemented in this round.

## Ready capture probes

`tools/m9x-glsl-program-corpus.c` adds seven independent probes. Existing
corpus cases keep their uploads and sampling state. New cases use a complete
128x128 texture with eight mip levels, explicit RGBA8 storage, nearest texel
filtering, disabled dithering, and a different solid colour at each level:
`(16+29*level, 240-23*level, 32+17*level, 255)`.

Texture coordinates span four repeats across a 128x128 quad, so their
implicit LOD is 2. The colour identifies the chosen mip without an image
pattern or wrapping affecting the result. The sampler state is queried into
`case.json`, and each probe records full RGBA readback.

| Probe suffix after `fs-tex2d-mip-` | Shader bias / sampler change | Purpose |
| --- | --- | --- |
| `base` | no shader bias | original-coordinate derivative control |
| `bias-one` | literal +1 | make the retained constant route LOD-sensitive |
| `bias-varying` | `4*s-2` | changing bias in one draw, expected mip bands |
| `bias-nonlinear` | `16*(s-0.5)^2-2` | bias computed per fragment; vertex evaluation gives a different image |
| `bias-sampler` | `4*s-2`, sampler bias +0.75 | preserve sampler bias addition |
| `bias-clamp` | `4*s-2`, sampler LOD bounds [1,3] | preserve min/max LOD clamping |
| `bias-blend` | `4*s-2`, nearest texel / linear mip filter | preserve fractional mip interpolation |

`--describe CASE` emits exact GLSL sources without opening X. The existing
native FS compiler accepts the base and literal-bias controls in one pass;
all five varying-bias probes compile their GLSL successfully but the M9
backend refuses them with `texture sampling needs a computed coordinate pass`.
This establishes the present implementation boundary with actual sources.

The capture client builds with `-Wall -Wextra -Werror -std=gnu99 -O2`.
The saved-stream audit passes. A local software X-server run could not start
because it could not establish listening sockets, so no software
rendering result is asserted. None of these preparation checks is an actual
new vendor capture.

## Concrete next vendor capture

Root owns the target lock and chooses the vendor host. On a boot with the
genuine M-Series kernel/X/GL stack, record PCI device, revision and subsystem,
loaded kernel module, vendor library hashes, display mode, renderer and direct
context identity. Keep those beside the capture. Do not run this against the
current Mesa deployment and label its output vendor ground truth.

The existing wrapper accepts the seven names explicitly:

```sh
M9_HOST=root@VENDOR_HOST M9_LOCK=ESTABLISHED_TARGET_LOCK \
  tools/m9x-glsl-program-corpus.sh captures/M9_BOARD-txb-DATE \
    fs-tex2d-mip-base fs-tex2d-mip-bias-one \
    fs-tex2d-mip-bias-varying fs-tex2d-mip-bias-nonlinear \
    fs-tex2d-mip-bias-sampler fs-tex2d-mip-bias-clamp \
    fs-tex2d-mip-bias-blend
```

The wrapper captures both draws, command and resource bytes and the exact
C2 allocations during the held context. Retain the sampler/resource state,
constant spans and original readback before pruning. Generate manifests for
the new evidence as for the existing corpus.

Compare all seven raw images with the independent mip-selection model:

```sh
python3 tools/m9-txb-evidence.py --require-vendor \
  captures/M9_BOARD-txb-DATE/fs-tex2d-mip-*
```

The model avoids nearest-mip threshold columns within 0.02 LOD and reports
checked pixels, mismatches, worst byte error, renderer and readback hash;
it never marks a non-vendor renderer as vendor proof. The model is a derived
sampling expectation. Preserve differing genuine vendor output for analysis,
instead of replacing it with a generated expected image.

Only after the varying/nonlinear captures join their producer arithmetic,
record layout and consumer mode should the backend pack the coordinate plus
bias, cut at that packed value, and emit the captured mode. The runtime must
keep constant bias folding, sampler addition/bounds, and final colour/depth/
blend/query semantics intact. Cube, 3-D, projected and shadow combinations
remain separate unproved forms; do not extrapolate this 2-D route to them.
