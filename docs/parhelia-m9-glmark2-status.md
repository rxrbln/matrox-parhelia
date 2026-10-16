# glmark2 on the m9x driver: where the 60 fps goal actually stands

## 2026-10-08 final wind-down checkpoint

All three background agents completed. Canonical DRM956e813/Mesa894fe3f
and finalfullmodule/23-objectDSObuilds pass; new builds are not deployed.
Targetbootd4ca4e03-cf6c-4512-bff0-14a0d5182c85 remains poweredon, healthy,
submitted=completed7b460/gen0/queue0/async0/wedged0; noX/app/testrunning.
Current loadedDRM3f93002/Mesa01eef6c preserve seven noFAA basic passes.
Today's focused Piglit is8PASS/1SKIP(ARB_framebuffer_objectrequired)/0FAIL.
FAA both320x240 triangle pixel controlsPASS; Q3FAA remains unfinished.
Later-dual pure encoder/owners integrated; unregistered runtimeca4d079 is
preserved onwip/m9-faa-dual-later-native and as an exact archived patch.
GL1 diagnostic saved1071jobs/529inputs but screenshotloading-only, so affected
job inclusion is unproved. GL2 timeout cause remains open; new exact fatal-job
capture is compiled/CPU-proved and awaits one future physicaldiagnostic.
[Complete authoritative handover and resume order](handover-20261008.md).

## 2026-10-08 latest: seven no-FAA images pass; Quake3 GL2 reaches GPU timeout

The latest1024x768 physical batch uses DRM3f93002 and Mesa01eef6c,
DSOca734b68995b6dfbb0d39f462e82f85f2182178967a72daccb6c2cfc72980ba6.
Glxgears114.411 appFPS and sixglmark114/115/115/114/113/115 pass basic
scanout review, no skips; allglmark naturalexit0. GL1 real gameplay still
has stretchedweapon/HUDcorruption. The per-job CPU view allocation fix
gets GL2 past ENOSPC, then a216-DWORDtwo-pass stream stalls atDWORD213:
target1ab240/completed1ab1e0, generation1/wedged1, contextlost/allskips.
No GL2gameplay/FPS or full-suiteX-health pass. Root archived raw evidence
before terminating its hungtestSSH and NanoKVMpoweroff/power recovery.
Applicationvblank0, display60Hz; appFPS is submissionrate.
[Exact current proof and images](../tests/m9-nonfaa-regression-20261008/hardware-compact/README.md).

The separately integrated nativeVS output subset88a1990 selects up tofive
live outputs from a shader declaringmore;49152NIR/ISA refs,12actualcontext
controls/default99/geometry11PASS, full23-objectbuild190dfcd286ae68ca1844f096745bd2048bd5b9191d45f3766d53ef0c93896ec0.
This new feature has not been deployed yet; seven-live unions/lightmap
sampler-record topology remain gated. Preserve optimizations and GPU transforms.

## 2026-10-08: ordinary rendering retested; Quake3 gaps remain

DRM45b33e6/Mesa6b03281 on the physical M9128 completes glxgears and six
1024x768 glmark2 scenes with FAA disabled, GPU work retired, no skips or
wedge and intact basic screenshots. Presentation candidate RE618895d0 avoids
the interval-zero full-frame CPU copies: app-reported gears112.082 FPS,
glmark113–115, versus about2 with the previous copy path. Display is60Hz;
this submission-rate result needs physical event/scanout timing audit.
No async hardware capability or uncapped raster-performance claim.

Quake3 GL1 has an active gameplay screenshot, but stretched weapon polygons
and corrupt HUD pixels remain. GL2 skips11594/23140 draws (9163 swizzled mix,
2431 interpolation) and lacks a screenshot after an X-server crash. The
previous unflip NULL copy is fixed; software cursor save-under now crashes.
GPU stays gen0/queue0/wedged0 on one ordinary boot. Starting ordinary apps
on a retained FAA graph still needs the ordered native context handoff.
[Exact test evidence and pixel review](../tests/m9-nonfaa-regression-20261008/hardware-sync-fallback/README.txt).

## October 8: GL2 work resumed; full back-end clear control fails

Root resumed the GL2 track while three agents continue FAA. No new general
GL2 operation or hardware feature pass was added in the preceding six hours.
One same-binary1024x768 multipass control clears the entire1MiB back-end object
instead of the estimated144KiB. The failure is unchanged: completed460,
submitted520, queue1/wedged1. All eight command bodies match the prior run.
No pixel/FPS claim or speculative production clear-span change follows.
Evidence lives in canonical Mesa
`tests/native-gpu-padding-runtime-proof/hardware-full-be-clear-20261008`.
The target recovered to boot37a97217-6098-45e7-be55-24c4c019d602, module absent.
Current investigation concerns actual native padding coverage and the
preprocessing/raster transition. The October7 wind-down below is historical.

## October 7 wind-down: latest multipass result

Work is paused at the user's request. New compiler/math host checks pass,
but the unchanged1024×768 GPU multipass fixture on canonical Mesaa295717
failed before readback: final520unreached, completed460/raw461, queue1/wedged1.
The first draw pair completes3a0; the second final FS pass JOB7 never publishes
its first470 dependency. Successful and failing command bodies are identical;
the cause is unresolved. No new pixel or FPS qualification is claimed.
Code, default-off gate and strict oracle remain retained. See the
[handoff and exact evidence](parhelia-m9-wind-down-20261007.md).

The older implementation/version notes below do not supersede that result.

## Current implementation gaps, checked against Mesa3c12a86

The older chronological entries below are not a current missing-op list.
Vertex log2/exp2/pow and bounded arithmetic branching are implemented; they
need broader application qualification, not a second implementation.

* Mesa957c46a implements one direct-UV vertex texture fetch with retained GPU
  resources and fragment-sampler restoration. It passes hardware pixels
  and defaults on in3c12a86. Installed glmark2 terrain uses direct UVs but
  needs two vertex reads and five linked varying outputs; those paths remain open.
* General data-dependent control flow and loops remain gated. Uniform
  specialization, static unrolling and bounded arithmetic if-conversion
  exist; conditional texture and side-effect paths are not generally covered.
* Indirect vertex inputs and indirect constant-buffer reads remain refused,
  including general dynamically indexed uniform arrays.
* Native input/output limits still gate shaders: four source attributes,
  five compiler output streams, and two streams by default. Wider packed
  paths pass selected fixtures and bump, not every application. Dynamic
  point size and clip-distance transport remain refused.
* Fragment varying texture bias and complete explicit LOD/gradient resource
  routing remain open, as do broader multipass and discard/depth/blending
  combinations. These include extension paths beyond baseline GLSL1.10.
* Vertex `fsign`, needed by `texture:texgen=true`, is implemented in1802132.
  Canonical build,86vertex references/five refusals and independent texgen
  math checks pass. Mesa3b02e61 corrects ordinary native register availability
  from vendor policy; the physical image now has a complete cube without the
  earlier horizontal bands, with2108GPUuser draws/zero CPUuser. Dark stretched
  texture is consistent with the software scene reference. Exact equal-pose
  pixel qualification remains open, as does the CPU fallback's banding.

The actual compiler/finalizer-to-mask consumer join now passes under bounded
no-vertex-texture policy (REd0102161); full live EE19 resource/view binding
and broad default-cap promotion remain distinct open work. Existing complex
bump rendering and its417FPS clean result do not qualify terrain, all Quake3
GL2 lighting variants, or the complete glmark2 suite.

## 2026-10-07: Q3 improves to44.7FPS; direct vertex textures pass hardware

The whole Q3 generic shader fixture passes0/1572864wrongpixels,maxchannel1,
with161instructions/61constant span,2GPUapplication draws/zeroCPU. The initial
fixture mistakenly counted an internal texture-upload blit as application
fallback;593811c corrects that distinction and the strict run passes.
One matching1024x768fullscreen basicGL2 run,FAAoff/vsync0/queue1, gives44.7FPS
versus40.7 (~9.8%); actual234640retirement,empty/wedged0. A ballpark single-run
comparison acrossboots. The post-timing menu screenshot retains title artifacts;
it is not full-scene/gameplay pixel proof.
[Q3 hardware evidence](../tests/m9-vectorize-hardware-20261007/README.md).

Mesa957c46a direct-UV vertex texture passes4GPUapplicationdraws/zeroCPU,
four sampler publications and0/1572864wrongpixels,maxchannel1. Both vertex
and fragment texture palettes change, then textures are deleted before
readback. Actual234a60retirement,empty/wedged0. The bounded path defaults on
in3c12a86; all existing restrictions and M9X_NATIVE_VS_TEXTURE=0 remain.
[Vertex-texture hardware evidence](../tests/m9-vertex-texture-hardware-20261007/README.md).

Mesa088441d additionally integrates paired-source/constant-key retry for
case1:226/74→176/61, capsunchanged,2048hostold/newbitidentical. Combined97refs/
12refusals,batch,49admission/lifetime/default-optout checks andbuildPASS.
This second retry still needs its own lightall source hardware pixels;44.7FPS
was measured before it. Remaining >4attribute fallback still needs work.

## 2026-10-07: bounded Q3 retry and direct-UV vertex texture integrated

Mesa2303be9 retries constant-limited shaders with bounded vectorization;
81ad856 adds the whole public Q3 shader pixel fixture. Actual case0 changes
137instructions/69constant span to161/61, under unchanged188/63 limits.
2048 host comparisons are bit-identical; physical pixels/FPS are pending.
Case1 remains refused in canonical code; agent prototype176/61 versus226/74
has host numerical evidence but is not yet integrated or hardware-qualified.

Mesa957c46a integrates a gated direct-input vertex texture path. Combined
build,97vertex references/12refusals, batch restoration and47actual admission/
retention checks pass. Physical changing-palette pixels remain pending.
M9X_NATIVE_VS_TEXTURE defaults off. The path avoids CPU texture refresh for
successful native draws while preserving refresh for draw-module fallbacks.
No complete terrain or new FPS result is claimed.

## 2026-10-07: constant reuse passes pixels; Q3 remains at 40.7 FPS

Mesa045e788 reuses safe broadcast constants, fcfbcc7 retains useful bounded
refusal diagnostics and f31f2ec adds the pixel fixture. Hardware passes16GPU
user draws/zeroCPU,0/1572864wrongpixels. One same-boot matching Q3 run gives
40.7FPS versus40.8: flat. Exact refusal dumps show137slots/69constant span,
226slots/74span, and a third shader exceeding four source attributes. Span
includes the13reserved prefix entries, not just occupied constant keys.
[Actual pixels, Q3 and refusals](../tests/m9-constant-broadcast-hardware-20261007/README.md).

Corrected vendor vertex-texture tracing joins live UV input to attribute5
(word04500688); earlier reg1 probes selected blend weight and do not prove UV
binding. Direct resource/stage evidence is integrated in REcfaa3c90. Native
vertex-texture implementation and application qualification remain open;
transformed UVs require an additional producer stage.

## 2026-10-07: submission scratch qualifies at 40.8 FPS basic Q3 GL2

Kernele1703f9 combines eight per-submit temporary arrays into one, preserving
GPU ownership. Hardware current-attribute pixels still pass16GPUdraws/zeroCPU,
0/1572864wrongpixels. One1024x768 fullscreen Q3 run reports40.8FPS versus39.1,
about4.3% higher, sameMesa8ce8651, FAAoff/vsync0/queue1 and basic lighting.
This is a ballpark comparison across boots. Laststeady ioctl6.40→5.25ms;
54jobs/frame and existing constant/slot/attribute refusals remain unchanged.
Actual231760 retires healthy, queuepeak4. No new GL2 feature is claimed by
this optimization. [Physical proof](../tests/m9-submit-scratch-hardware-20261007/README.md).

## 2026-10-07: ordinary native mask corrected; current attributes pass hardware

Mesa3b02e61 derives the ordinary preprocessing mask from actual scratch use,
preserving the captured tail reservations and coupling only bound output
groups. The submitted texgen program requiresbc00 rather thanbf00, confirmed
by genuine vendor150dc0 and twelve controls. One physical1024x768 native run
has no previous sawtooth geometry bands,2108GPUuser draws/zero CPUuser and
healthy130180 retirement. The703FPS includes a screenshot and is retained as
a diagnostic. Both app-only and app-plus-Xorg CPU-transform controls still
show the old bands; no CPU workaround is enabled in the implementation.
[Physical images and controls](../captures/m9x-texgen-fused-mask-20261007/README.md).

Mesa8530cf9/8ce8651 additionally accepts zero-stride current attributes and
expands their actual per-draw values into owned GPU source records. Actual
ordinary/indexed draws with changing colours pass16GPUuser draws/zero CPU,
0/1572864 wrong pixels/maxchannel1 at1024x768.
[Hardware pixel proof](../tests/m9-current-attributes-hardware-20261007/README.md).
One fullscreen Q3 basicGL2 run now reports39.1FPS versus35.5 in the previous
matching fullscreen checkpoint (FAAoff/vsync0). Six draws/frame move from
CPU to GPU transforms; jobs55→54. This is a single-run integrated comparison,
not isolated attribution or a new qualification of optional lighting modes.
[Q3 profile and measurement](../tests/m9-q3-current-attributes-20261007/README.md).

## 2026-10-07: vertex sign integrated; native texgen image still fails

Canonical1802132 lowers sign with the existing vendor compare/MAD/select
forms. All86host vertex references and five required refusals pass. Three
sign fixtures each test512independent inputs exactly; the complete installed
texgen shader passes512double matrix/normalize/libm-asin references with
maximum error1.58e-5. It uses137slots/48constant pairs/two attributes/one
packed varying, without raising limits.

One physical1024x768/windowed/one-head/FAAoff/vsync0 run on normal73,
boot4636e49f, uses defaultRECORD1/VARYINGS2 andQUEUE1. All2837application
draws use GPU transforms; final9b0e0/raw9b0e1 retires with an empty healthy
queue immediately and2seconds later. The actual screenshot shows distorted,
dark geometry with horizontal bands. The image is FAIL, so the reported
946FPS is not a valid rendering benchmark. Root retains the sign improvement;
the new observation does not identify sign arithmetic as the cause.

Mesabb0c20a subsequently adds bounded input-only capture for this ordinary
native path. Two actual frames retain the submitted C2, source attributes,
constants, declarations and output layout, with two GPU user draws and zero
CPU user transforms. Final9b500/raw9b501 retires healthy with an empty queue
immediately and two seconds later. These are submitted inputs, not GPU
vertex-output readback or another image qualification. The actual shader uses
scratch registers10/12 beside varying outputs11/13 while its ordinary mask
remainsbf00. Vendor mask-policy comparison is the next diagnostic scope.
No full independent application pixel reference is claimed.
[Exact failed-image evidence](../tests/m9-gl2-texgen-hardware-20261007/README.md).
[Actual ordinary inputs](../captures/m9x-texgen-fused-inputs-20261007/README.md).

## 2026-10-07: complex bump clip coordinates pass; clean run reaches 417 FPS

Canonical Mesa `05c7c29` follows the actual vendor finalizer's coupled
output-pair availability rule. One 1024x768 height-bump qualification passes
all 1056 clip-coordinate components for 264 vertices and all 1440 index
bounds, with 156 GPU user draws and zero CPU user transforms. The captured
window shows the textured, lit bump object. The exact record, source inputs,
independent matrix oracle and PNG are in
[the hardware archive](../captures/m9x-glmark-bump-coupled-mask-20261007/README.md).
This closes the observed complex bump position corruption; it is not a full
independent pixel reference for every varying or fragment result.

The captured run reports 37 FPS with complete input/program dumps and record
readback. At the user's performance request, one separate three-second run
of the same DSO without that instrumentation reports **417 FPS / 2.400 ms**,
1024x768 windowed, one display head, FAA/vsync off, RECORD2/VARYINGS5/QUEUE1.
It completes 1251 GPU user draws, zero CPU user transforms, and retires
`84ac0`/raw `84ac1` with an empty, healthy queue immediately and two seconds
later. The factor relative to 37 FPS is diagnostic overhead, not a measured
code speedup. Prior 34/43/85 FPS bump diagnostics had incorrect geometry and
do not establish a valid clean rendering baseline. This clean run does not
contain a second pixel capture; image and clip evidence belong to the
separate qualification with the same binary.

The source mask fix preserves shader instructions, component slicing and
BE-owner synchronization. Default varying cap remains two; broader packed
shader coverage, the full EE19 binding policy and other GL2 scenes remain
open. [Source and hardware scope](parhelia-m9-complex-bump-mask-hardware.md).

## 2026-10-07: indexed variableZ/W and forced-clear fixtures pass

The actual indexed264-vertex diagnostic with vertex-varying perspectiveW
passes both6336record components exactly and both65536pixel palettes with
zero wrong pixels/maxchannel1. It uses GPU user transforms and fresh retained
record metadata. This excludes vertex count, indexed draws or varyingZ/W
alone as the complex bump failure.
[Exact hardware proof](../captures/m9x-native-variable-zw-264-20261007/README.md).

Mesa21a446f selected back-end owner waits also pass the full1024x768
forced-clear regression:221184components, both786432pixel palettes and
5BEstate swaps, healthy23cda0 retirement. Queue peak4 and async target are
from earlier work, so no asynchronous throughput improvement is inferred.
Component-aware dependenciesbd792ac reduce the bump split program by
23slots,179to156, preserving default program bytes; no FPS gain is claimed
for an application whose rendering still fails.

Read-only source/C2/owner audits find disjoint logical packed and later
flat views. Most missing expectedW values do not occur anywhere in the
retained output span; a decoder permutation alone cannot restore them.
Complex vendor register allocation and availability masks remain under
emulator investigation. Full GL2 and default varying promotion remain open.

## 2026-10-07: paired publication follows vendor form but Z/W still fail

