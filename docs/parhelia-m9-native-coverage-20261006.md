<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9128 GPU geometry coverage and presentation evidence, October 6

Historical runtime `335d442` (rendering change `5ad6c17`) supports five native
output streams in its compiler but defaults to **two** hardware-delivered
streams. Texture transforms and coordinate sharing are enabled; a separate
coordinate beside another stream and native multi-pass fragment shaders remain
gated. Vendor `vs-varyings-max` transports fourteen varying slots using nine
preprocessing draws and three raster passes. That captured multi-draw grammar
has not yet delivered a third native varying through the modern driver; five
previous chain/descriptor experiments rendered nothing or stalled. Do not
raise the default cap based on the vendor's larger API limit.

## Current deployed result

Canonical32fa687 is built/deployed/tested with defaults enabled. Native
ffarray2/ffarray/texture/texaff/textureddepth readbacks are exact. Packed XY/ZW
coordinates keep two independent texture arrays plus color within the measured
two-stream hardware limit. The full offline VS corpus is43/43 plus6/6 expected
refusals. [Latest physical proof](../tests/m9-packed-coord-native-20261006/README.md)
pins source, DSO, actual M9128 identity, same boot and complete workflow.

One1024x768/fullscreen/vsync0 Quake3 timedemo reports51.4FPS, versus49.5 in
the prior unit-projector candidate. About99.2% GPU calls, CPU60rasterizer
fallbacks/60frames, no projector/input-cap fallback.70jobs/frame and15uniform
splits remain. Native submission lies outside CPU draw timing, so the older
all-nested timing description below applies only to its historical CPU path.
No full benchmark repetition. FAA off throughout; two FAA investigators
continue command/resource admission independently from performance work.

## Historical Quake3 baseline

`tests/m9-q3-profile-20261006` retains the M9128, 1024x768 fullscreen,
OpenGL1 renderer, uncompressed 32-bit texture, vsync-off diagnostic. One
387-frame timedemo reports **49.5 FPS**, against the immediately preceding
same-board **49.6 FPS** ballpark. It is not a P690 improvement.
Steady sixty-frame intervals average approximately:

| Evidence | Per frame |
| --- | ---: |
| Wall time | 19.8 ms |
| Draw scope, including nested work | 13.4 ms |
| Emit scope, nested in draw | 10.6 ms |
| Submit scope, nested in emit | 8.9 ms |
| Ioctl scope, nested in submit | 6.9 ms |
| Jobs / draws | 54 / 129 |
| Program / depth / mask / clip splits | 53 / 36 / 5 / 3 |
| Uniform / vertex-ceiling splits | 0 / 0 |
| Native draws / draw module per 60 frames | 0 / about 7500 |
| Refused / skipped | 0 / 0 |

The timing scopes overlap: summing them overstates total cost. About 2.8 ms
of draw time lies outside emit, while program changes dominate job boundaries.
Per-object uniform-span batching is therefore not the first performance fix
for this particular GL1 workload. The active fallback reason needs the interval
reason tally; the initial temporary context teardown (`native 0, module 0`)
does not identify gameplay coverage. A short gameplay interval suffices to
capture that tally without repeating the complete timedemo.

## Implemented coverage prerequisite, native readback validated

Mesa folds short uniform-decided `if` branches to `bcsel` ALU before driver
finalization. `m9_nir_find_inlinable_uniforms` inspected remaining if/loop nodes
only, leaving those selects unspecialized. A real scalar NIR fixture with three
vec4 inputs therefore emitted all three even when the uniform selected one.

The shared discovery now follows select conditions that contain only uniforms
and literals. The early native geometry check defers inlinable shaders to the
existing compiled-layout validator, keeping the hardware output cap unchanged.
Offline choices 0/1/2 now emit exactly their one selected input; choice 3 really
reads three and stays distinct. A mixed input/uniform condition still marks
zero inlinable uniforms. Exact required vendor programs remain **23/23**, full
corpus **177/182**, while total emitted slots fall **1660 -> 1655**.

`tools/m9-mesa-glsl.c` adds `varychoice`: four uniform changes in one context,
full readback against independently scaled interpolated colors each time. Its
native acceptance check is three GPU draws, one CPU fallback, and zero wrong
pixels in all variants. This is a coverage fix, with no measured Quake3 gain
claimed for Quake3. The native fixture passes all four full readbacks;
[the native proof](../tests/m9-native-uniform-select-20261006/README.md)
records the actual first GPU draw and the final three-input CPU fallback.
The bounded gameplay profile now identifies approximately63% projector,
36% output-cap and1% rasterizer-state fallback calls; these are counts, not
time shares. Projector semantics must be proved before changing that guard.

## Remaining prerequisites

* Three or more native streams: preserve full vendor preprocessing chain
  state/lifetime and resources, then explain why the existing matching route
  still does not deliver output; another output-register seed guess is not a
  prerequisite proof.
* Per-object uniform batching: establish per-draw constant-span relocation and
  visibility from a complete vendor job. The historical CPU-only GL1 run has zero such splits; latest native coverage exposes15/frame.
* Submission/presentation: the existing path already overlaps submission and
  engine work. These intervals cannot independently attribute all ioctl time
  to Present or vblank; vsync is disabled. Root should inspect active fallback
  and shader-driven job boundaries before changing presentation synchronization.

## Dominant gameplay fallback, identified after the coverage fixture

The bounded gameplay reason report after the select fix has approximately
7534 CPU calls per interval: 4774 projective-coordinate refusals, 2700 original
fragment inputs exceeding the output cap, and 60 unsupported raster-state
calls. No native Quake3 draws occurred. The first item is about 63 percent of
calls and is the immediate investigation target; a coordinate-pair gate was
not reached in this report.

A projector of exactly one needs no divide, but this must be proved from the
actual specialized vertex output and draw's input defaults, not assumed from
the application's use of 2-D textures. The optional bounded fallback dump
retains both shader NIR, vertex element formats and constants after a completed
profile interval. `m9-mesa-glsl ffarray` is the corresponding fixed-function
MODULATE fixture using a two-component client texture coordinate array. Its
missing q is one by the existing source-record unpack contract. The existing
`ff` immediate-mode test is unchanged. Both use the independently computed
texture-times-interpolated-color reference. The new fixture host build passes;
Producer proof and native readback now pass in the retained
[unit-projector proof](../tests/m9-unit-projector-native-20261006/README.md), superseded by packed-coordinate deployment. A shader or four-component source with genuinely varying q
must retain the guard.

### Two-coordinate client-array companion proof

`m9-mesa-glsl ffarray2` extends the fixed-function array case to two MODULATE
texture units, separate two-component TC0 and mirrored TC1 arrays, and color.
Its independent full-frame reference is texture(TC0) * texture(TC1) * color.
The mirrored array exposes accidentally duplicated XY or incorrectly selected
ZW even when both units bind the same traceable image. This reproduces the
captured Quake3 diffuse/lightmap producer layout; source arrays provide q=1.

The paired-coordinate native candidate uses two existing output streams: color
and XY/ZW coordinates. It retains the measured hardware stream cap and refuses
nonidentity transforms. Offline native program comparisons pass (43/43 plus
6/6 expected refusals). Native acceptance remains pending and must check both
pixel results and actual native/fused-pair/four-attribute metadata. No new FPS
number is inferred. Performance validation is deferred while FAA command state
and rendering are prioritized.