Mesa5f78a2d stages mixed output scalars in an ordinary temporary before a
complete packed register-pair MOV, matching the actual vendor source corpus.
Host79reference/5refusal/batch/fullDSO checks pass. One short1024x768
physical height-bump test retires34native user draws/zero CPU user fallback,
real3d00/raw3d01, queueempty/peak2/wedged0 immediately and2s later. Its exact
last retired job69 record still fails: all264X/Y values match,156Z/W pairs
fail in ranges12..107 and204..263, all1440indices valid. This is not a
sufficient corruption fix; source-proven publication improvement is retained.
The captured350ms X-window image is black, with possible startup timing;
no image qualification or FPS gain/regression follows. Full proof is copied
at /tmp/m9-gl2-paired-output-20261007/proof; GL2 agent is archiving and
investigating writer/record-bank ownership. Default varyingcap2 unchanged.

## 2026-10-07: actual bump inputs narrow the position corruption

Current Mesa883280f/DSOb29 retains complete source vertices, indices,
constant runs and exact retired-record metadata from two actual bump frames.
All264clipX/Y values match;128clipZ/W pairs fail in ranges40..135 and232..263.
All1440logical indices are in range. Partial scalar packed publication versus
vendor output-store semantics is under investigation; the responsible layer
is not yet proved. Final17140/raw17141 retires with queueempty/peak1/wedged0
immediately and2s later, noGPUtimeout. Diagnostic43FPS is not a benchmark.
The previous captured bump image remains visibly incorrect.

A separate queued map-update wireframe run has20native draws/zero CPU,
healthy retirement and a visible structured wave mesh. Its20FPS includes
readback/startup during the one-second run and cannot be compared directly
to34FPS QUEUE0. CPU-only map optimization4f2012e is retained. Completion
cache071517c host concurrency/error/lifetime tests pass; no measured FPS gain
is claimed. Default varyingcap2 remains.
[Queued-map proof](../tests/m9-cpu-buffer-queue-hardware-20261006/README.md).

## 2026-10-06: complete strict packed records and textured/depth pixels pass

Integrated Mesa307ff0a/4f2012e, rootDSObca674ce, now passes a fresh full
1024x768 fixture with exit0: all221184actual record components and both
independent786432pixel palettes pass. Six GPU application draws,0application
CPU transforms, real14440/14560retirement, emptyqueue/wedged0/noGPUtimeout.
The versioned dumps retain full442368byte payloads and the measured six-field
tile/bank decoder; negative controls reject incomplete/wrong layout records.
This closes the prior fixture-only raw-record failure. It does not close the
bump application image failure below or qualify all GL2/defaultcap promotion.
[Exact strict hardware proof](../tests/m9-gl2-record-complete-hardware-20261006/README.md).

## 2026-10-06: bump-height retires but actual image fails

The next1024x768 bump-height run completes170GPU user draws/0CPU user
fallback with healthy142c0 retirement and noGPUtimeout. Its actual X window
capture shows stretched triangles reaching toward the screen edges. This
is an image failure; printed85FPS is not a validated rendering benchmark.
Actual metadata also has six fields, with264source vertices and indexed
rasterization; the standalone non-indexed proof does not qualify this workload.
Packed transport/coordinate/index diagnosis remains open; no cause
is yet established. Defaultcap2 stays unchanged.
[Exact failed image and evidence](../tests/m9-gl2-bump-packed-hardware-20261006/README.md).

## 2026-10-06: actual buffer wireframe now completes on the GPU

One short1024x768 windowed run with Mesa c7b8a6e/DSOdc6e40b6,
FAAoff/vblank_mode0 and explicit RECORD2/VARYINGS5 completes with exit0:
34GPU user vertex draws,0CPU transforms,0refused/skipped. Actual3a60 fence
retires, queueempty/wedged0 immediately and2s later, noGPUtimeout. The
duration1 run reports about34FPS with profiling enabled. The older68FPS
failed-run number is invalid and cannot establish a regression. Independent
image fidelity is not yet qualified. Defaultcap2 remains unchanged.
[Exact application evidence](../tests/m9-gl2-wireframe-packed-hardware-20261006/README.md).

## 2026-10-06: full 4608-vertex route renders correctly at1024x768

The same Mesa c7b8a6e/DSOdc6e40b6 now passes both complete1024x768 palettes:
zero wrong pixels of786432 each, maximum channel error1. Full4608-vertex
four-stage textured/depth chains retire4c0/5e0 without GPU timeout, queue
empty/wedged0, six native GPU user vertex draws and zero user CPU fallback.
This is a substantial physical rendering correction for the previously
wedging layout. Its overall fixture still rejects the assumed dense CPU
record layout; actual complete bytes are retained for tiled component-plane
and extent proof. Defaultcap2 remains. Next qualify actual glmark2 scenes,
radial multipass and optional Q3 lighting; no FPS gain is claimed yet.
[Exact full-size evidence](../tests/m9-gl2-packed-order-full-hardware-20261006/README.md).

## 2026-10-06: packed publication fix gives correct textured/depth pixels

Mesa c7b8a6e follows the vendor GLSL high-before-low packed output schedule,
retaining safe paired pins and using normal temporaries for coordinates.
Root79/79 references,5refusals, vendor witness, batch and full DSO pass.
One fresh physical M9128 test now passes all256 textured/depth pixels in
both palettes, max channel error1; six user draws use GPU vertex shaders,
zero application CPU fallback. Actual220/340 retirement, emptyqueue and
wedged0 are preserved. The record oracle still rejects its assumed dense
layout: raw bytes instead show component planes at64-DWORD boundaries.
Its dump/allocation-span contract needs proof; full large-workload check is
next at1024x768. Defaultcap2 remains, no FPS gain is claimed.
[Exact first correct pixel evidence](../tests/m9-gl2-packed-order-hardware-20261006/README.md).

## 2026-10-06: source stage corrections retire both tiny palettes; pixels still fail

Mesa038b89e derives A8 from the native intermediate/final stage role.
Mesa7eb502a derives register89 target classes and native high bits from
actual targets and vendor source. Each correction was tested once with the
same bounded four-stage/five-stream textured, depth-tested workload. Both
palettes retire in both tests (220/340 and4c0/5e0), with empty queues,
wedged0 and no GPU timeout. Both fresh raw records remain incorrect and all
256 pixels in each palette fail, maximum channel errors114 and121. The
source fixes stay; full4608-vertex dispatch remains unqualified. No FPS gain.
[A8 hardware evidence](../tests/m9-gl2-role-state-hardware-20261006/README.md)
and [output-class hardware evidence](../tests/m9-gl2-output-class-hardware-20261006/README.md).

Mesa7f21ae0 preserves aggregate counters and separately reports draws while
the actual utility blitter is running. The fixture still requires six GPU
application draws, zero application CPU fallback and valid records/pixels;
the texture-upload utility copy remains visible. Root production helper
positive/negative checks,512-cell oracle, batch and full DSO build pass.
This classification correction does not qualify physical pixels. Current
default Q3GL2 performance remains the38.9FPS checkpoint below.

## 2026-10-06: preserved small GL2 record and pixels fail; next palette stalls

Same healthy normal boot and unchanged driver/DSO, improved fixture: first
chain3a0 retires with wrong records and256/256 wrong pixels. Clear400 retires,
second chain4c0 times out; raw401/completed400/wedged1 remains unchanged2s
later. No late completion observed. Thus small single-draw retirement does
not prove sustained health. The fixture copied stale palette0 data after
the timeout; that file is labelled explicitly. Canonical6859cdc fixes record
freshness and root host checks pass; driver cause remains open.
[Actual retained record/pixel evidence](../tests/m9-gl2-record-capture-hardware-20261006/README.md).

## 2026-10-06: tiny four-stage route retires, record oracle rejects output

Fresh normal M9128 boot, Mesa dc1db2e and kernel73c2a45: the full optional
five-stream chain completes at16x16/one cell/24 padded vertices. Actual220
retirement, raw221, wedged0 and no GPUtimeout are observed. The guarded
2304-byte packed dump fails its first field value (0.12890625 vs expected-2),
so no pixel qualification or speed gain follows. The full4608-vertex run
below still fails. Submitted C2/source/constants and read-only transport
observations are retained; raw packed file was unlinked by the fixture and
needs explicit preservation in the next diagnostic.
[Actual minimal hardware proof](../tests/m9-gl2-minimal-hardware-20261006/README.md).

## 2026-10-06: bounded reciprocal/root branches support radial GPU passes

Mesa3948308 admits the unchanged captured radial shader with two/four stops
through7/9nativeGPU passes by enabling bounded pure expensive-ALU selection.
Unsafe speculation guards and defaultcap2 remain. Root full fragment/vertex/
batch gates and full DSO build pass; independent native-word radial reference
checks12288cells/count, with three exact-zero boundary limits explicit.
[Root integration evidence](../tests/m9-expensive-alu-integration-20261006/README.md).
Hardware seed/pixels/record publication remain unqualified; no new FPS claim.

## 2026-10-06: current default Q3GL2 hardware regression passes

Mesaebe263e current default path completes one short native Q3GL2 timedemo:
**387frames/9.9seconds/38.9FPS**,1024x768windowed, FAAoff/vsync0, basic
HDR/toneMap/normal/specular/deluxe mapping0 profile. NoGPUtimeout/contextloss.
This is close to prior38.3FPS; no convincing speed gain is established.
Actual current DSO8d62f1ac... and genuine legacy DRI entrypoint were checked.
See [exact current hardware proof](../tests/m9-q3-gl2-current-20261006/README.md).
Optional four-stage five-stream qualification still fails below; default
rendering success does not qualify that route or all optional lighting.

## 2026-10-06: complete optional route pipeline integrated; pixels pending

Canonical Mesa58674dc derives all four stages for three to five streams from
actual native outputs, strides, offsets and retained owners. Default varying
cap2 and RECORD1 remain unchanged; optional routes require RECORD2. Root full
DSO,76numeric references/five refusals,512geometry trials, batch bounds,
pinned vendor witness and independent CPU oracle self-test pass. These host
checks do not prove device pixels. Earlier per-head slot counts below describe
that compiler checkpoint, not the complete current four-stage dispatch cost.

Mesaebe263e also submits pending native work before deleting its owned vertex
shader, fixing a pre-existing CPU metadata lifetime error. Actual ordinary and
four-stage helper tests pass; removing the new sync causes both tests to fail.
Full root DSO8d62f1ac8bb38aac365cdc52d7c4807a8f23c160b2ab551ca5424986b3329218 builds.
See [integration proof](../tests/m9-gl2-route-integration-20261006/README.md).
Fresh normal M9128 hardware qualification now FAILS: preceding native blit100
and clear160 retire, but the optional four-stage five-stream dispatch times
out at220 with writeback161. No GPU record/pixel qualification or FPS gain.
The initial loader-only llvmpipe refusal is retained separately and was fixed
by including the linked unified Gallium DSO directory in LD_LIBRARY_PATH.
Mesa20b02d4 now implements the actual vendor preprocessing stage publication,
restart and index43 dependencies in one range, with live framed lengths.
Root build/batch/sourcewitness and independent framing/lifetime review pass.
[Corrected integration proof](../tests/m9-gl2-pre-transition-build-20261006/README.md);
Corrected physical qualification also FAILS220/writeback161, despite
preceding blit100/clear160retirement. No stage-specific stall or eventual
completion proof follows from that100mswatchdog snapshot. The source fix stays;
read-only fence observation and bounded stage isolation are next.
See [corrected device proof](../tests/m9-gl2-pre-transition-hardware-20261006/README.md).
See [actual device evidence](../tests/m9-gl2-route-hardware-20261006/README.md).

## 2026-10-06: wireframe compiles natively; physical dispatch fails

Mesa465b546 reduces the installed buffer wireframe vertex shader from299
slots to188, within the unchanged default188 limit. Compiler/reference and
vendor-source checks pass. One short physical M9128 run at1024x768,
FAAoff/vblank_mode0, enters native1/drawmodule0, then loses the context:
`two-pass job failed (Input/output error)`. The kernel reports an actual
stream timeout. Printed68FPS is invalid and there is no measured gain.
The improvement is retained. Mesa f437a9b corrects a source-proven early
finish marker copied into the register-dot opcode: actual vendor head dots
omit bit30 and the tail owns it. Marker-placement regressions, numeric geometry
references and the full DSO link pass; corrected GPU dispatch remains untested.
See [the complete hardware proof](../tests/m9-gl2-wireframe-20261006/README.md).
A separate one-run basic Quake3GL2 timedemo on the same Mesa/kernel
completed387frames/10.1seconds/**38.3FPS**,1024x768windowed,FAAoff/vsync0.
HDR/toneMap/normal/specular/deluxe mapping are0; optional full lighting and
independent image fidelity remain unproved. No matching before/after gain
is claimed. See [the exact profile and logs](../tests/m9-q3-gl2-20261006/README.md).

Mesa `b6bedfb` additionally slices pure ALU work per independent vertex head.
Installed bump-height3-stream cost230->134 slots; five-stream237->164.
Packed coordinate dependencies,512 numeric transform/normalization trials,
75/75 references,5 refusals and full canonical DSO link pass. Default varying
cap remains2; optional multi-varying route-fed transport is still being
implemented. This is retained GPU-work reduction with no measured FPS claim.
See [the integration evidence](../tests/m9-vs-output-dependencies-20261006/README.md).

Historical standings below use older builds and different resolutions.


## Read this first: five standings below, measured on different builds

The **eleventh** is the current one: `M9X_VS_MAX_SLOTS` raised from 128 to
188 where the vertex stage's own cost steps, and the constant span bounded at
the 63 pairs the engine takes.  The **tenth** is the build before it: the
vertex sine on by default and the vertex subtract's two operand fields put
the right way round.  The **ninth**
is the build before it: the vertex program specialised on the
uniform that bounds its loop, and the register a dead intermediate used to
hold given back.  The **eighth** is the build before it -- the coordinate
stream's coefficients moved out of the per-job constant span,
`M9X_VS_TEXTURE` on by default.  The **seventh** measures the second varying
(`M9X_VS_VARYINGS` now 2) with one process a benchmark on the build before
that; the **sixth** measures the whole default run in one `glmark2`
invocation on the build before *that*.  The older ones are kept as the
measurements they were and are not swept together with the ninth.

## Standing 2026-09-20 (eleventh): the length bound raised, and nothing moved

MEASURED on the M9128 at 1920x1200 on a **freshly rebooted card**, module
`/root/tmp/dxtmip-m9x_drm.ko` named explicitly, **one X server and one
`glmark2` invocation an arm** against one deployed library (md5 `6b1fdaad`)
carrying both arms behind the knob, both hardware lock files held across the
whole cycle, `M9X_SKIP_REPORT=1`, `M9X_PROFILE=40`, `duration=5` on every
benchmark.  **33 of 33 results and rc=0 in both arms, 0 stream timeouts**,
and `texture`, `texpair`, `rtt300`, `vary2tex`, `vssin` and `vsbool` each
0 of 76800 pixels different before and after the sweep.  The fixture suite
on the same card is **298 cases, 298 PASS, 0 FAIL, 0 non-M9**, which includes
the seven new vertex chain and span cases.

`M9X_VS_MAX_SLOTS=128` against `M9X_VS_MAX_SLOTS=188`, the new default:
**twenty-seven of the thirty-three rows are equal to the frame rate**, and
the loudest four are `effect2d 3x3` 1.049, `buffer subdata` 1.048,
`blinn-phong-inf` 0.986 and `jellyfish` 0.986 -- the sweep's own noise.  The
mean ratio is 1.000.

**Nothing changed path, and that is the result.**  No glmark2 vertex shader
compiles to between 129 and 188 slots, so the raised bound admits none of
them; the three `buffer` rows stay in the draw module because their shader is
**299 slots**, still past the bound and still measured to lose there.
`bump:height` compiles to 224 slots and stays out for the same reason.  The
bound moved to where the measurement puts it, and this sweep is the proof
that moving it took nothing with it.

## Standing 2026-09-20 (tenth): the vertex sine on the engine, and no regression

MEASURED on the M9128 at 1920x1200 on a **freshly rebooted card**, module
`/root/tmp/dxtmip-m9x_drm.ko` named explicitly, **one X server and one
`glmark2` invocation an arm** against one deployed library (md5 `1582bf8b`)
carrying both arms behind its knob, both hardware lock files held across the
whole cycle, `M9X_SKIP_REPORT=1`, `M9X_PROFILE=40`, **`duration=5`** on every
benchmark.  **33 of 33 results and rc=0 in both arms, 0 stream timeouts**,
and `texture`, `texpair`, `rtt300`, `vary2tex`, `vssin` and `vsbool` each
0 of 76800 pixels different before and after the whole sweep.  The fixture
suite alongside it is **291 cases, 291 PASS, 0 FAIL, 0 non-M9** on a freshly
booted card, which includes the eight new vertex cases.

The library is the ninth standing's plus two changes: `fsin`/`fcos` compile
natively by default, and the vertex subtract no longer swaps its two operand
fields, which fixes every register comparison, every complement of a
condition and both halves of a two-register minimum and maximum
(`docs/parhelia-m9-vertex-isa-gaps.md` section 8).  The arms are
**`M9X_VS_SINCOS=1`**, what ships now, and **`M9X_VS_SINCOS=0`**, the
control.

This sweep is a **guard, not an expectation**.  One glmark2 vertex shader
uses a sine -- `jellyfish.vert`, twice -- and no other does, so the two arms
should agree almost everywhere, and they do.

| benchmark | sincos=1 fps | sincos=0 fps |
|---|---:|---:|
| `build` use-vbo=false | 279 | 279 |
| `build` use-vbo=true | 300 | 300 |
| `texture` nearest / linear / mipmap | 292 / 291 / 291 | 292 / 291 / 291 |
| `shading` gouraud | 248 | 248 |
| `shading` blinn-phong-inf | 154 | 153 |
| `shading` phong / cel | 98 / 91 | 98 / 91 |
| `bump` high-poly / normals / height | 135 / 185 / 164 | 135 / 185 / 164 |
| `effect2d` 3x3 / 15-tap | 87 / 34 | 87 / 34 |
| `pulsar` | 224 | 222 |
| `desktop` blur / shadow | 47 / 128 | 47 / 128 |
| `buffer` map | 155 | 155 |
| `buffer` subdata | 139 | 155 |
| `buffer` interleaved map | 155 | 155 |
| `ideas` | 108 | 108 |
| `jellyfish` | 76 | 75 |
| `terrain` | 1 | 1 |
| `shadow` | 118 | 118 |
| `refract` | 8 | 8 |
| `conditionals` fs0:vs0 / fs5:vs0 / fs0:vs5 | 234 / 144 / 233 | 234 / 144 / 233 |
| `function` low / medium | 171 / 73 | 171 / 73 |
| `loop` no-loop / uniform=false / uniform=true | 171 / 171 / 163 | 171 / 171 / 163 |

**Thirty of the thirty-three are equal to the frame rate.**  `jellyfish`, the
one benchmark with a vertex sine, is 76 against 75; `shading
blinn-phong-inf` and `pulsar` differ by one and two on shaders with no sine
in them, which is this sweep's noise.

One row read oddly in the sweep and was chased down: **`buffer` subdata, 139
with the sine on against 155 with it off**, where the other two `buffer` rows
were 155 in both arms and all three had been 136 in the ninth standing.  Its
vertex shader contains no sine, so the knob cannot reach it.  MEASURED
afterwards on the same card, the three `buffer` benchmarks **alone**, two
passes an arm in one X session:

```text
pass 1 sincos=1: 136 136 136      pass 1 sincos=0: 136 136 136
pass 2 sincos=1: 136 136 136      pass 2 sincos=0: 136 136 136
```

All four passes agree with each other and with the ninth standing, so the
139 and the 155s are what these three rows do **after seventeen other
benchmarks in the same `glmark2` process**, not what the knob does.  Read the
`buffer` rows of a full-run sweep with that in mind.

Nothing regressed, nothing that moved lost frame rate, and the sine costs
nothing where it is not used.

## Standing 2026-09-20 (ninth): eighteen of the thirty-three run every vertex on the engine

MEASURED on the M9128 at 1920x1200 on a **freshly rebooted card**, module
`/root/tmp/dxtmip-m9x_drm.ko` named explicitly, **one X server and one
`glmark2` invocation an arm** against one deployed library (md5 `22eff4de`)
carrying all three arms behind its knobs, both hardware lock files held
across the whole cycle, `M9X_SKIP_REPORT=1`, `M9X_PROFILE=40`,
**`duration=5`** on every benchmark.  33 of 33 results and rc=0 in every arm,
**0 stream timeouts**, and `texture`, `texpair`, `rtt300` and `vary2tex`
exact before and after the whole sweep.

Three arms of the same library, in this order:

* **default** -- what ships: the vertex program is specialised on its
  inlinable uniforms, and `M9X_VS_MAX_SLOTS` is 128;
* **off** -- `M9X_VS_SPECIALISE=0 M9X_VS_MAX_SLOTS=80`, which is the reach
  the build before this one had, and is the control;
* **4096** -- `M9X_VS_MAX_SLOTS=4096`, no program-length bound at all, which
  is what the bound is there to prevent.

| benchmark | off fps | off native/module | default fps | default native/module | 4096 fps | 4096 native/module |
|---|---:|---|---:|---|---:|---|
| `build` use-vbo=false | 229 | 1117/0 | 229 | 1117/0 | 229 | 1117/0 |
| `build` use-vbo=true | 285 | 1437/0 | 286 | 1437/0 | 285 | 1437/0 |
| `texture` nearest | 290 | 1437/0 | 289 | 1437/0 | 290 | 1437/0 |
| `texture` linear | 289 | 1437/0 | 289 | 1437/0 | 289 | 1437/0 |
| `texture` mipmap | 289 | 1477/0 | 289 | 1437/0 | 289 | 1477/0 |
| `shading` gouraud | 231 | 1117/0 | 231 | 1157/0 | 231 | 1117/0 |
| `shading` blinn-phong-inf | 147 | 757/0 | 147 | 757/0 | 147 | 757/0 |
| `shading` phong | 95 | 477/0 | 95 | 477/0 | 95 | 477/0 |
| `shading` cel | 88 | 437/0 | 88 | 437/0 | 88 | 437/0 |
| `bump` high-poly | 113 | 557/0 | 113 | 557/0 | 113 | 557/0 |
| `bump` normals | 183 | 917/0 | 183 | 917/0 | 183 | 917/0 |
| `bump` height | 161 | 16/782 | 161 | 11/787 | 161 | 16/782 |
| `effect2d` 3x3 | 79 | 0/397 | 86 | 0/437 | 81 | 0/397 |
| `effect2d` 15-tap | 33 | 0/157 | 33 | 0/157 | 33 | 0/157 |
| `pulsar` | 219 | 5450/27 | 219 | 5290/19 | 219 | 5395/38 |
| `desktop` blur | 42 | 1185/1561 | 42 | 1392/1617 | 42 | 1174/1473 |
| `desktop` shadow | 128 | 16271/132 | 128 | 16411/76 | 128 | 16051/220 |
| `buffer` map | 136 | 416/661 | 136 | 234/668 | **101** | 1192/0 |
| `buffer` subdata | 136 | 0/677 | 136 | 0/677 | **100** | 477/0 |
| `buffer` interleaved map | 136 | 0/677 | 136 | 0/677 | **99** | 517/0 |
| `ideas` | 103 | 62158/82958 | 103 | 62649/84805 | 103 | 62735/86605 |
| `jellyfish` | 73 | 1369/7068 | 74 | 1047/4933 | 74 | 863/3713 |
| `terrain` | 1 | 0/0 | 1 | 0/0 | 1 | 0/0 |
| `shadow` | 106 | 1079/672 | 106 | 1083/672 | 106 | 1005/632 |
| `refract` | 4 | 0/0 | 4 | 0/0 | 4 | 78/39 |
| `conditionals` fs 0:vs 0 | 226 | 1119/125 | 226 | 1116/122 | 226 | 1115/120 |
| `conditionals` fs 5:vs 0 | 141 | 717/0 | 141 | 717/0 | 141 | 717/0 |
| `conditionals` fs 0:vs 5 | 226 | 1117/0 | 226 | 1117/0 | 226 | 1117/0 |
| `function` low | 166 | 837/0 | 166 | 837/0 | 166 | 837/0 |
| `function` medium | 72 | 357/0 | 72 | 357/0 | 72 | 357/0 |
| `loop` no-loop | 161 | 22/775 | **166** | 837/0 | 166 | 837/0 |
| `loop` uniform=false | 161 | 0/797 | **166** | 837/0 | 166 | 837/0 |
| `loop` uniform=true | 154 | 0/797 | **159** | 797/0 | 159 | 797/0 |

**Fifteen of the thirty-three are fully native in the off arm and eighteen
with the default** -- a plain count of the benchmarks whose every counted
draw took the compiled vertex program, nothing rounded up.  The three that
moved are the three `loop` benchmarks, and all three moved for one reason:
`vertex-loop` and `vertex-uniform` both default to true in glmark2, so every
one of them hands the driver a loop whose bound is the uniform `VertexLoops`.
`docs/parhelia-m9-uniform-specialisation.md` carries that, and it is the same
mechanism the fragment stage already used, not a branch unit.

**Read the frame rates against the control in the same sweep.**  The thirty
benchmarks that take the **identical path** in the default and off arms are
the control, and this sweep is unusually quiet: twenty-seven of them are
equal to the frame rate, `build use-vbo=true` and `texture nearest` differ by
one, `jellyfish` by one, and the single loud row is `effect2d 3x3` at 1.089.
Their default/off ratio averages **1.003**.  The three rows that moved onto
the engine sit at **1.031, 1.031 and 1.032** -- above the control, not inside
its noise.  **Nothing that moved lost frame rate, and the rows that moved
gained about three per cent.**

The 4096 arm is why the default caps the program length.  It takes a
twenty-first benchmark set fully native -- the three `buffer` rows -- and
each of them **loses about a quarter of its frame rate**: 136 to 101, 100 and
99 against a control that is otherwise equal row for row.  glmark2's buffer
vertex shader is four clip-space transforms and three edge distances, 299
slots where every program the engine already wins on is at most 79.

**What makes the long program lose is now measured**
(`docs/parhelia-m9-vertex-isa-gaps.md` section 1.8): the engine's own vertex
stage costs about twice as much per vertex past a step between 186 and 191
slots, and at 299 slots it costs more than the entire host transform it
replaces.  MEASURED at 1920x1200 on the three `buffer` rows in one
invocation an arm, `M9X_PROFILE=200`: the draw-module arm is 6.40 ms a frame
with `draw 2.96 emit 1.92`, and the native arm is **10.03 ms** with **`draw
0.00 emit 0.00`**.  The host half goes to zero and the frame still gets 3.6 ms
slower.  `M9X_VS_MAX_SLOTS` is therefore **188**, the highest length measured
to run below that step -- a cost bound, not a correctness one: the engine
renders a 5161-slot program exactly.

That shader only reaches the backend at all because a register held by a dead
intermediate is now given back (`parhelia-m9-native-vertex-backend.md`
section 1): before that the allocator ran out with 28 of its 29 registers
held by intermediates of instructions that had long since finished, and the
`buffer` and `bump:height` shaders were refused as "out of temporaries".
`bump:height` compiles now too, but its draws still fall back for a reason
further down the path -- it writes four varyings.

**That run did not fail for its length.**  MEASURED on 2026-09-20 with the
vertex sine on: `M9X_VS_MAX_SLOTS=4096` lets ioquake3's eight
`deformVertexes` programs run, and the engine answers `two-pass job failed
(Input/output error); context lost`, after which the game renders nothing and
reports 380 fps of it, and the run leaves a kernel stream timeout behind.
The resource those programs exhaust is the **constant span**, not the
program store: the engine takes 63 constant pairs and stops at 65, and those
eight need 68 to 82.  The driver bounds the span as well now
(`M9_VS_MAX_CONST_PAIRS`), which is what keeps a raised length bound from
reaching that configuration -- `docs/parhelia-m9-vertex-isa-gaps.md`
sections 1.7 to 1.9.

## Standing 2026-09-19 (eighth): the textured draws run on the engine

MEASURED on the M9128 at 1920x1200 on a freshly rebooted card, module
`/root/tmp/dxtmip-m9x_drm.ko` named explicitly, **one X server and one
`glmark2` invocation an arm** against one deployed library (md5 `08a16d74`)
carrying both arms behind its knobs, both hardware lock files held across
the whole cycle, `M9X_SKIP_REPORT=1`.  33 of 33 results and rc=0 in every
arm, **0 stream timeouts**, and `texture`, `texpair`, `rtt300` and
`vary2tex` exact before and after each sweep.

Two sweeps of the same library, the **default arm first** in both:

* the table below, glmark2's thirty-three at **`duration=5`** with
  `M9X_PROFILE=40`, which is the standard here now -- half glmark2's own
  10 s, and a window small enough that every benchmark but `terrain` still
  reaches one and keeps its native count;
* a second sweep at glmark2's own default duration with
  `M9X_PROFILE=400`, further down, which agrees row for row.

Two arms: **default** is what ships (`M9X_VS_TEXTURE=1`), **off** is
`M9X_VS_TEXTURE=0`, every sampling draw back in the draw module, which is
what shipped before.

| benchmark | off fps | off native/module | default fps | default native/module |
|---|---:|---|---:|---|
| `build` use-vbo=false | 239 | 1157/0 | 239 | 1157/0 |
| `build` use-vbo=true | 302 | 1517/0 | 302 | 1517/0 |
| `texture` nearest | 304 | 27/1490 | 305 | 1517/0 |
| `texture` linear | 303 | 0/1517 | 305 | 1557/0 |
| `texture` mipmap | 303 | 0/1517 | 305 | 1517/0 |
| `shading` gouraud | 241 | 1175/22 | 240 | 1197/0 |
| `shading` blinn-phong-inf | 151 | 757/0 | 139 | 677/0 |
| `shading` phong | 96 | 477/0 | 96 | 477/0 |
| `shading` cel | 89 | 437/0 | 89 | 437/0 |
| `bump` high-poly | 115 | 597/0 | 115 | 597/0 |
| `bump` normals | 186 | 8/909 | 189 | 917/0 |
| `bump` height | 166 | 0/838 | 166 | 35/803 |
| `effect2d` 3x3 | 88 | 0/437 | 88 | 0/437 |
| `effect2d` 15-tap | 34 | 0/157 | 34 | 0/157 |
| `pulsar` | 232 | 5690/19 | 230 | 5605/36 |
| `desktop` blur | 43 | 100/2493 | 43 | 1401/1689 |
| `desktop` shadow | 130 | 0/17191 | 130 | 16511/36 |
| `buffer` map | 139 | 0/852 | 139 | 364/663 |
| `buffer` subdata | 123 | 0/637 | 108 | 0/557 |
| `buffer` interleaved map | 139 | 0/677 | 140 | 0/677 |
| `ideas` | 105 | 63250/83348 | 105 | 62733/79993 |
| `jellyfish` | 75 | 1547/8023 | 75 | 2099/11683 |
| `terrain` | 1 | 0/0 | 1 | 0/0 |
| `shadow` | 109 | 1023/636 | 109 | 1096/672 |
| `refract` | 4 | 70/62 | 4 | 0/0 |
| `conditionals` fs 0:vs 0 | 234 | 1157/91 | 234 | 1157/123 |
| `conditionals` fs 5:vs 0 | 144 | 717/0 | 144 | 717/0 |
| `conditionals` fs 0:vs 5 | 234 | 1157/0 | 234 | 1197/0 |
| `function` low | 171 | 877/0 | 171 | 837/0 |
| `function` medium | 72 | 357/0 | 72 | 357/0 |
| `loop` no-loop | 166 | 15/822 | 166 | 25/812 |
| `loop` uniform=false | 166 | 0/797 | 166 | 0/837 |
| `loop` uniform=true | 158 | 0/797 | 158 | 0/757 |

Two rows of that table read low in the default arm -- `shading
blinn-phong-inf` 151 -> 139 and `buffer subdata` 123 -> 108 -- and **neither
changed path**: both are wholly native, respectively wholly draw module, in
both arms, so nothing the default does can reach them.  They are this
sweep's noise floor, and the 10 s sweep has them at 144/144 and 141/138.

**Ten of the thirty-three are fully native in the off arm and fifteen with
the default** -- a plain count of the benchmarks whose every counted draw
took the compiled vertex program, nothing rounded up, and the same ten and
fifteen in both sweeps.  The five that moved are `texture` x3,
`shading:gouraud` and `bump:normals`.

**Read the frame rates against the control in the same sweep.**  A whole-arm
drift of a few per cent is real on this box, so the seventeen benchmarks
that take the **identical path in both arms** -- wholly native or wholly
draw module in each -- are the control: their default/off ratio averages
**0.989** at `duration=5`, its own worst row 0.878.  Every benchmark whose
path *changed* is at or above **0.991**: `texture` 1.003 to 1.007,
`bump:normals` 1.016, `desktop:shadow` 1.000, `ideas` 1.000, `shadow`
1.000, `jellyfish` 1.000, `pulsar` 0.991, `gouraud` 0.996.  **Nothing that
moved onto the engine lost frame rate.**

The 10 s sweep says the same with the drift larger: its eighteen unchanged
benchmarks average 0.989 spanning 0.966 to 1.000, and every changed row
sits inside that band or above it -- `texture` 1.003, `bump:normals` 1.016,
`desktop:shadow` 0.985, `shadow` 0.973.

<details><summary>the same two arms at glmark2's own default duration</summary>

| benchmark | off fps | off native/module | default fps | default native/module |
|---|---:|---|---:|---|
| `build` use-vbo=false | 231 | 1997/0 | 231 | 1997/0 |
| `build` use-vbo=true | 288 | 2797/0 | 288 | 2797/0 |
| `texture` nearest | 290 | 390/2807 | 291 | 3197/0 |
| `texture` linear | 290 | 0/2797 | 291 | 2797/0 |
| `texture` mipmap | 290 | 0/2797 | 291 | 2797/0 |
| `shading` gouraud | 231 | 2105/292 | 231 | 2397/0 |
| `shading` blinn-phong-inf | 144 | 1597/0 | 144 | 1597/0 |
| `shading` phong | 93 | 797/0 | 93 | 797/0 |
| `shading` cel | 86 | 797/0 | 86 | 797/0 |
| `bump` high-poly | 114 | 1197/0 | 114 | 1197/0 |
| `bump` normals | 182 | 170/1427 | 185 | 1997/0 |
| `bump` height | 164 | 0/1998 | 164 | 46/1552 |
| `effect2d` 3x3 | 87 | 0/797 | 87 | 0/797 |
| `effect2d` 15-tap | 34 | 0/397 | 34 | 0/397 |
| `pulsar` | 226 | 9810/35 | 227 | 9525/92 |
| `desktop` blur | 43 | 1470/1457 | 43 | 2031/329 |
| `desktop` shadow | 130 | 0/37667 | 128 | 33931/3052 |
| `buffer` map | 141 | 0/1747 | 138 | 1534/1138 |
| `buffer` subdata | 141 | 0/1597 | 138 | 0/1597 |
| `buffer` interleaved map | 141 | 0/1197 | 138 | 0/1197 |
| `ideas` | 109 | 129310/143014 | 107 | 130314/165267 |
| `jellyfish` | 77 | 7007/41973 | 77 | 3281/17268 |
| `terrain` | 1 | 0/0 | 1 | 0/0 |
| `shadow` | 110 | 2276/1376 | 107 | 1570/970 |
| `refract` | 4 | 0/0 | 4 | 0/0 |
| `conditionals` fs 0:vs 0 | 235 | 2409/254 | 227 | 2691/529 |
| `conditionals` fs 5:vs 0 | 144 | 1197/0 | 141 | 1597/0 |
| `conditionals` fs 0:vs 5 | 235 | 2397/0 | 227 | 1997/0 |
| `function` low | 172 | 1597/0 | 167 | 1597/0 |
| `function` medium | 73 | 797/0 | 72 | 797/0 |
| `loop` no-loop | 167 | 237/1360 | 162 | 263/1334 |
| `loop` uniform=false | 167 | 0/1597 | 162 | 0/1597 |
| `loop` uniform=true | 160 | 0/1597 | 155 | 0/1597 |

</details>

### Three things changed, and each was measured on its own

**1. A coordinate's coefficients moved out of the per-job constant span.**
They were constant pairs the driver filled per job, so a draw that sampled a
differently addressed render target ended the job.  They now ride a
source-record slot -- one more declared attribute, four floats the record
writer puts in every record of the draw -- and records are per draw already
(`docs/parhelia-m9-native-vertex-backend.md` section 4a).  MEASURED on
`desktop:shadow`, whose four windows are render targets of a width the
descriptor rounds up to 32:

| | jobs a frame | uniform splits a frame | fps |
|---|---:|---:|---:|
| coefficients in the constant span | **16.00** | **8.25** | 124 |
| coefficients in the source record | **6.67** | **0.00** | 131 |
| draw module, for comparison | 5.67 | 0.00 | 129 |

**2. A sampler reads its pair out of the varying's own stream.**  The raster
declaration word's low twelve bits are four component selectors, and nothing
captured ever uses a non-zero first one.  The hardware accepts it: a texture
coordinate attribute declared over the stream slot an interpolated varying
already uses, with selectors `c | (c+1) << 3 | 6 << 6 | 6 << 9`, reads that
varying's components `c` and `c+1`.  MEASURED through the driver's own
pixel-exact fixtures, six of them -- `texture`, `texpair`, `vary2tex`,
`vary2blend`, `rtt300`, `texaff` -- each **0 of 76800 pixels different**
with sharing on, exactly as with a coordinate stream of its own
(`docs/parhelia-m9-raster-stream-limit-re.md`).

That is what `texture` needed.  The scene links its sampled pair at
components 1 and 2 of a varying the fragment program also interpolates, so
the coordinate used to cost a second output target and with it the clip
position; now it costs nothing and the pass carries one stream.  MEASURED at
1920x1200, one hold, one library:

| `texture` linear | fps | ioctl ms a frame |
|---|---:|---:|
| draw module | 307 | 3.171 |
| native, coordinate stream of its own | 288 | 3.414 |
| native, shared with the varying | **308** | 3.174 |

**3. What the separate stream costs, for the record.**  Before the sharing
route existed, the two-stream shape cost `texture` frame rate and it was not
the second output target as a fixed per-pass cost: the job count is **2 a
frame in both arms at every size** and the CPU half is 0.00 on the native
arm, while the excess is in the GPU half and scales with the rendered area
(MEASURED over four sizes, one benchmark a size):

| size | megapixels | off, ioctl ms | native, ioctl ms | excess |
|---|---:|---:|---:|---:|
| 240x150 | 0.036 | 0.123 | 0.154 | +0.031 |
| 480x300 | 0.144 | 0.314 | 0.360 | +0.046 |
| 960x600 | 0.576 | 0.929 | 1.037 | +0.108 |
| 1920x1200 | 2.304 | 3.333 | 4.142 | +0.809 |

`0.026 ms + 0.139 ms a megapixel` fits the first three to a thousandth.  A
coordinate stream that cannot share -- because it scales, or because its
varying is not one the pass interpolates -- still takes that second target,
so it stays refused behind `M9X_VS_COORD_PAIR=1`.

### The engine does not act on the cull bits (MEASURED)

Both paths state the same cull bits in index `ef`, and **clearing them
changes nothing**: `M9X_NO_CULL=1` on three native scenes at 1920x1200 moves
neither the frame rate nor the ioctl phase by more than the noise --
`bump:normals` 191 fps and 5.125 ms against 191 and 5.130, `texture` 288 and
3.417 against 289 and 3.404, `build` 322 and 2.856 against 323 and 2.840.
If the engine were removing back-facing triangles, taking the bits away
would have made it rasterise them.

That matters because the draw module removes them on the **CPU** before it
hands a draw over and the native path does not: `texture`'s cube reaches the
engine as 22 source records a frame through the draw module -- varying
between 16 and 40 with the rotation -- and as a fixed 40 natively.  So every
native draw of a closed mesh rasterises its hidden half.  Why index `ef`'s
cull field is inert is **OPEN**, and it is the next thing to settle: it
would pay on every native scene, not only the textured ones.

## Standing 2026-09-19 (seventh): the second varying moves two scenes onto the engine

MEASURED at 1920x1200 on the M9128, glmark2's default thirty-three, one
process a benchmark in one X session a sweep, `M9X_SKIP_REPORT=1`,
`M9X_PROFILE=400`, **0 stream timeouts**, both hardware lock files held
across the whole cycle, and the m9-mesa-glsl suite run in the same hold with
its usual sixteen failures and nothing else.

The native/draw-module column is the profile line's own count of draws the
compiled vertex program took against draws the Gallium draw module
transformed on the CPU, summed over the sweep.  `-` is a benchmark whose
frame count never reached a profile window.

| benchmark | cap 1 fps | cap 1 native/module | cap 2 fps | cap 2 native/module |
|---|---:|---|---:|---|
| `build` vbo=false / vbo=true | 274, 295 | 2397/0, 2797/0 | 274, 294 | 2397/0, 2797/0 |
| `texture` nearest / linear / mipmap | 284, 283, 284 | 0/2797 each | 284, 283, 284 | 0/2797 each |
| `shading` gouraud | 241 | 2397/0 | 241 | 2397/0 |
| `shading` blinn-phong-inf | 149 | 1197/0 | 149 | 1197/0 |
| **`shading` phong** | 89 | **0/797** | **95** | **797/0** |
| **`shading` cel** | 83 | **0/797** | **88** | **797/0** |
| `bump` high-poly / normals / height | 135, 182, 165 | 1197/0, 0/1597, 0/1598 | 135, 182, 165 | same |
| `effect2d` 3x3 / 15-tap | 85, 33 | 0/797, - | 85, 33 | same |
| `pulsar` | 220 | 9985/0 | 220 | 9985/0 |
| `desktop` blur / shadow | 44, 123 | 0/5573, 0/31155 | 44, 123 | same |
| `buffer` map / subdata / interleaved | 154, 154, 154 | 0/1197 each | 154, 154, 154 | same |
| `ideas` | 109 | 120627/99452 | 109 | 120951/98876 |
| `jellyfish` | 77 | 397/398 | 77 | 397/398 |
| `terrain` | 1 | - | 1 | - |
| `shadow` | 118 | 1594/797 | 118 | 1594/797 |
| `refract` | 8 | - | 8 | - |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | 1997/0, 1197/0, 1997/0 | 229, 142, 228 | same |
| `function` low / medium | 169, 72 | 1597/0, 397/0 | 169, 72 | same |
| `loop` no-loop / uniform=false / uniform=true | 167, 167, 159 | 0/1597, 0/1597, 0/1197 | 167, 167, 159 | same |

**`shading:phong` and `shading:cel` are the whole delta**, and they are
complete: every draw of both moves from the draw module to the compiled
vertex program, and both gain about 6 fps.  Nothing else changes by more
than 1 fps in either direction, and no benchmark loses a draw.

### What still keeps the rest on the CPU, by its own words

The driver now tallies its refusals by reason.  Across the thirty-three:

* **the fragment program samples a texture** -- `texture` x3, `bump:normals`,
  `bump:height`, `effect2d` x2, `desktop` x2, `shadow` in part, `terrain`,
  `refract`, `jellyfish` in part.  This, and not the varying cap, was what
  held most of the list on the build this standing measures; the eighth
  standing above is what changed it.
* **control flow** -- `loop` x3 and `buffer` x3, whose vertex shaders the
  native backend does not compile.
* **the varying read is past the M9X_VS_VARYINGS cap** -- `ideas` in part,
  `jellyfish` in part, `terrain`, `refract`, `bump:height`: three varyings
  and up, which one preprocessing draw cannot carry
  (`docs/parhelia-m9-vertex-output-target-re.md`).

### The texture coordinate: built, measured, and off

Both obstacles the earlier experiment named -- "the texture coordinate is
not the varying's first pair" and "the texture coordinate needs a
per-vertex span scale" -- are now the vertex program's own work.  A
coordinate stream is an output target of its own carrying
`(V[c], V[c+1]) * scale + bias` in one captured multiply-add, whose two
constant pairs the driver fills per job with the sampler's transform times
the render target's addressed span
(`docs/parhelia-m9-native-vertex-backend.md` section 4a).

**It is correct and, as it stood that day, it did not pay.**  What it cost
and why is the eighth standing below, which also carries the fix and the
default the driver ships now; the numbers in this seventh standing are the
build *before* that fix and are kept as the measurement they were.

MEASURED on the M9128, 2026-09-19, one hold of both hardware lock files,
module `/root/tmp/dxtmip-m9x_drm.ko` named explicitly, glmark2's default
thirty-three at 1920x1200 in **one invocation an arm** of the same
deployed library (md5 `fd11173c`), `M9X_SKIP_REPORT=1 M9X_PROFILE=400`,
33 of 33 results and rc=0 both arms, **0 stream timeouts**, and `texture`,
`texpair`, `rtt300` and `vary2tex` exact before and after.  The X server
ran the off arm throughout, so what moves below is the benchmark client's
own draws.

| benchmark | off fps | off native/module | on fps | on native/module |
|---|---:|---|---:|---|
| `build` vbo=false / vbo=true | 280, 303 | 2797/0, 2797/0 | 281, 303 | unchanged |
| **`texture`** nearest / linear / mipmap | 292, 292, 292 | 226/2571, 0/3197, 0/2797 | **234, 234, 233** | **2397/0 each** |
| `shading` gouraud | 247 | 2221/176 | 247 | 2397/0 |
| `shading` blinn-phong-inf / phong / cel | 151, 95, 88 | 1597/0, 797/0, 797/0 | 151, 95, 88 | unchanged |
| `bump` high-poly | 137 | 1597/0 | 137 | 1597/0 |
| **`bump` normals** | 185 | 151/1446 | **186** | **1597/0** |
| `bump` height | 167 | 0/1998 | 167 | 269/1329 |
| `effect2d` 3x3 / 15-tap | 88, 34 | 0/797, 0/397 | 88, 34 | still refused |
| `pulsar` | 227 | 9595/78 | 227 | 10235/347 |
| **`desktop`** blur | 47 | 1730/6329 | 47 | 2169/1433 |
| **`desktop`** shadow | 128 | 0/30927 | **124** | **25311/2340** |
| `buffer` map / subdata / interleaved | 156 each | 0/4147, 0/1597, 0/1197 | 156 each | 8476/1271, unchanged, unchanged |
| `ideas` | 108 | 121170/103791 | 109 | 129698/157793 |
| `jellyfish` | 77 | 13217/83148 | 78 | 5075/29163 |
| `terrain` / `refract` | 1, 8 | -, - | 1, 8 | -, - |
| `shadow` | 119 | 2138/1406 | 119 | 2319/1399 |
| `conditionals` 0/0, 0/5, 5/0 | 234, 144, 233 | 2636/782, 1597/0, 2397/0 | 234, 147, 241 | unchanged |
| `function` low / medium | 171, 73 | 1597/0, 797/0 | 175, 74 | unchanged |
| `loop` no-loop / uniform=false / uniform=true | 169, 169, 162 | 66/1531, 0/1597, 0/1597 | 174, 174, 166 | still refused |

Two caveats on the counts.  The profile window is 400 frames and its
counters reset at every print, so a scene is charged the windows that
closed while it ran and a boundary can carry up to one window of the
previous scene -- which is what `texture:nearest`'s 226 in the off arm is.
And `terrain` and `refract` never reach a window at all.

**Where the frame rate goes, from the profile's own phases:**

* `texture` -- the CPU half goes to **zero**, `draw 0.58` and `emit 0.57` ms
  a frame become `0.00`, and the GPU half **rises**, `ioctl 3.35` ->
  `4.2..4.6` ms a frame.  The scene draws forty vertices and was already
  GPU-bound at 3.4 ms a frame, so there was no CPU time to win and the
  engine now does more.  Why it does more is **not established**: the
  preprocessing pass runs a compiled 40-slot transform instead of the
  pass-through and binds two output targets instead of one, but neither has
  been isolated.  `bump:normals`, textured and native on a **single**
  stream, gains rather than loses, which points at the second target.
* `desktop:shadow` -- the CPU half falls by **7.5 ms a frame** to zero, and
  the job count triples, 5..6 -> 16 a frame, on **eleven uniform splits a
  frame where there were none**.  Each window is a render target with its
  own addressed span, the coefficients live in the vertex program's per-job
  constant span, and a draw that changes that span ends the job.  This is
  the same mechanism that cost `ideas` and `pulsar` when the native path
  first landed (`parhelia-m9-native-vertex-backend.md` 8.3), reached by a
  new route.

**The pictures are right.**  `texpair` -- a float varying ahead of the
coordinate, so the sampled pair lands at components 1 and 2 of one varying
slot, exactly what `texture` links to -- is 0 of 76800 pixels different
against its CPU reference, and so are `rtt300` and `rtt:64x23`, a 300-wide
and a 64x23 render target addressed over a 32-aligned span.  Screenshots of
`texture:texture-filter=linear` on both arms and from llvmpipe show the same
crate with the same mapping and the same lighting (the rotation phase
differs because the frame rates do, which is why the pixel-exact fixture is
the evidence and the screenshot is the corroboration).

**What would make it pay**, in the order the measurements point:

1. Get the coefficients out of the per-job constant span, or out of the job
   key, so a per-draw span stops ending the job.  That is `desktop`'s whole
   cost and it is the larger of the two.
2. Explain the GPU-side cost of `texture`.  The controlled comparison is
   already in the table: one stream gains, two lose.  Removing the second
   target is possible without any vertex-program change if the **raster
   declaration's selectors** can place the pair -- the field is
   `s0 | s1 << 3 | s2 << 6 | s3 << 9` and the coordinate form `0x0688` is
   selectors 0,1,2,3 -- which would let the coordinate share the varying's
   own stream.  No captured raster declaration uses a non-zero `s0`, so it
   needs a probe of its own; `tools/m9-user-raster-streams.c` is where it
   goes.

### A split fragment program on the native path is refused by the hardware

Also MEASURED, 2026-09-19, and also left off (`M9X_VS_SPLIT`).  A shader the
engine runs as several raster passes can express its streams as the union
over the passes -- for glmark2's `shadow`, whose first pass computes the
coordinate its second takes from the inter-pass record, that union is a
single varying stream, and `m9_job_native` already loops over the passes.
Built and run, **the engine draws nothing**: the `coordroute` fixture, the
two-pass shape `shadow` compiles to, takes the native path and differs in
76800 of 76800 pixels, with no stream timeout logged and the card alive
afterwards.  The next probe is the submitted stream of a two-pass native
job decoded against the same job through the draw module.

## Standing 2026-09-19 (sixth): the whole default run in ONE invocation

MEASURED at 1920x1200 on the M9128, **`glmark2 --size 1920x1200` with no
`-b` at all -- one process, one GL context, glmark2's own default
thirty-three in glmark2's own order** -- `M9X_SKIP_REPORT=1`, exit code
**0**, **33 of 33 results**, **0 stream timeouts**, the known-good `texture`
fixture passing **before and after**, both hardware lock files held across
the whole cycle.  **Not one "draws skipped" line in any of the thirty-three.**
`tools/m9x-glmark-default-suite.sh` runs it this way now; the
one-server-a-benchmark control stays at
`tools/m9x-glmark-default-suite-fresh-x.sh`.

| benchmark | fps, one invocation | standing before, one process a benchmark |
|---|---:|---:|
| `build` vbo=false / vbo=true | 274, 295 | 274, 296 |
| `texture` nearest / linear / mipmap | 284, 284, 284 | 285, 284, 285 |
| `shading` gouraud / blinn-phong-inf / phong / cel | 242, 149, 89, 83 | 242, 149, 89, 83 |
| `bump` high-poly / normals / height | 135, 182, 165 | 135, 183, 165 |
| `effect2d` 3x3 / 15-tap | 84, 33 | 84, 33 |
| `pulsar` | 222 | 221 |
| `desktop` blur / shadow | 44, 124 | 44, 125 |
| `buffer` map / subdata / interleaved | 154, 154, 154 | 154, 154, 154 |
| `ideas` | 111 | 111 |
| `jellyfish` | 77 | 77 |
| `terrain` | 1 (1835 ms) | 1 (1831 ms) |
| `shadow` | 118 | 118 |
| `refract` | 8 (131 ms) | 8 (132 ms) |
| `conditionals` 0/0, 0/5, 5/0 | 230, 143, 229 | 230, 143, 229 |
| `function` low / medium | 169, 72 | 169, 72 |
| `loop` no-loop / uniform=false / uniform=true | 167, 167, 160 | 167, 167, 160 |

Every row is within one frame a second of the standing below, which is the
control this is read against.  Running the list in one process changes no
frame rate: the per-benchmark sweep was never measuring anything different,
it was only working around the crash below.

Offline gates from the build directory they were built in, exit codes read
rather than last lines (`tools/m9-offline-gates.sh`): `m9-batch-test` rc=0,
`m9-vs-compile-test` rc=0, `m9-fs-compile-test` rc=0.  Fixture suite on the
same library, on a freshly rebooted card: **263 cases, 247 PASS, 16 FAIL, 0
non-M9, 0 stream timeouts** -- the same sixteen as the standing below, the
ten `dxt*`, the four `giga10*` and `mrt10:2`/`mrt10:4`.  The deployed library
is byte-identical to a second build of the same tree (md5
`6e8cfd2b081c42c349276120dd6e21b9`, verified on the target before the run).

`tools/m9x-glmark-default-suite.sh` was then run as a script, against the
same library: **33 scenes, rc=0, 0 stream timeouts**, 29 at or above 60 fps
and the four below being the 15-tap `effect2d`, `desktop` blur, `terrain` and
`refract`; `texture` passing after it.

### A long session costs the run after it, and it is not a stream timeout

CAPTURED once, worth naming because it looks exactly like a driver fault: the
same script run after the 263-case fixture suite, without a reboot in
between, lost **28 draws to "job failed, context lost"** and died at the
twentieth benchmark on an X `BadValue`, with **0 stream timeouts** in dmesg.
What dmesg did carry is `m9x: no contiguous DMA32 for 5529600 bytes;
scattered pages`.  After a reboot the identical command is the clean 33 above.
So a benchmark run that follows a long GL session is not a control for
anything; reboot first.

### The rc=139 between scenes was ours, and what it was

CAPTURED under gdb, the pre-fix library, one invocation: the client takes
**SIGSEGV after the fourteenth result** -- the 15-tap `effect2d` -- and every
frame of the stack is inside the driver:

```
#0  ralloc_free ()            libgallium
#1  m9_render_destroy ()      libgallium
#2  vbuf_destroy ()           libgallium
#3  draw_destroy ()           libgallium
#4  m9_context_destroy ()     libgallium
#5  st_destroy_context_priv ()
...  MakeContextCurrent
```

glmark2 destroys the scene's GL context when it moves on, so the crash is at
`glXMakeContextCurrent` time, which is why it reads as "moving between
scenes".  The minimal pair is the 15-tap `effect2d` followed by **`pulsar`**
-- the next benchmark in the default order -- and it reproduces in about
forty seconds (CAPTURED); the backlog's "to `shadow`" named the next
benchmark of a `-b` sweep, not of the default list.

The fault is a **fixed-size array**.  `m9_vbuf_render::extra_offset` was
declared `int extra_offset[M9_FS_MAX_VARYINGS + M9_FS_MAX_TEXTURES]`, the
CPU views of **one** fragment pass, and `struct util_dynarray indices`
followed it in the struct.  A split variant has a view per interpolated input
and per coordinate stream of *every* pass, so the emit loop wrote up to eight
entries past the array and into that dynarray's `data` and `mem_ctx`.  The
15-tap `effect2d` is such a split shader.  Nothing faults while the job runs;
the corrupted dynarray is freed at context teardown, and
`util_dynarray_fini()` then calls `ralloc_free()` on the garbage.

**Fixed in the Mesa m9x driver by `d14b711`** (2026-09-19 07:43), which sized
the array `M9_FS_MAX_EXTRA` as part of the terrain work -- that commit records
the same overflow surfacing the other way, in `ralloc_size` on the *next*
allocation during `m9_render_emit`.  DERIVED by bisecting the driver on the
minimal pair, one build a step, each deployed by hash: `22c0352` CRASH,
`d14b711` CLEAN, `98be636` CRASH, `900c8ad` CLEAN, and current main CLEAN over
the whole thirty-three.  It is **not** an upstream glmark2 bug, so no
workaround is kept.

Two measurement notes the bisect cost a run each to learn.  The driver's
"rendering needs kernel user submit (`m9_user_submit=1`) and firmware
`m9x/rsfragptrs-normal.bin`" message is also what `m9_probe_user_submit()`
prints when its **allocation** fails -- a card another client has filled, or
a module the box auto-loaded without the parameter -- so it does not by
itself mean the driver is too old for the module.  And a stalled engine makes
the `conv13`, `conv16` and `convrtt:*` fixtures fail all 76800 pixels; all of
them pass on a card that has just been rebooted, so that band is a state
signal, not a regression.

## Standing 2026-09-19 (fifth): what a split job's back-end clear cost

MEASURED at 1920x1200 on the M9128, glmark2's default thirty-three, **one
process a benchmark in one X session** -- which is sound again since the m9x
shared-handle table began closing what it opened, and the control run one
server a benchmark agrees -- `M9X_SKIP_REPORT=1`, **0 stream timeouts**, the
known-good `texture` fixture passing before and after, both hardware lock
files held across the whole cycle.  **Not one "draws skipped" line in any of
the thirty-three.**

| benchmark | fps | standing before |
|---|---:|---:|
| `build` vbo=false / vbo=true | 274, 296 | 273, 294 |
| `texture` nearest / linear / mipmap | 285, 284, 285 | 283, 282, 283 |
| `shading` gouraud / blinn-phong-inf / phong / cel | 242, 149, 89, 83 | 242, 149, 88, 82 |
| **`bump`** high-poly / normals / height | **135**, 183, 165 | **105**, 182, 164 |
| `effect2d` 3x3 / 15-tap | 84, 33 | 84, 33 |
| `pulsar` | 221 | 219 |
| **`desktop`** blur / shadow | **44**, 125 | **37**, 124 |
| `buffer` map / subdata / interleaved | 154, 154, 154 | 153, 153, 153 |
| `ideas` | 111 | 110 |
| `jellyfish` | 77 | 77 |
| `terrain` | 1 (1831 ms) | 1 (2041 ms) |
| `shadow` | 118 | 111 |
| **`refract`** | **8** (132 ms) | **5** (216 ms) |
| `conditionals` 0/0, 0/5, 5/0 | 230, 143, 229 | 229, 142, 228 |
| `function` low / medium | 169, 72 | 168, 72 |
| `loop` no-loop / uniform=false / uniform=true | 167, 167, 160 | 166, 166, 159 |

Nothing regressed; four scenes moved and each is attributed below.  Offline
gates from the build directory they were built in, exit codes read rather than
last lines (`tools/m9-offline-gates.sh`): `m9-batch-test` rc=0,
`m9-vs-compile-test` rc=0, `m9-fs-compile-test` rc=0.  Fixture suite **263
cases, 247 PASS, 16 FAIL, 0 non-M9, 0 stream timeouts**; the sixteen are the
ten `dxt*`, the four `giga10*` and `mrt10:2`/`mrt10:4`, which
`docs/parhelia-m9-mrt-routing.md` 2f already records as needing the GigaColor
kernel module -- the same sixteen features, none of them this work.

### Where `refract`'s frame went, and where it goes now

MEASURED at 1920x1200, `M9X_PROFILE=20`, one binary a row:

| | ms/frame | fps | jobs | ioctl | be | varying | KiB gathered |
|---|---:|---:|---:|---:|---:|---:|---:|
| before | 210.8 | 5 | 130 | 81.1 | 82.0 | 15.9 | 118983 |
| after | 128.0 | 8 | 70 | 74.7 | 16.1 | 7.0 | 29574 |

**The frame was never mostly submission.**  `M9X_DRYRUN=1` validates the same
command stream in the kernel and does not execute it; the ioctl phase is then
**4.6 ms** where the live one is 75, and the whole frame is 94.6 ms where the
live one is 166.  So the call itself costs about 5 ms and the other 70 is the
engine drawing, which is why the ioctl phase **does not move with the job
count**: 81.1 ms at 130 jobs, 76.0 at 70, 82.8 at 55.  The CPU and GPU halves
add rather than overlap, and the CPU half is what this work cut.

**82 of the 210 ms were one memset.**  A split job cleared the engine's
back-end object ahead of itself, every time, because a job that finds another
shape's state there stops the engine.  The object is a megabyte of VRAM and the
clear is a CPU write-combined store, so it cost about 0.63 ms a job.  Two
things bound it:

* **the shape it carries is remembered**, for every job the engine runs, and a
  split job clears only when what is there is not its own.  This does not fire
  on `refract`: a padded split job narrows the clip window to the pixels its
  own geometry covers, so a mesh cut into seventy jobs changes it seventy
  times -- and the clip window is genuinely part of what the object holds.
  With `M9X_BE_SHAPE_CLIP=0`, which leaves it out of the comparison, the engine
  stops and the stream times out (MEASURED).  It does fire for `desktop`,
  whose sixteen split jobs a frame now spend **1.49 ms** in the clear.
* **the clear is sized by the extent the object uses**, not by the allocation.
  The mask extent is linear in the target's area, `area / 24.495 + 20` dwords
  (DERIVED from the dumps, below), 367 KiB at 1920x1200 against a 1 MiB
  `M9_BE_BYTES`.  MEASURED on `refract` at 23040 job vertices, one binary with
  `M9X_BE_CLEAR_BYTES` the only variable and every cell rendering with the
  fixture passing after it: **be 41.68 ms at 1 MiB, 20.99 at 512 KiB, 15.09 at
  384 KiB, 10.66 at 256 KiB**.  The driver now writes the relation with the
  divisor rounded down and 16 KiB of margin.

**The record ceiling is the second lever, and its failure at the maximum is
explained.**  MEASURED on `refract`, `M9X_JOB_VERTICES` the only variable, no
draw skipped in any cell: 130 jobs and 210.8 ms at 11520, 70 and 166.5 at
23040, 67 and 165.9 at 32768, 55 and 165.5 at 46080.  The default is now
23040.  `65532` -- the most the 16-bit raster index allows -- **exhausts
VRAM**: the transient stream views are `20 * ceiling * 16` bytes, 21 MiB there
against 3.7 at the old default, and the run reports "cannot allocate target
depth" and "cannot allocate the c2 image", draws nothing and times the stream
out (MEASURED).  It is **VRAM, not system memory** -- this target has 16 GB --
so the note that blamed 481 MB is replaced rather than merely withdrawn.

**`bump` high-poly gains from the ceiling for a different reason: the weld.**
MEASURED, one binary, the ceiling the only variable: at 11520 the scene is 14
jobs of 144004 records and 105 fps; at 23040 its 144000-corner mesh fits one
indexed run, welds to 40219 referenced vertices, and it is **3 jobs of 40224
records and 135 fps**.  `refract` welds the same way at 46080 (208998 corners
to 34835 referenced) but gains nothing by it, because its cost is not the
records.

**The varying gather is a quarter of what it was.**  A split job preprocesses
one copy of its geometry a raster pass, so pass `i` reads records at the first
copy's index plus `i * records`, and the CPU-gathered varying views were
replicated behind the first set to match.  The copies are unnecessary: the view
a pass binds is a relocation of its own, so the gather is written once at the
tail of the view and each pass's base is pulled back by exactly its own copy's
offset.  MEASURED with this as the only change: 118294 to 29574 KiB a frame,
17.47 to 6.94 ms, and the frame 166.45 to 154.02 ms.

**Several draws to a job was already the driver's behaviour.**  MEASURED with
`M9X_PROFILE=100`: `ideas` puts **351 draws into 20 jobs** a frame and
`shading=phong` 7 into 3.  Where the two are 1:1 it is because a mesh draw
fills the record ceiling on its own, or because the job is split and a split
job carries exactly one raster draw.  Every one of `refract`'s 70 jobs is
split, so this lever could not have reached it.

**What now bounds `refract`, plainly.**  Of its 128 ms: the engine 75, the
back-end clear 16, the draw module and the record fill about 24, the varying
gather 7, the record copy 4, the command build 2.  Removing **every** remaining
CPU cost would leave 75 ms, which is **13 fps**.  The scene is a dependent
texture chain that the engine cannot run in one pass, so it rasterises its
208998-corner mesh four times a frame; 60 fps needs the pass count or the
fragment cost to fall, not the submission path.

### The picture

CAPTURED off scanout at 1920x1200, `xwd` of the root window at 5 s and 8 s,
against an llvmpipe frame of the same scene at the same seconds (`Xvfb :77`,
`LIBGL_ALWAYS_SOFTWARE=1`, the T2 `glmark2`, `maim`).  The scene animates, so
the two renderers are at different phases and the 8 s pair is the one where
they nearly coincide:

| | distinct colours | mean r,g,b | 8x8 tile difference |
|---|---:|---|---:|
| M9, 8 s | 57051 | 15.2, 12.6, 19.4 | **1.5 of 255** vs llvmpipe |
| llvmpipe, 8 s | 54667 | 14.9, 12.2, 18.7 | -- |
| M9 before this work, 8 s | 55671 | 14.9, 12.4, 19.0 | **1.0 of 255** vs the M9 after |

So the frame the faster driver draws is the frame the slower one drew, and
both are the reference's.


## Superseded: standing 2026-09-19 (fourth): the register allocator, and terrain's colour

MEASURED at 1920x1200 on the M9128, **one X server and one process a
benchmark** (`tools/m9x-glmark-default-suite-fresh-x.sh`),
`M9X_SKIP_REPORT=1`, all thirty-three completed, **0 stream timeouts**, a
known-good fixture passing before and after the sweep, the hardware lock held
across the whole cycle.  The tree is the merged one plus the multisample
fragment store and the fragment register allocation fix
(`docs/parhelia-m9-fragment-pass-splitting.md` 8.4b).

| benchmark | fps | draws skipped a frame |
|---|---:|---|
| `build` vbo=false / vbo=true | 273, 294 | none |
| `texture` nearest / linear / mipmap | 283, 282, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 242, 149, 88, 82 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | none |
| `effect2d` 3x3 / 15-tap | 84, 33 | none |
| `pulsar` | 219 | none |
| `desktop` blur / shadow | 37, 124 | none |
| `buffer` map / subdata / interleaved | 153, 153, 153 | none |
| `ideas` | 110 | none |
| `jellyfish` | 77 | none |
| **`terrain`** | **1** (2041 ms a frame) | none |
| `shadow` | 111 | none |
| `refract` | 5 | none |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | none |
| `function` low / medium | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 166, 166, 159 | none |

Offline gates, run from the build directory they were built in and their exit
codes read rather than their last line (`tools/m9-offline-gates.sh`, which
refuses a gate the build directory says is out of date): `m9-batch-test`
rc=0, `m9-vs-compile-test` rc=0, `m9-fs-compile-test` rc=0 with all four
split cases ok.  The fragment gate's summary line reads "required exact
23/23" even when a case above it has failed, so only the exit code says so --
which is how a refusal of `split shared-coordinate-stream` was first missed
here.

Fixture suite: **256 cases, 242 PASS, 14 FAIL, 0 non-M9, 0 stream timeouts**,
the engine proved before and after, and the same tally on the build before and
after the interpolation-register scope fix.  The fourteen are the ten `dxt*`
cases, which the driver refuses with "texture levels beyond the captured 32x16
tail need the kernel texture window", and the four `giga10*`; all fourteen
fail identically with `M9X_RA_RESET=0 M9X_CHECK_RA=0`, so they are the state
of those two features and not the allocator (MEASURED, `dxt1`, `giga10clear`
and `giga10tex` run both ways one after the other).

The list was run twice, once on the build that carried the interference check
alone and once on the build that also scopes the interpolation-register reuse
to `14..1f`, and **every one of the thirty-three came back with the same frame
rate on both** -- 273, 294, 283, 282, 283, 242, 149, 88, 82, 105, 182, 164,
84, 33, 219, 37, 124, 153, 153, 153, 110, 77, 1, 111, 5, 229, 142, 228, 168,
72, 166, 166, 159 -- with no draws skipped and 0 stream timeouts either time.
The scope fix tightens allocation, so it could in principle have forced a cut
somewhere; it did not.

**Nothing regressed.**  Every row matches the standing below it within
run-to-run noise, and the allocator change -- which touches every fragment
program the driver compiles -- costs no frame rate anywhere.  `terrain` is
unchanged at 1 fps, and its schedule is longer than it was (ten raster passes
offline where the placement that carried over let it fit in three), so the
frame rate it reports is bought with correct registers rather than shared
ones.

**`terrain`'s colour is most of the way home and the rest is named.**
CAPTURED at 1920x1200, `terrain:bloom=false:tilt-shift=false`, against an
llvmpipe frame of the same variant:

| | frame mean r, g, b | 8x8 tile difference |
|---|---|---:|
| before | 68.6, 43.6, 113.8 | 58.6 of 255 |
| **after** | **92.5, 89.6, 79.8** | **21.8 of 255** |
| llvmpipe | 95.0, 88.8, 31.7 | -- |

Red and green now match; the frame has the reference's relief, its grass and
its dirt, and what is left is a blue excess on the lit pixels.  Every term of
the colour is correct on its own -- the texture mix 2.9, `totalDiffuse` 7.3,
`pointSpecular` 9.8 and the whole lighting sum 9.7 of 255, the last of which
was the flat 135, 136, 255 this file used to record.  The remainder is one
step, the **product of the record the texture mix is carried in with a vector
the consuming pass computes**, and it is the third component of a narrowed
record that comes back wrong.
`docs/parhelia-m9-fragment-pass-splitting.md` 8.4b and 8.4c.

The **striped blocks of the tilt-shift blur are untouched** and still open.

## Superseded: standing 2026-09-19: the three scenes that were not drawing everything

MEASURED at 1920x1200 on the M9128, one X server and one process a benchmark,
`M9X_SKIP_REPORT=1`, **0 stream timeouts**, fixture suite **228 of 228 PASS**
both before and after the sweep.  Each of the three had been reporting a frame
rate for a picture it did not draw.

| benchmark | before: fps, draws skipped a frame | after: fps, skipped | what it was |
|---|---|---|---|
| `shadow` | 145, **1447** | 111, **0** | a depth-only framebuffer derived no raster parameters |
| `jellyfish` | 125, **1245** | 77, **0** | the blend path took only registers the program never named |
| `loop` uniform=true | 345, **3446** | 159, **0** | the loop bound arrived as the float encoding of 5 |

The frame rates fall because the work is now being done.  `loop` at
159 fps sits beside the 167 fps of `loop:fragment-uniform=false`, which
is the same picture with the bound written as a literal, so the specialised
variant is doing the same five iterations.

### The pictures, not only the tallies

CAPTURED off scanout at 1920x1200, `xwd` of the root window 6 s into each run,
one X server a scene:

| scene | distinct colours | mean r,g,b |
|---|---:|---|
| `shadow` | 805 | 22.5, 22.5, 22.5 |
| `jellyfish` | 232951 | 68.8, 105.1, 199.9 |
| `loop` uniform=true | 1786 | 44.5, 44.5, 44.5 |

`shadow` is the one worth reading closely, because a frame rate and a skip
tally cannot tell an empty shadow map from a full one: the fragment shader
samples the map and multiplies the lit colour by **0.5** where the surface is
farther from the light than the stored depth, and with an empty map
(`light_distance` = 1.0) that branch is never taken.  The capture's luminance
histogram has exactly that pair -- **76 over 26.7% of pixels and 38 over
3.0%**, one being half the other, with 68% black background.  So the map
carries real depth and the shadow is drawn.

The distinct-colour count is **not** comparable between runs here: these
scenes animate, and a capture 6 s in is at a different phase from one at 5 s
or 8 s.  The histogram pair is what carries the reading.

The software reference agrees on exactly the part the shadow map decides.
CAPTURED through the system Mesa under the same X server, 6 s in:

| luminance band | M9 | software |
|---|---:|---:|
| 0, background | 68.35% | 64.46% |
| **38, shadowed floor** | **2.99%** | **3.13%** |
| **76, lit floor** | **26.65%** | **26.44%** |
| 61-95 other than 76 | 0.47% | 1.22% |
| above 95 | 0.21% | 2.87% |

`jellyfish` agrees with its reference throughout: 232951 distinct colours
against 231066, mean 68.8/105.1/199.9 against 67.9/103.3/196.0.  `loop`
uniform=true agrees on the mean, 44.5 against 44.6.

**Open, and not one of the three:** `shadow`'s bright band is missing on the
M9 -- 0.21% of pixels above luminance 95 where the reference has 2.87%, and
0.47% against 1.22% in 61-95, with the difference showing up as background.
The floor and its shadow are exact; what the reference draws and the M9 does
not is the lit model that casts it, which `shadow.vert` does not shade
(`Color = MaterialDiffuse`, one flat value) -- the horse is drawn with
`light-basic.vert/frag` instead.  This is a separate defect from the
depth-only framebuffer and it was invisible while every draw into that
framebuffer was being skipped.  **The capture that would attribute it** is
`shadow` on the build before these fixes, beside `shading=gouraud` on both
builds, since `gouraud` uses the same `light-basic` pair; it was prepared
(`/root/tmp/skip3-run5.sh` against the `/root/parhelia-m9x-skipbase` prefix)
but did not run, because the target was rebooted for other work before it
reached the head of the hardware lock.

`jellyfish` is the blended scene, 232951 distinct colours over a blue
distribution centred near 160-165.  `loop` is the `fract` noise pattern, 256
luminance levels thinly spread over the grid with the background black.

Five fixtures were added with these fixes, because none of the 228 already in
the suite covered any of the three shapes: `depthonly` renders a shadow map
into a framebuffer with no colour attachment (the existing `depthtex` always
attached a colour texture as well), `blendheavy` blends a shader whose body
names the whole register pool, and `loopuni:0`, `:5` and `:24` draw a loop
over a uniform bound.  The suite is **233 of 233 PASS** with them, and each of
the five differs in **0 of 76800 pixels** from its CPU reference (MEASURED).

`blendheavy` and the `loopuni` cases carry an OFFLINE negative control -- the
same shader is refused by the build before the fix; `depthonly` has none,
because the refusal it guards was in the driver's framebuffer path rather
than in the compiler the offline test drives.


### The whole thirty-three with four render targets advertised

MEASURED 2026-09-19 after `caps->max_render_targets` was raised from 1 to 4
and the fragment constant count was bounded at the measured-good 56: all
thirty-three completed, **0 stream timeouts**, and **not one "draws skipped"
line in any of the thirty-three logs**.  One X server and one process each,
1920x1200, `M9X_SKIP_REPORT=1`, every benchmark holding **both** hardware
lock files (see the backlog entry; a first attempt without that had another
agent's `glmark2` on the same X server and halved two rows).

| benchmark | fps | standing |
|---|---:|---:|
| `build` vbo=false / vbo=true | 273, 295 | 273, 295 |
| `texture` nearest / linear / mipmap | 283, 282, 283 | 284, 283, 283 |
| `shading` gouraud / blinn-phong-inf / phong / cel | 242, 149, 88, 83 | 243, 149, 88, 83 |
| `bump` high-poly / normals / height | 105, 182, 164 | 105, 182, 164 |
| `effect2d` 3x3 / 15-tap | 84, 33 | 84, 33 |
| `pulsar` | 219 | 220 |
| `desktop` blur / shadow | 37, 124 | 37, 124 |
| `buffer` map / subdata / interleaved | 153, 153, 153 | 153, 153, 154 |
| `ideas` | 109 | 110 |
| `jellyfish` | 77 | 77 |
| `terrain` | 1 | 1 |
| `shadow` | 111 | 111 |
| `refract` | 5 | 5 |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | 229, 142, 228 |
| `function` low / medium | 168, 72 | 168, 72 |
| `loop` no-loop / uniform=false / uniform=true | 166, 166, 159 | 167, 167, 159 |

Every row is within one frame a second of the standing below it.  Nothing in
the MRT work is on a single-target path: the fragment backend's one-output
schedule is unchanged and its corpus result is identical, the clear binds
target 0 alone exactly as before when the framebuffer holds one attachment,
and the raised cap only changes what `GL_MAX_DRAW_BUFFERS` reports to a
client that asks.  The constant bound is the one change that could have
refused a shader here, and no scene reaches it -- the largest pass in the
thirty-three holds 42 constants against the bound's 56.

The MRT fixtures alongside it, same binary, same locks, known-good fixture
before and after: `mrt:2`, `mrt:3`, `mrt:4`, `mrt:4:5` and `mrtcolor:2` all
**0 of 76800 pixels differing on every target**, and `fsconst:30` -- 68
constants, which used to fail all 76800 pixels and log a stream timeout --
now reports "a job could not be built; those draws are skipped" and leaves
the engine alive.

### The whole thirty-three on this build

MEASURED afterwards on the same binary, `tools/m9x-glmark-default-suite-fresh-x.sh`
at 1920x1200 with `M9X_SKIP_REPORT=1`: all thirty-three completed rc=0, **0
stream timeouts**, and **not one "draws skipped" line in any of the
thirty-three logs**.  The fixture suite ahead of the sweep was 233 of 233.

| benchmark | fps |
|---|---:|
| `build` vbo=false / vbo=true | 273, 295 |
| `texture` nearest / linear / mipmap | 284, 283, 283 |
| `shading` gouraud / blinn-phong-inf / phong / cel | 243, 149, 88, 83 |
| `bump` high-poly / normals / height | 105, 182, 164 |
| `effect2d` 3x3 / 15-tap | 84, **33** |
| `pulsar` | 220 |
| `desktop` blur / shadow | **37**, 124 |
| `buffer` map / subdata / interleaved | 153, 153, 154 |
| `ideas` | 110 |
| **`jellyfish`** | **77** |
| **`terrain`** | **1** |
| **`shadow`** | **111** |
| **`refract`** | **5** |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 |
| `function` low / medium | 168, 72 |
| `loop` no-loop / uniform=false / **uniform=true** | 167, 167, **159** |

Every row outside the three is identical to the standing below it, so the
integer lowering -- which touches every fragment shader the driver compiles --
changed nothing else that a frame rate can see.  Four are still under 60 fps
(`effect2d` 15-tap, `desktop` blur, `terrain`, `refract`), each for a reason
recorded below; none of them skips a draw.

## Superseded: standing 2026-09-19 (merged): the whole thirty-three on one build

The two sweeps below it were measured on **different builds that were never
tested together** -- the `terrain` sweep without `refract`'s record-ceiling
fix (`22c0352`), the `refract` sweep without the resource splitter.  Both are
merged, and this is the hardware sweep of the union.  It is the only standing
to read as current; both of those are superseded.

MEASURED at 1920x1200 on the M9128, **one X server and one process a
benchmark** (`tools/m9x-glmark-default-suite-fresh-x.sh`),
`M9X_SKIP_REPORT=1`, all thirty-three completed, **0 stream timeouts**, with a
known-good fixture passing before and after each sweep.  Two columns: the
merged tree as it was, and the same tree carrying the index-88 fix that
closes `coordconv:4` (`docs/parhelia-m9-fragment-pass-splitting.md` 8.5).
Fixture suite: **224 of 224 PASS** on the merged tree and **228 of 228** with
the fix, which lists `coordconv:4`, `:5`, `:6` and `:4d` as well.

| benchmark | merged, fps | with the fix, fps | draws skipped a frame |
|---|---:|---:|---|
| `build` vbo=false / vbo=true | 273, 295 | 273, 294 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 243, 149, 88, 83 | 243, 150, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | 105, 182, 164 | none |
| `effect2d` 3x3 / 15-tap | 84, 33 | 84, 33 | none |
| `pulsar` | 220 | 220 | none |
| `desktop` blur / shadow | 37, 124 | 37, 124 | none |
| `buffer` map / subdata / interleaved | 153, 153, 154 | 153, 153, 154 | none |
| `ideas` | 110 | 110 | none |
| `jellyfish` | 125 | 125 | **1245**, out of temporaries for blending |
| **`terrain`** | **1** | **1** | **none** |
| `shadow` | 145 | 145 | **1447**, a depth-only framebuffer has no raster parameters |
| **`refract`** | **5** | **5** | none |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | 229, 142, 228 | none |
| `function` low / medium | 169, 72 | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 167, 167, 345 | 167, 167, 345 | **3446** on uniform=true, control flow |

**Nothing regressed in the union.**  Every row of both sweeps matches both
earlier standings within run-to-run noise; `terrain` still skips nothing and
`refract` is still 5 fps.  The three scenes that skip draws skip them for the
fragment-backend reasons they did before, none of which is the split path.

`refract` is still **correct**: CAPTURED off scanout at 1920x1200, the
refracting bunny over black, 174602 distinct colours on the merged tree and
190375 with the fix -- the same range as the 181662 the llvmpipe reference
gave in the superseded standing below, which was not re-measured here
because llvmpipe drew no frame in ten seconds at either size.

`terrain` is **not**, and the fix moves it without closing it.  CAPTURED off
scanout at 1920x1200 and 800x500 with a known-good fixture passing after:
the distinct-colour count over the root window goes from **1297 to 104000**
and the frame from a flat near-white wash to a lit landscape with relief and
detail, but the scene is blue-dominant where the reference is brown and
green, and the near field carries a regular striped block pattern fixed in
screen pixels.  So the frame rate of 1 fps is still not a result for it.

**Those are two defects, and the scene's own options separate them**
(CAPTURED 2026-09-19 at 1920x1200, one X server a variant, against llvmpipe
frames of the same four variants taken on the build host).  With both
post-processing effects off -- `terrain:bloom=false:tilt-shift=false`, the
terrain drawn straight to the window -- the silhouette and the relief are
exact and the **colour is still wrong**: the far field saturated blue, the
near field dark green with saturated red patches.  Turning the bloom pair on
changes nothing about it.  The **striped blocks appear only with the
tilt-shift blur**, which is the nine-tap pass, and they are not visible in
the far field because the far field is a saturated constant there.  Fix the
colour first: the blur's input is the wrong picture.
`docs/parhelia-m9-fragment-pass-splitting.md` 8.4 and 8.4a.

## Superseded: standing 2026-09-19 (second), the resource splitter alone

The merged standing above replaces this; it was measured without `refract`'s
record-ceiling fix.  `terrain`'s picture is no longer what this says.

MEASURED at 1920x1200 on the M9128, `tools/m9x-glmark-default-suite-fresh-x.sh`,
`M9X_SKIP_REPORT=1`, 0 stream timeouts.

| scene | before | after |
|---|---|---|
| `terrain` | 1 fps, **24 draws a frame refused** by the fragment backend | 1 fps, **0 draws skipped**, mesh silhouette and sky on the monitor, body wrong |

What changed is the fragment splitter: it now partitions a shader by the
resources a raster pass holds -- texel slots, the register file and the six
interpolation registers -- rather than by the fetch count alone, and three
defects in front of that are fixed (the coordinate's varying location was
read from the wrong intrinsic, two units could not share a varying location
at an odd component, and the layout gate counted a coordinate stream a
sampler).  `docs/parhelia-m9-fragment-pass-splitting.md` section 8.

**`terrain` is not conformant and the frame rate is not a result.**  The
terrain body comes back with R and G exactly zero.  The pattern is fixed in
screen pixels -- the same block size at 1920x1200 and at 480x300 (MEASURED,
both captured) -- so it is the inter-pass record, not texture sampling.

The defect is isolated to a fixture that did not exist before:
**`coordconv:4` fails, 54092 of 76800 pixels, against a CPU reference**, while
`coordconv:2` and `coordconv:3` pass exactly.  It is glmark2's tilt-shift
blur in miniature -- N taps around a base coordinate the fragment stage
computes, one producing pass a tap -- and `terrain` presents through the
nine-tap form of it.  It is not the record slot count, not the pass count and
not the choice of cuts; each of those is ruled out with one variable in
section 8.5 of the splitting note.

**This very likely also explains `refract`'s black frame**, which is the same
family: four passes, three record slots, every coordinate computed.

### The whole list, re-run (MEASURED 2026-09-19, one X server a benchmark)

`tools/m9x-glmark-default-suite-fresh-x.sh`, 1920x1200, `M9X_SKIP_REPORT=1`,
**0 stream timeouts**, all thirty-three completed.

| benchmark | fps | draws skipped |
|---|---:|---|
| `build` vbo=false / vbo=true | 273, 296 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 243, 149, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | none |
| `effect2d` 3x3 / 15-tap | 84, 33 | none |
| `pulsar` | 221 | none |
| `desktop` blur / shadow | 37, 124 | none |
| `buffer` map / subdata / interleaved | 153, 153, 154 | none |
| `ideas` | 110 | none |
| `jellyfish` | 125 | 1244, out of temporaries for blending |
| **`terrain`** | **1** | **none** (was 24) |
| `shadow` | 145 | 1447, no valid raster parameters |
| `refract` | 39 | none |
| `conditionals` 0/0, 5/0, 0/5 | 229, 142, 228 | none |
| `function` low / medium | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 167, 166, 345 | 3445 on uniform=true, control flow |

Every row matches the previous standing within run-to-run noise.  `terrain`
is the only change, and the three scenes that still skip draws skip them for
the reasons they did before.

## Superseded: standing 2026-09-19 (first)

**MEASURED 2026-09-18** at 1920x1200 on the dual-DisplayPort M9128 (subsystem
102b:22c0, 1 GiB VRAM) in the 16 GB host, one head driven, stock X server with
the image's glamor, Mesa built with LLVM JIT (`draw-use-llvm`).

Measured with `tools/m9x-glmark-sweep.sh` and `tools/m9x-glmark-parse.awk`.
The parser refuses to count a scene whose draws the driver skipped, whose job
lost its context, or whose stream timed out - each of those reports a very high
frame rate precisely because nothing was drawn.  It annotates rather than
invalidates a scene that fell back off a hardware path, because such a scene
does draw.  A fault raised *after* the last scene's result is charged to the
run, since submission is asynchronous and the error surfaces later than the job
that caused it.

glmark2 used to segfault (rc=139, client-side, no stream timeout) moving off
the 15-tap `effect2d` scene within one process, which is why the list was
swept one scene per process.  That was the driver's `extra_offset` overflow,
fixed by `d14b711`; the default list has run in one invocation since
(standing at the top of this file).

Every run asserts `GL_RENDERER` is `Matrox M9`: a Mesa build configured without
`--cross-file cross-t2-llvm.ini` links against the host libdrm and X, EGL falls
back to llvmpipe silently, and every fixture still passes against the CPU
reference.

## Superseded: standing 2026-09-19 (later), the record ceiling alone

The merged standing above replaces this; it was measured without the resource
splitter, which is why `terrain` still skips 24 draws here.

MEASURED at 1920x1200, **one X server and one process a benchmark**
(`tools/m9x-glmark-default-suite-fresh-x.sh`), `M9X_SKIP_REPORT=1`, the whole
thirty-three in one sweep, **0 stream timeouts**, no
`glCheckFramebufferStatus` failure and no lost context anywhere in the run.
Fixture suite alongside it: **221 cases, 221 PASS, 0 FAIL, 0 non-M9** against
the module carrying the 10-bit surface-window commits, so the four `giga10`
failures of the sweep below are closed as well.
The only row that moved is `refract`; every other benchmark is within a frame
a second of the sweep below, which is the control this is read against.

| benchmark | fps | draws skipped a frame |
|---|---:|---|
| `build` vbo=false / vbo=true | 270, 294 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 242, 149, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | none |
| `effect2d` 3x3 | 84 | none |
| **`effect2d` 15-tap** | **33** | none |
| `pulsar` | 220 | none |
| **`desktop` blur** | **37** | none |
| `desktop` shadow | 124 | none |
| `buffer` map / subdata / interleaved | 153, 153, 153 | none |
| `ideas` | 111 | none |
| `jellyfish` | 125 | **1244**, out of temporaries for blending |
| **`terrain`** | **1** | **24**, five samplers against four units and the register file |
| `shadow` | 145 | **1447**, a depth-only framebuffer has no raster parameters |
| **`refract`** | **5** | none - **and the picture is right** |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | none |
| `function` low / medium | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 166, 166, 344 | **3437** on uniform=true, control flow |

Twenty-nine of the thirty-three are at or above 60 fps, as before; `refract`
was already below it at 39 and is further below now.  That is a real cost and
it is named in the backlog, but it buys the first correct frame this scene has
ever produced on the card.

### What `refract` was doing, and what it does now

**The black frame was never the split path.**  A split shader preprocesses one
copy of the geometry per raster pass, so `m9_job_reserve` refuses a run of more
than `ceiling/passes` source records - 2856 of the 11520 ceiling for a
four-pass shader.  Every producer sized its runs by the **whole** ceiling, and
on the refusal simply returned.  The draw module handed refract's main pass
over in runs of 11520, **every one of them was refused**, and the frame carried
the tail alone.

MEASURED with `M9X_SPLIT_TRACE=2`, which names every job and not only the split
ones - one frame of `refract`, in order:

```text
m9x job 33: 2048x1280 passes 0 draws 1 records 4      the distance target's clear
m9x job 34..51: 2048x1280 passes 1 draws 1 records 11520   the depth pass, 18 jobs
m9x job 52: 2048x1280 passes 1 draws 1 records 1640        its tail
m9x job 53..58: 1024x640 .. 32x20 passes 1 records 4       the generated mip chain
m9x job 59: 1920x1200 passes 4 draws 1 records 156         the main pass, ENTIRE
```

The depth pass draws all 208998 corners of `bunny.obj` (69666 triangles, one
record a corner); the main pass, over the same mesh, carried **156**.  52
triangles of 69666, which is a black screen.

This **predates the padded split path**.  The control was run: the 2026-09-18
deployment at `/root/parhelia-m9x-mesa`, built before `a47dcf5`, `4cd7d1d` and
`15dbebf`, renders `refract` at **1 distinct colour** in 2.3 M pixels while
reporting `draws skipped` for coverage.  So this is a scene that had **never
rendered**, not a regression from those three; the padded path removed the
coverage refusal and exposed the record loss underneath it.

`m9_job_max_vertices()` (`98be636`) is that ceiling in one place - the pass
count and the padding a partial draw needs - and `m9_job_reserve`, the draw
module's two append paths and the three native vertex paths all use it.  A run
that still does not fit is **tallied as a skipped draw**, so this class of loss
cannot be silent again.

CAPTURED off scanout with the fix, `xwd` of the root window at 5 s and again at
8 s of a 1920x1200 run, the known-good fixture rendering after it in the same
session:

| | distinct colours | mean r,g,b |
|---|---:|---|
| 2026-09-18 control, 5 s | 1 | 0, 0, 0 |
| HEAD before the fix, 5 s / 8 s | 15 / 4 | 0, 0, 0 |
| **with the fix, 5 s** | **223965** | 21.6, 18.1, 26.0 |
| with the fix, 8 s | 194363 | 17.8, 15.8, 22.9 |
| llvmpipe reference, same size | 181662 | - |

The picture is the refracting bunny over black, and it matches the software
reference in structure and colour; the distinct-colour ratio against it is
1.23.  Reproduced on the final binary (`22c0352`) after a fresh module load:
227978 at 5 s, 194812 at 8 s, fixture after it, 0 stream timeouts.

### The cost, and where it comes from

MEASURED, `M9X_PROFILE=20` at 1920x1200 with the fix:

```text
m9x: 20 frames 212.10 ms/frame (job vertices 11520): draw 194.11 emit 184.21
     submit 186.90 varying 16.21 records 4.04 ioctl 81.24 | jobs 130 draws 130
     records 418084 varying KiB 118982.6 refused 0 skipped 0
```

130 jobs a frame against 28 while it was dropping the geometry, 81 ms of submit
ioctl at 0.62 ms a job, and 116 MiB of CPU varying gather - the varyings are
repeated once per raster pass behind the first set, so a four-pass job pays for
its geometry four times.

The job count is forced by the hardware rule, not by a choice: a split job
carries exactly one raster draw (3c), and its records are capped at
`ceiling/passes`.  The only lever inside that rule is a larger record ceiling,
and the 16-bit index space caps `records * passes` at 65536, so 16384 records a
job and about 13 jobs for this mesh.  `M9X_JOB_VERTICES=65532` does **not**
reach it on this target: MEASURED, 0 jobs, 0 draws, nothing drawn and a stream
timeout, the cause of which is **not** established - this target has 16 GB (MEASURED), so the earlier memory attribution is withdrawn.

## Superseded: standing 2026-09-19, before `refract`'s geometry was delivered
### All thirty-three run, twenty-nine above 60 fps, four still skip draws

MEASURED at 1920x1200, **one X server and one process a benchmark**
(`tools/m9x-glmark-default-suite-fresh-x.sh`, and see why below), the padded
split path on by default, `M9X_SKIP_REPORT=1`, 0 stream timeouts, fixture
suite 211 cases 207 PASS.  The four failures are the `giga10` cases and belong
to the **module**, not to this build: MEASURED, the same Mesa binary against a
kernel carrying the 10-bit surface-window commits passes `giga10tex`,
`giga10rt` and `giga10blend` exactly and leaves only `giga10clear` reporting a
channel difference.

| benchmark | fps | draws skipped a frame |
|---|---:|---|
| `build` vbo=false / vbo=true | 273, 294 | none |
| `texture` nearest / linear / mipmap | 284, 283, 283 | none |
| `shading` gouraud / blinn-phong-inf / phong / cel | 243, 149, 88, 83 | none |
| `bump` high-poly / normals / height | 105, 182, 164 | none |
| `effect2d` 3x3 | 84 | none |
| **`effect2d` 15-tap** | **33** | none |
| `pulsar` | 220 | none |
| **`desktop` blur** | **37** | none |
| `desktop` shadow | 124 | none |
| `buffer` map / subdata / interleaved | 153, 154, 154 | none |
| `ideas` | 111 | none |
| `jellyfish` | 125 | **1244**, out of temporaries for blending |
| **`terrain`** | **1** | **24**, five samplers against four units and the register file -- **0 as of the second standing above** |
| `shadow` | 145 | **1446**, a depth-only framebuffer has no raster parameters |
| **`refract`** | **39** | none |
| `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | none |
| `function` low / medium | 168, 72 | none |
| `loop` no-loop / uniform=false / uniform=true | 166, 167, 345 | **3444** on uniform=true, control flow |

Twenty-nine of the thirty-three are at or above 60 fps and twenty-nine draw
every draw they are given.  The four that skip draws are `jellyfish`,
`terrain`, `shadow` and `loop:fragment-uniform=true`, each for a **fragment
backend** reason rather than anything in the split path.

**A frame rate with skipped draws is not a result.**  Two of these were only
found because `M9X_SKIP_REPORT` was on: `shadow` reported 295 fps while
skipping 2942 draws a frame and `loop:fragment-uniform=true` reports 345 fps
while skipping 3444.  Both look like the fastest scenes in the list.

### What changed on 2026-09-19

| scene | before | after | why |
|---|---|---|---|
| `shadow` | 295 fps, 2942 draws a frame skipped | 145 fps, none for coverage | the padded split path carries a multi-pass draw whose geometry does not fill the box it spans.  CAPTURED on scanout at the same point of the run: the same 1786 distinct colours at **twice the mean luminance**, 11.2 to 22.2, which is the pass it was dropping |
| `refract` | 41 fps, 403 draws a frame skipped | 39 fps, none | the same |
| `desktop` | stops the engine inside the full list, taking the seventeen benchmarks behind it down | 37 fps, whole list completes | the back-end object is swapped rather than cleared under a live job |
| the list | ends at benchmark 16 | all 33 complete, 0 stream timeouts | the same |

The `desktop` row is not a reconstruction: the **control was run**.  Against
the build of 2026-09-18 on the same target, same harness and same module,
`desktop` at position 16 reports `two-pass job failed (Input/output error);
context lost` and 6536 fps, and benchmarks 17 to 33 all exit rc=1.

### Two readings this retires

* **`refract` is not conformant** even now.  It skips no draws and reports
  39 fps, and the frame on the monitor is **black** - 16 distinct colours in
  2.3 million pixels, mean 0, CAPTURED at 5 s and again at 8 s, with the same
  capture showing `pulsar` at 88075 distinct colours and `texture` at 15096.
  So "0 skipped draws" is not conformance, and the ledger's "refract is
  conformant" is withdrawn.  *(Closed below: the geometry was being dropped
  by a producer that sized its runs by the whole record ceiling rather than
  by a split job's share of it.)*
* **`shadow` was never 143-147 fps for a complete picture.**  That number is
  what it reaches now, drawing the pass it used to skip; the 295 fps it
  reported before was the scene without it.

## Superseded: standing 2026-09-18

MEASURED at 1920x1200, one process per benchmark, 0 stream timeouts, fixture
suite 194 cases 194 PASS.  `desktop` draws its blur where it used to stall
and take the seventeen benchmarks behind it down with it.

Four are below 60, each for a different reason:

| benchmark | fps | why |
|---|---:|---|
| `effect2d` 15-tap | 31-33 | fragment execution.  The record-bandwidth reading is **refuted**: halving its stride from 16 dwords to 8 -- and its record from 141 MiB to 70 -- left it at the same 33 fps (MEASURED 2026-09-19, one binary, `M9X_RECORD_LIVENESS` the only variable).  `parhelia-m9-fill-rate.md` had it right |
| `desktop` blur | 38 | GPU execution: 26.77 ms a frame, submit 23.13 of it, ioctl 22.84, over 28 jobs and 24 draws with none refused or skipped.  At 0.82 ms a job this is work, not the per-job overhead that dominates `refract` at 0.23 |
| `terrain` | 1 | INVALID - draws refused by the **fragment backend**, not by the coverage rule: five distinct samplers against four units, and the register file.  MEASURED 2026-09-19 |
| ~~`refract`~~ | 38-40 | **conformant** - it was never the geometry class.  A record carries x and y multiplied by w with w in slot 3, so recovering a screen position **divides**; the extent test multiplied, which agrees only where w is 1 - every fixture it had been written against.  The bunny's computed extent landed at `6694,1583..10158,5042` on a 1920x1200 target, outside it, so the draw was refused for covering none of it.  With the divide, skipped draws go 1 to 0 a frame and CPU varyings 0.4 to 101.6 KiB.  `conv3subw` is the fixture that fails without it.  It still reports SETUP-FAILED inside the full run while passing alone, which is a separate unreleased-resource question |

**Open: which `refract` measurement is current.**  A second measurement on
2026-09-19 (`M9X_PROFILE=4`, 1920x1200) reported `refract` at 19.89 ms a frame
with **`skipped 1` and 0.3 KiB of CPU varyings**, 400 native against 2801 draw
module -- the *before* numbers of the row above, from a build made in a
different tree.  Nothing was reverted in `m9x` that would restore them, so the
likely cause is that build not carrying `2f752ee`.  Until a fresh rebuild of
current `m9x` HEAD is rerun on the card, treat 0 skipped / 101.6 KiB as the
figure to reproduce and this as unreconciled, not as two valid results.
**Closed 2026-09-19**: a fresh rebuild of `m9x` HEAD reproduced 0 skipped and
101.6 KiB exactly, so that reading stands and the 0.3 KiB one was the older
build.  Both, however, describe a frame that drew 156 of 208998 records; with
the ceiling respected the figure is 118982 KiB (see the standing section).

**The depth attachment is not what refuses them, and carrying the
destination's depth box into the scratch surface would restore nothing**
(MEASURED, `M9X_SKIP_REPORT` over the whole default list): with the geometry
asked about first, *no* draw anywhere in the thirty-three is refused for
depth.  A scratch surface is the size of the box the geometry spans, so it
only helps a draw that **fills** that box -- a rectangle does, a mesh does
not.  `refract`'s missing draw is its bunny under a four-pass shader and
`terrain`'s is its mesh under seven, and no target of any size can be covered
by either, so the multi-pass schedule cannot carry them at all.  *(Superseded
for `refract`: the padded path does carry it - the geometry reaches the
target's extent through four thin rectangles while the clip window keeps the
pixels it covers - and what was left of the scene after that was a producer
dropping the runs that did not fit a split job's record ceiling.  See the
standing section at the top.)*  A single-pass
compilation would sidestep the coverage requirement, and **neither shader can
be compiled in one pass on this hardware** -- see below; see also
`parhelia-m9-fragment-pass-hardware.md` 3bc.

### Where their passes come from

OFFLINE, `M9_FS_SPLIT=1 m9-fs-compile-test --glsl` on glmark2's own shaders:

```text
light-refract.frag passes 4
    pass 0 units 0 samplers 0 varyings 3 record 1/2  routes coord
    pass 1 units 1 samplers 1 varyings 2 record 1/2  routes -1 coord 0
    pass 2 units 2 samplers 2 varyings 2 record 1/2  routes -1 -1 coord 1 0
    pass 3 units 1 samplers 1 varyings 2 record 0/0  routes -1 coord 2
```

**`refract` is a dependent texture chain, and nothing else.**  Its three
fetches would fit one pass -- four units are available and it uses four across
all four passes -- and every pass compiles, so neither the unit count nor the
register file is the constraint.  Each fetch's coordinate is computed from the
*previous fetch's result*: `DistanceMap` positions the read of `NormalMap`,
which positions the read of `ImageMap`.  A raster pass can only take a
coordinate that is affine in a varying, computed per vertex, or one handed to
it through the inter-pass record, so a fully dependent chain costs one pass per
sampler (already recorded in `parhelia-m9-fragment-pass-splitting.md` 5,
MEASURED from the vendor capture).  **No compiler can make this one pass**; it
would need the engine to sample with a coordinate computed inside the pass,
which it cannot do.

**`terrain` is both, plus a chain.**  It is refused before any splitting for
**out of temporaries** -- the register file, not the unit count -- and it binds
five distinct samplers against four units; with `M9X_SPLIT_PRESSURE=1` it
compiles into seven passes, three of which carry no sampler at all and exist
only to route values forward.  Its normal map feeds the lighting, so it has a
dependent link as well.

So aiming backend work at *fewer passes* will not close either scene.  The only
route that would is making a multi-pass job over geometry that does not cover
its target legal, which is the hardware limit in 3bb.  One idea nobody has
tried: a split job carries exactly one raster draw (3c), so the covering
geometry would have to be appended to that same draw -- extra triangles that
cover the target and are rejected before they write, if rasterised coverage
rather than written pixels is what the back end counts.  **UNTESTED.**

The rest, for the record: texture 284/283/284, build 271/271, shading
188/121/75/70, bump 89/179/154, effect2d 3x3 79, pulsar 188, desktop shadow
124, buffer 147/127/130, ideas 102, jellyfish 119, shadow 104, conditionals
198/123/197, function 146/63, loop 144/144/300.

## One X session runs the whole list

**Resolved 2026-09-19.**  It was never the X server.  glamor, DRI3 and Present
all released the client's pixmaps; the m9x Gallium driver kept the kernel
handle underneath them.

The driver holds a per-screen table of shared GEM handles
(`struct m9_prime_import`) so that two resources naming one dmabuf share a
handle and a refcount.  `m9_prime_import_get()` creates that handle with
`drmPrimeFDToHandle()`; `m9_prime_import_put()` unlinked and freed the entry
when the last reference went and **never closed the handle**.  An open handle
keeps its buffer object alive until the file description closes -- for
glamor's screen, until the X server exits.  So every dmabuf the server
imported for a DRI3 client stayed resident, and the client's exit freed
nothing.  The entry now records whether this screen created the handle and
closes it with the last reference; a handle the caller owns, such as a KMS
handle taken in and handed back out, is still left alone.

MEASURED at 1920x1200 on the M9128, one X session and five identical `build`
clients in a row, read off `/sys/kernel/debug/dri/0/vram-mm` in pages, the two
builds differing only in that commit:

| | before the fix | after the fix |
|---|---:|---:|
| before X | used 11754  free 45590 | used 11754  free 45590 |
| X up, no client | used 17572  free 39772 | used 17572  free 39772 |
| after client 1 | used 24413  free 32931 | used 19853  free 37491 |
| after client 2 | used 31253  free 26091 | used 19853  free 37491 |
| after client 3 | used 38093  free 19251 | used 19853  free 37491 |
| after client 4 | used 44933  free 12411 | used 19853  free 37491 |
| after client 5 | used 51773  free  5571 | used 19853  free 37491 |
| after X exit | used 11754  free 45590 | used 11754  free 45590 |

6840 pages a client become none.  The 2281 pages the first client does add are
one surface and they are not per client: the table is flat from the first
client to the fifth.  `build` reports 294-295 fps on both builds, so the
release costs nothing.

The three are named by the driver's own trace.  With `M9X_DEBUG_RESOURCE=1` on
the X server, each client contributes exactly three lines and, on the fixed
build, three releases:

```text
m9x: import handle 23 type 2 1920x1200 b8g8r8x8_unorm 9338880 bytes
m9x: shared handle 23 released, closed
```

9338880 bytes is 2280 pages, three of them 6840; `type 2` is
`WINSYS_HANDLE_TYPE_FD`.  So they are the client's DRI3 back buffer chain,
imported into the server as dmabufs by glamor -- 1920x1216 after the render
surface's height padding, not 1920x1200.

What it used to cost: by the sixteenth benchmark of the default list there was no room for a 1920x1200 render target,
`desktop`'s blur targets were refused with `glCheckFramebufferStatus` =
`GL_FRAMEBUFFER_UNSUPPORTED`, and glmark2 reported **96 fps for a picture it
did not draw** -- no skipped draw, no stream timeout, no lost context, so no
log parser could catch it.

**`tools/m9x-glmark-default-suite.sh` -- one X session, one process a
benchmark -- is the sweep to use.**  `tools/m9x-glmark-default-suite-fresh-x.sh`
starts one X server per benchmark and remains the control: the numbers in this
file from before 2026-09-19 were taken with it, and it is still what to reach
for when a benchmark has to start from a known server state.

MEASURED 2026-09-19, the whole default list at 1920x1200 with
`M9X_SKIP_REPORT=1`, **one X session against one server per benchmark**, the
two run within minutes of each other on the same card:

| n | benchmark | one session | one server each |
|---:|---|---:|---:|
| 1-2 | `build` vbo=false / vbo=true | 273, 295 | 273, 294 |
| 3-5 | `texture` nearest / linear / mipmap | 284, 283, 283 | 283, 282, 283 |
| 6-9 | `shading` gouraud / blinn-phong-inf / phong / cel | 243, 150, 89, 82 | 242, 149, 88, 82 |
| 10-12 | `bump` high-poly / normals / height | 105, 182, 165 | 105, 182, 164 |
| 13-14 | `effect2d` 3x3 / 15-tap | 84, 33 | 84, 33 |
| 15 | `pulsar` | 220 | 219 |
| **16-17** | **`desktop` blur / shadow** | **37, 124** | **37, 124** |
| 18-20 | `buffer` map / subdata / interleaved | 153, 154, 154 | 153, 153, 153 |
| 21 | `ideas` | 111 | 110 |
| 22 | `jellyfish` | 77 | 77 |
| 23 | `terrain` | 1 | 1 |
| 24 | `shadow` | 111 | 111 |
| 25 | `refract` | 5 | 5 |
| 26-28 | `conditionals` 0/0, 0/5, 5/0 | 229, 142, 228 | 229, 142, 228 |
| 29-30 | `function` low / medium | 169, 72 | 168, 72 |
| 31-33 | `loop` no-loop / uniform=false / uniform=true | 167, 167, 159 | 166, 166, 159 |

No row differs by more than 1 fps, **`desktop` at position 16 reports 37 and
not 96**, no `glCheckFramebufferStatus` failure, no lost context and **0
stream timeouts** in either sweep.  A second one-session sweep after a reboot
repeated it: 274, 297, 285, 284, 285, 244, 150, 89, 83, 105, 183, 165, 84, 33,
221, **37**, 125, 154, 154, 154, 110, 77, 1, 112, 5, 230, 143, 229, 169, 72,
167, 167, 160 -- at most 3 fps above the control, and nothing missing.

A known-good fixture was run **after** the one-session sweep, in the same
hardware lock hold: `m9-mesa-glsl`, **263 cases, 247 PASS, 16 FAIL, 0 non-M9,
0 stream timeouts**.  The sixteen are the ten `dxt*` cases ("texture levels
beyond the captured 32x16 tail need the kernel texture window"), the four
`giga10*`, and `mrt10:2` and `mrt10:4`; all sixteen fail identically on the
build without this commit, so none of them is the handle release.

One caveat about the skip report, which applies to both sweeps equally: with
`M9X_SKIP_REPORT=1` a glmark2 run prints `0 seen, 0 skipped` twice and nothing
else, because glmark2 exits without destroying the context that did the
drawing, and the report is written from `m9_context_destroy()`.  So it proves
that the contexts which *were* destroyed dropped nothing; it is not a count of
the benchmark's own draws.  The one-session and one-server sweeps print the
identical 66 lines, so the comparison above is like for like.

## The suite is thirty-three benchmarks, not sixteen scenes

MEASURED 2026-09-18.  Every sweep before this one ran the sixteen scene
*names*; glmark2's default run is **thirty-three parameterised
benchmarks**, and variants reach paths a bare scene never does.  A driver
that passes the sixteen can still fail the run, so "12 of 15 above 60"
overstated where this driver stands.

First partial baseline at 1920x1200, one process per benchmark:

| benchmark | fps | |
|---|---:|---|
| `build` x2 | 267, 271 | ok |
| `texture` nearest / linear / mipmap | 285, 284, 284 | ok, but see below |
| `shading` gouraud / blinn-phong-inf / phong / cel | 188, 120, 75, 70 | ok |
| `bump` high-poly / normals / height | 88, 180, 157 | ok |
| `effect2d` 3x3 | 79 | ok |
| `effect2d` 15-tap | 31 | BELOW |
| `pulsar` | 200 | ok |
| `desktop` blur | 38 | BELOW, and it renders its blur |

Three findings from it:

* **`desktop`'s stall used to kill the seventeen benchmarks after it.**  The
  scratch surface closed that (`parhelia-m9-fragment-pass-hardware.md` 3bc):
  the whole list of thirty-three now runs in one go with no stream timeout,
  29 of them at or above 60 fps, and `desktop` draws its blur at 38 fps.  Its
  default string is `desktop:windows=4:effect=blur:blur-radius=5:passes=1:separable=true`,
  which is what "all glmark" has to mean.
* **`bump:bump-render=normals` runs at 180 fps** where the ledger recorded it
  as refused for an indirectly indexed uniform.  It was fixed at some point
  and nobody noticed, because only the bare `bump` scene was ever swept.
  There may be more stale entries of that kind.
* **`texture:texture-filter=mipmap` reports 284 fps and is almost certainly
  wrong**: `m9_blit_convert` resampled a filtered blit NEAREST, so every
  CPU-built level is a decimation rather than a reduction.  A fast, incorrect
  result in the "ok" column - which is why a frame-rate sweep cannot answer a
  conformance question on its own.

## The scenes that were blocked

All but one now render.  They were blocked on one feature - the fragment
multipass path - and on three defects found while implementing it.

| scene | rate | state |
|---|---|---|
| `effect2d` default (1 tap, no split) | 222-231 fps | renders |
| `effect2d` 3x3 kernel (2 passes) | 86-89 fps | renders |
| `effect2d` 15-tap (5 passes) | 25-33 fps | renders, with a 141 MiB record |
| `shadow` | 143-147 fps | renders |
| `refract` (4 passes) | 1 fps at 1920x1200, 2 at 640x480 | renders; 95% of the frame is the CPU mipmap of its distance target, not fill |
| `terrain` | 21 fps at 1920x1200, INVALID | `terrain.frag` still refused for register pressure; `M9X_SPLIT_PRESSURE=1` compiles it into seven passes over **mesh** geometry (576 records a draw), which fills no box a scratch surface could be given, so those draws are refused rather than drawn and it stays off |
| `desktop` separable blur (4 passes) | 38 fps at 1920x1200 | draws its blur: the partial pass renders on a scratch surface of the pixels it covers and is copied into place (3bc).  The frame is the engine's -- 26.77 ms of which 22.84 is the submit ioctl, against 2.73 draw and 2.66 emit |

The rest of the suite was already above 60 fps and is unchanged; the slowest
were `function medium` 62, `shading cel` 70, `shading phong` 87.

## Three of these cannot reach 60 fps, and that is the fill rate

`parhelia-m9-fill-rate.md` measures one full-screen fragment operation at
**1.157 ms** per 1920x1200 screen, linear across `alu0..alu16`.  A 60 fps frame
therefore holds **14.4** such operations.

| scene | full-screen operations | ceiling |
|---|---:|---:|
| `effect2d` 15-tap | ~19 | ~46 fps |
| `terrain` tilt-shift pair | ~18 | ~50 fps |
| `desktop` 11-tap separable blur over composited windows | ~23 | ~43 fps |

These are not failures and no scheduling reaches 60 fps on them at this size.
Correct rendering at an honest rate is the deliverable.

### `refract` is not fill-bound, and that reading is withdrawn

`refract` was recorded here as fill-bound.  It is not, and the arithmetic said
so before the measurement did: four raster passes into a distance target the
driver clamps to 2048x1280 is about sixteen full-screen operations, 18.5 ms,
some 54 fps, against a measured 1 fps.

The driver's own phase profile names the missing second (MEASURED,
`M9X_PROFILE=4` at 1920x1200):

```text
m9x: 4 frames 1065.62 ms/frame: draw 8.05 emit 0.11 submit 8.08 varying 0.00
     records 0.45 ioctl 7.25 target 0.01 clear 0.00 transfer 1014.03
     | jobs 40 draws 40 ...
```

**95.2% of the frame is a CPU transfer**, and the GPU half of it is 8 ms.  Ten
draws a frame is the whole of the scene's submission cost.

The transfer is the mipmap chain of the distance target.  `scene-refract.cpp`
calls `glGenerateMipmap` on its 2048x1280 colour render target once a frame
(CAPTURED, `RefractPrivate::draw`), the driver implements no
`generate_mipmap`, and `m9_update_target` refuses a target whose level is not
zero, so the state tracker falls back to mapping the texture and building
every level on the CPU -- through the de-tiling transfer path.

The size scaling confirms it (MEASURED): at a 640x480 canvas the distance
target is 1280x960, 2.13x smaller in area than the 2048x1280 the 1920x1200
canvas clamps to, and the transfer falls from 1014 ms to 473 ms, a ratio of
2.14.  The cost is proportional to that texture's area and to nothing else.

So the ceiling for `refract` is not the fill rate; it is a per-frame CPU
readback of a render target.  Rendering the mipmap chain on the engine -- or
allowing a non-zero level as a render target so `util_gen_mipmap` can -- is
what this scene needs.  **UNTESTED**, not implemented.

## What was actually wrong, in the order it was found

Each was closed from the working side - take a passing fixture, add one
property at a time until it fails - which beat every attempt to reason from the
command stream.

1. **Index `ef` bits 10:9** carry a raster pass's place in a multi-pass
   sequence (0 only, 1 first, 2 middle, 3 last).  The driver emitted zero,
   telling the engine every pass was a complete draw, so the inter-pass record
   never bound.  CAPTURED: single-pass draws `00000000`; every two-pass case
   `00000200` then `00000600`; the five-sampler schedule
   `00000200 00000400 00000400 00000400 00000600`.
2. **A producing pass's root went to the colour output `0x2d`** instead of the
   destination `encode_lops` computes, so it was packed into the render
   target's pixel format.  A shader whose root arrives through arithmetic
   worked all along; one whose root arrives through a plain move did not.
3. **Depth must be evaluated in the first raster pass only.**  The passes of a
   split shader are one fragment pipeline in sequence and those behind inherit
   the coverage.  Restating the test in a continuation pass stops the engine -
   which is why every *windowed* scene with a splitting shader stalled, since a
   window carries a depth attachment whether the scene tests against it or not.
4. **Index `88` bits 15:4 flag register pairs the program leaves unused, and a
   pair flagged unused is one the engine reuses.**  A four-unit pass reads a
   texel out of `0x0e`, which the backend was declaring free.  Reproduced with
   single-pass fixtures, no split involved: `texhi:4:0` wrong in 37822 of
   76800 pixels, every other unit exact.
5. **Three unjustified constants**, each replaced by the real bound rather than
   raised: a 128-relocation cap (the real bound is the command image's word
   count), a fixed batch relocation array, and a 32 MiB BO size cap applied to
   every memory class.  `M9_VRAM_GPU_SIZE` was deleted outright - it matched
   BAR0's length rather than any hardware limit, and four clamps compared
   engine addresses against the aperture instead of the memory.

6. **The engine keeps per-target state in the object bound at index `be`.**
   A split draw returning to an earlier render-target shape read the state a
   target of another shape had left there, and the engine stopped.  Fixed with
   one `be` object per render-target shape, allocated beside the depth and HZ
   pair.

   The decisive evidence was CAPTURED rather than reasoned: dumping the stream
   over `splitpair:84x84:320x240:3` shows the stalling third draw's two
   submissions are **byte for byte identical** to the first draw's - command
   dwords, relocations and BO list all equal.  Nothing the driver emitted
   separated the draw that renders from the draw that stops the engine, which
   is what turned the search towards state the engine carries between jobs.
   Against the rendering `88x88` case the only differing dword in the entire
   stream is the clip/scissor word.  Dumping `be` after each job then showed
   the engine writing a mask over the first 16 KiB whose extent is
   proportional to the target's area - 306 dwords for 84x84, 334 for 88x88,
   3149 for 320x240, always contiguous from offset 0.

   Two negatives worth keeping: one `be` per *colour resource* stalls where one
   per *shape* passes, so the engine wants sharing by shape rather than
   per-surface isolation; and clearing `be` every job passes at 3 draws and
   stalls at 20, which is exactly why the probes use 20 draws - a rare event
   must not be able to hide.

## What still stalled, and what it was

**Closed 2026-09-19.**  The scene-scale fault was the driver clearing the
engine's back-end state object with a CPU `memset` while a job that bound it
was still executing: a job posted asynchronously retires only at the *next*
submission (CAPTURED from the kernel), and the scratch path's seed copy is a
draw into the very surface whose object is cleared next.  A race, which is
why the count a context survived was never sharp and why every reproduction
attempt gave a different number.  A target shape now keeps two back-end
objects and a clear swaps to the spare
(`parhelia-m9-fragment-pass-hardware.md` 3bf).

MEASURED with `M9X_SPLIT_BE=3`, one binary, `M9X_BE_SWAP` the only variable:
`conv3rep` fails 76800 of 76800 pixels and stops the engine without the swap,
and passes three runs over with it.

`desktop` renders its blur at **38 fps** with nothing refused or skipped
(MEASURED 2026-09-19, one process, no stream timeout), so the row above that
said it stops the engine is retired.  `terrain` does not stop the engine
either -- it is **refused by the fragment backend**, which is a different
problem and is stated below.

### What the scene asks for, measured rather than assumed

`M9X_DEBUG_SPLIT` over `glmark2 -b desktop --size 1920x1200` names every split
draw the scene issues (CAPTURED, 2026-09-18):

```text
m9x record: 420x420   stride 12 ->   8 MiB, new object
m9x record: 1920x1200 stride 12 -> 106 MiB, new object
m9x record: 420x420   stride 12 wants   8 MiB, kept 106 MiB
m9x record: 1920x1200 stride 12 wants 106 MiB, kept 106 MiB
... twenty-two more, strictly alternating ...
m9x: two-pass job failed (Input/output error); context lost
```

So the scene is **twenty-six split draws alternating two target shapes**,
420x420 and 1920x1200, with an eleven-tap separable blur the backend cuts into
**four raster passes** - the stride the driver logs is `4 * (passes - 1)`.  It
is not the first split draw that stops the engine, and it is not the first
frame: twenty-six of them render first.

Two things this settles without another run.  The record is **not** refused any
more - 106 MiB is allocated and kept, so the thirteen refusals of the older log
are gone.  And the scene keeps exactly **two** live target shapes, so the
number of simultaneously live shapes is not a property of `desktop` at all.

### The two properties named as untested are both retired

Against a clean checkout of this tree built and deployed under its own prefix,
`GL_RENDERER` asserted `Matrox M9` on every run (MEASURED, 2026-09-18, twenty
draws a case):

| case | what it adds | result |
| --- | --- | --- |
| `splitpair:320x240:320x240:20:5` | two passes, one shape | renders |
| `splitpair:320x240:320x240:20:7` | two passes | renders |
| `splitpair:320x240:320x240:20:9` | three passes, stride 8 | renders |
| `splitpair:320x240:320x240:20:11` | **four passes**, stride 12 | renders |
| `splitpair:1920x1200:1920x1200:20:1` | **1920x1200**, no split | renders |
| `splitpair:1920x1200:1920x1200:20:5` | 1920x1200, two passes, 35 MiB record | renders |
| `splitpair:1920x1200:1920x1200:20:11` | 1920x1200, four passes, 106 MiB record | renders |

A split draw into a target far larger than 320x240 renders, the scene's own
pass count renders, and the two together render.  Each on its own is therefore
**not** what stops the scene.

The `be` object is not the size limit either.  Its mask extent is linear in the
target's area - 306 dwords at 84x84, 334 at 88x88, 3149 at 320x240, which fits
`area / 24.495 + 20` to within three dwords (DERIVED from the dumps).  That is
367 KiB at 1920x1200 against a 1 MiB `M9_BE_BYTES`, and the object would only
overrun above about 6.4 Mpx - a real latent limit, but not this one.

### What is left, and the fixture that reaches it

What the scene has that no case above has is the **combination**: alternating
shapes *and* four passes, at the scene's own sizes, with four colour resources
sharing the window shape and a plain fill between the split draws.  One further
property comes with the alternation - the scene's first split draw is the
*small* one, so the shared inter-pass record is allocated at 8 MiB and then
**replaced** at 106 MiB under jobs already submitted.

`tools/m9-mesa-glsl.c` now reaches every one of these, and the ladder to walk
in one boot is, in this order so that each control is reached before the step
expected to stall:

```text
splitpair:160x120:320x240:20:5     alternating shapes, two passes
splitpair:160x120:320x240:20:11    alternating shapes, four passes
splitpair:1920x1200:420x420:20:11  the scene's shapes, record allocated once
splitpair:420x420:1920x1200:20:11  the scene's shapes, record replaced
splitdesk:2:1:11:320x240           a plain fill between the split draws
splitdesk:2:4:11:320x240           four colour resources on one shape
splitdesk:3:4:11:1920x1200         the scene
```

`splitdesk:frames:windows:taps:WxH` is the desktop frame at any scale; at
`1920x1200` its windows are 420x420, the size the scene itself uses.

## Readings this retires

* `effect2d` at 640x480 rendering clean does **not** refute the 3x3 kernel
  stalling there: the default kernel is one tap and never splits.  Two agents
  reported those as contradictory results for the same scene.
* `refract` was reported as stalling.  It does not - it had simply never been
  measured, and completes with no timeout, no skipped draws and no lost
  context.  It was, however, drawing 156 of its 208998 records until
  `98be636`, so "completes with no skipped draws" was true and still meant a
  black screen.
* `desktop`'s 37 fps was never the 4-pass path: its log showed the 106 MiB
  record refused thirteen times, so that number was the fallback.
* Fixing the `be` per-shape stall was expected to close `desktop` and
  `terrain`.  It did not - both still stop the engine, which is how the second
  scene-scale fault was found.
* `desktop` was recorded as faulting on the **first frame**.  It does not: the
  capture above shows twenty-six split draws rendering first.
* The number of simultaneously live render-target shapes was recorded as an
  untested scene-scale property.  It is not a scene property: `desktop` keeps
  two shapes, which `splitpair` already covers.
* A split draw into a target far larger than 320x240 was the other untested
  property.  It renders, at two and at four passes.
* `jellyfish` and `ideas` are 117 and 108 fps here; their 1 fps on the 481 MB
  host was host memory, not a driver defect.
