# M9 FAA native testing, 2026-10-06

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

## 2026-10-08 latest: dual-texture FAA triangle renders correctly

DRM3f93002 derives the final rasterSCREEN/CLIP reader order from the actual
position producer. Authentic original XYZ vsXYZRHW MODULATE andSELECTARG2
controls prove opposite reader order. Our GPU-transformed liveXYZ route
had retained the XYZRHW reader order, causing one wrong originpixel despite
correct intermediate GPU positions. The native fix preserves both modes,
liveowner addresses and GPUtransforms.

One lockedphysicalbatch completesboth320x240controls, independentINITb0/
Lock200/Unlock220, generation0/queue0/wedged0. Both independentpixeloracles
PASS:9801trianglepixels,9361fullinterior+440coverageedges; correctdualtexture
MODULATE andCOLOR0-onlycontrol. Fullrawproof/images committedRE1d7b0202.
[Proof and screenshots](../tests/m9-faa-dual-xyz-source-20261008/hardware-20261008/README.md).

Quake3FAA remains open: liveapp bridge is stillFIRST-only,320x240,atmost3
sameprogram/sametexturepairdraws. Laterdualreuse, dynamicpair/program,
mips/effects/generaldimensionsneed source-qualified producers and HW proof.
The FAAgears48.3–48.7FPS milestone remains the lastqualified1024x768 result.
CLEAR_RESUME's strict authenticated appended-log receipt and realoriginal
Lock340 CPU return are closedinRE34f33214; zeroCLEARviews were transport-only.
Historical single-origin and receipt-open entries below are superseded here.

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

## 2026-10-08: FAA glxgears reaches48.3–48.7 FPS at1024×768

The physical M9128 completes512 original glxgears frames with GPU transforms,
FAA16x and GPU presentation. Clean windows measure48.262/48.714 FPS; the
requested >30 FPS glxgears milestone is verified. All512 twelve-draw batches,
GPU Unlocks and presentations retire; final submitted/completed ae2f0,
generation0, wedged0, empty queue. Independent actual scanout captures at
frames1/512 show all three gears and match the corresponding live destinations.
X PageFlip=true, hidden cursor and verified GLX interval1 remain enabled.
[Proof, current screenshot and artifacts](../tests/m9-faa-fullscreen-direct-bench-20261008/cache-preflight512/README.md).

DRMf97ca5d retains earlier firmware/stage/mapping optimizations and removes
only the redundant count2 attachment scan before private staging. Mesac9be273
retains compiled VS programs within immutable shader state; per-draw CB,
viewport, geometry and owned uploads remain fresh. Compared with the previous
29.719/29.840/30.023 FPS run this combined change is about63% faster. There
is no matched current M9128 no-FAA FPS. The older527 FPS belongs to M9148.

Quake3 GL1 FAA is still open. Its corrected current1108B dual-texture singleton
retires Lock200/Unlock220 with healthy transport but collapses to one wrong
pixel. GPU COLOR/UV/clip/screen planes are correct; zero clip codes and the
SoA-packed EE19 view are expected, not established causes. A real padded
texture extent bug is fixed (64×64 BGRA allocation0x4400 rather than0x4000)
and its clean-boot retest still has the identical pixel failure, ruling out
that allocation bug as the raster cause.
[Both hardware cases](../tests/m9-faa-dual-first-depth-20261008/hardware-current-20261008/README.md). Disabled-depth and
bounded dual batching runtime are now integrated and CPU/source qualified.
DRMb88ba44/Mesa6d69e7b derive seven reset dimension fields from live320/1024
layouts, including source MAIN backing extent rather than page-rounded GEM
capacity. Actual native/owned-client/reset proofs pass. Later CLEAR3 after
embedded-HZ history remains gated at the unread original query. The1024
mixed-depth hardware scene is queued; independent llvmpipe reference passes.
Five clean-boot dual hardware cases retire healthy Lock/Unlock but still
fail with the same one incorrect pixel; both full CPU texture views verify.
The corrected canonicalDRM8b5e25f observer reads sampled owner1 after Lock,
before resolve: exactly one nonzero origin DWORD10204080. Thus final resolve
does not collapse an otherwise covered triangle. MAIN/AUX still refuse
placement; their actual borrowed address-domain readback needs correction.
CanonicalMesa6b03281 now owns live dual-texture NIR, resource/sampler snapshots,
queue uploads and exact FIRST1/2/3 receipts. Actual-function ASAN/UBSAN proof
and complete23-object driver build pass; dispatch remains diagnostic-gated
until pixels qualify. DRM66a7d75/RE68e94daf integrate the source-qualified
later-clear prefix (nine ENDs, no invented unread query completion); its
native retained transaction is in progress. No new no-FAA glmark2/piglit/
Quake3 GL2 application result is claimed; a same-boot regression is queued.

## October 8: continuous GPU-presented FAA glxgears passes

DRM a86fcdc / Mesa3c3e1f3 completes96 real glxgears frames with GPU transforms
and GPU presentation:12 draws/frame, three primary and six secondary buffer
rotations, final submitted/completed20b10, generation0, wedged0, empty queue.
Frame1 exactly matches the CPU-presented reference. Short uninterrupted
windows report6.9–7.3 FPS, versus roughly5 FPS previously; this is a ballpark
comparison with diagnostic logging, not a controlled benchmark. Rendering is
320x240 on a1024x768 display, vblank_mode=0.
[Proof and screenshots](../tests/m9-faa-gpu-present-hardware-20261008/continuous96/README.md).

GPU presentation is now the default for this qualified destination within
opt-in FAA; `M9X_FAA_GPU_PRESENT=0` retains the CPU path. Larger dimensions and
textured Quake3 still need implementation and hardware validation. The dual
texture vendor FIRST DRAW-to-Lock flow is now captured completely through the
unread Lock boundary, with no fabricated completion; its native constructor
is in progress. [Dual source flow](../tests/m9-faa-dual-first-depth-20261008/README.md).


## October 8: all FAA depth comparisons and GPU presentation pass hardware

DRMa86fcdc/Mesa3c3e1f3 passes three144-draw frames covering all8 comparison
functions, three incoming depth relations and both write masks. All48 cases
pass the analytic pixel oracle: zero bad pixels among46848 checked per frame
with RGB tolerance8. GPU transforms, two secondary pool rotations, and GPU
presentation all complete. Final submitted/completed a5a0, generation0,
wedged0, empty queue. The outer ioctl now authenticates presentation origin
and maintenance instead of rejecting the correctly retired next frame.
[Hardware matrix, screenshots and proof](../tests/m9-faa-depth-comparison-scene-20261008/hardware/README.md).

This qualifies enabled comparison/write state and repeated GPU presentation
at320x240, not textured Quake3, disabled-depth/blend/stencil, or1024x768 FAA.
There is no timing claim for these paused screenshots.

## October 8: first GPU-presented FAA frame matches the working screenshot

Combined DRM1504380/Mesa3c3e1f3 physically retires first Lock5e0, Unlock600
and GPU presentation620. The actual320x240 glxgears screenshot is byte-identical
to the working CPU-presented reference. The next frame retires Lockb50 with
no wedge, but the outer batch ioctl still checks its result against U600
instead of presentation P620 and returns EIO. This is an identified completion
receipt publication bug; no GPU timeout occurred. Fixing that final origin join
and adding actual ioctl coverage precedes the next continuous test.
[Hardware evidence and screenshot](../tests/m9-faa-gpu-present-hardware-20261008/first-frame/README.md).

## October 8: live depth state and GPU presentation integrated, hardware pending

DRM1504380 and Mesa7730b80 fix an API/state mismatch: FAA accepted GL_LESS
but emitted vendor LEQUAL (`38038f`). The original comparison jump table and
validator now prove all eight compare codes/write masks; complete original
three-DRAW frames prove LESS/write (`38038b`), LEQUAL/write (`38038f`) and
ALWAYS/no-write (`38039d`) change only A8, with identical primary/programs.
[Source evidence](../tests/m9-faa-depth-state-source-20261008/README.md).
Per-draw state is owned and encoded through FIRST/initialized count2/count4;
disabled depth remains gated because its shader/EF route differs.

DRMb240d60/4f13da2 and Mesaff19d34/5133bb6 integrate opt-in
`M9X_FAA_GPU_PRESENT=1`. Live destination owners, mapping, fences and separate
Lock/Unlock/presentation receipts are retained through retirement or failure.
The next frame starts after the real presentation point; pool maintenance
preserves the immutable Unlock history. CPU lifecycle/receipt/pool tests and
the combined full module/20-object DSO build pass. GPU presentation pixels
and speed are still pending the combined target run; CPU presentation remains
the default. The earlier96-frame geometry proof remains the hardware baseline.

Mesac7e399b/81d0bdc/3c3e1f3 adds an executable live dual-UV carry/gather,
source-exact72B dual-MODULATE FS, and owned four-vector shader upload. Native
textured admission is still false until the complete secondary/texture-owner
submission is connected. Full-resolution INIT source is closed; larger
clear/draw/Lock joins remain in progress. No Quake3 FAA pass is claimed.




Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0-only


## October 8: large FAA batches pass64/96/176 draws on hardware

DRM8ea85e0 and Mesa585acba replace the old32-draw cap with a ceiling derived
from command-owner capacity and exact per-job size checks. A live GL grid now
renders64,96,176 separately transformed/lit draws in three complete frames,
including secondary rotation and crossing the old32KiB primary boundary.
All triangle centers have the expected colors; no reference interior pixels
are missing, maximum interior RGB error1. Final Unlock8130, generation0,
wedged0, empty queue. [Hardware proof and images](../tests/m9-faa-large-batches-20261008/README.md).

Quake3 still needs textured shader/UV topology, additional raster state and
large-frame data/code streaming. Source-qualified live texture ownership is
integrated (DRM7fff20c); whole dual-texture source image is now sealed.
GPU copy/finish source and pure encoder are integrated (DRM95e2178), with
runtime presentation in progress. Mesa5c60668 derives FAA-specific HZ and
companion backing sizes; original1024x768 INIT comparison is closed, but
runtime dimensions still need the complete clear/Lock/view transition.

## October 8: both FAA paths now pass continuous glxgears

The source two-stage path now completes96 actual GPU frames too, with3 primary
and6 secondary command-pool rotations, final Lock1fef0/Unlock1ff10 and healthy
empty queue (generation0,wedged0). Its first screenshot is byte-identical to
the four-stage result: zero missing reference interior pixels. Both preserve
GPU transforms and render all12 mixed raw-input draws/frame.
[Two-stage hardware proof and screenshots](../tests/m9-faa-glxgears-first-hardware-20261008/continuous96-two-stage/README.md).

DRM9543312 and Mesa1aa8d4d implement actual retained maintenance receipts and
pool reuse. Mesa2b25e0f selects the hardware-qualified two-stage flow by default
within opt-in FAA; M9X_FAA_FUSED=0 retains four-stage comparison. Original FF
producer tests cover default/explicit2 and explicit4, with10752 ISA comparisons. Current320x240 diagnostic performance is roughly5 FPS; short
windows do not establish a two-stage speedup. Application resolution remains
320x240 on a1024x768 display. Work now covers1024x768 live layouts, Quake3
texture ownership/shaders/state and larger batches, plus GPU presentation.
Full Quake3 FAA and unrestricted resolution remain unverified.

## October 8: continuous four-stage FAA glxgears passes96 frames

DRM b011d51 completes96 real12-draw FAA frames on M9128, including3 primary
and11 secondary command-pool rotations. Submitted/completed end at31f60;
generation0, wedged0, queue empty after cleanup. The first image retains zero
missing reference interior pixels; screenshots1/2/3/96 show complete gears.
[Hardware proof and images](../tests/m9-faa-glxgears-first-hardware-20261008/continuous96-owner-bound-fixed/README.md).

The old32 KiB FAA END cap is physically fixed. At320x240 on a1024x768 display,
two uninterrupted application windows report5.042/5.066 FPS (vblank_mode=0).
This is a diagnostic-path ballpark with remaining logging and CPU presentation,
not a1024x768 benchmark. Paused first-frame timings are excluded. Corrected
two-stage pool reuse, arbitrary dimensions and Quake3 FAA textures remain open.

## October 8: frame-4 command bound identified; fix awaiting hardware

The extended run failed on frame4 after three complete FAA frames. Returning
from Swap96 did not mean96 rendered frames: final submitted/completed remained
19a0 with generation1/wedged1. Source review found a stale32 KiB ordinary-command
limit rejecting an END within the authenticated256 KiB FAA owner. Frame3 ends
at32168 bytes; frame4 posts early clear commands then crosses that old bound.
DRM b011d51 fixes the FAA-only bound and adds actual-C boundary coverage.
This is a partial-submission rejection; a GPU timeout was not established.
[Failure capsule](../tests/m9-faa-glxgears-first-hardware-20261008/frame4-command-bound-failure/README.md).

DRM3cd00f3/e8f327c/973ce66 integrates source-derived initialized two-stage
encoding and native dispatch, with clear/no-clear epochs, mixed declarations,
same-profile validation and CPU tests. Repeated two-stage hardware validation
and command-pool maintenance remain pending. Quake3 FAA textures remain open.

## October 8: raw-input count fix restores FAA glxgears geometry

DRM02c2d52 derives raw register22 bits12:8 from the live declaration half
count `(declaration_count - 1) << 8`, in both the two- and four-stage heads.
Previously count2 retained one-attribute8020e and count4 two-attribute8040e
regardless of the actual input. Original vendor packer/merge/writer evidence
and complete decl3/5 source equality are [sealed here](../tests/m9-faa-raw-input-control-source-20261008/README.md).

Physical four-stage glxgears now renders all reference interior geometry:
missing pixels0/26548, down from5138. Three12-draw FAA frames reach
8e0/900,1130/1150,1980/19a0 with healthy empty queue, generation0 and no
wedge. First image29038 nonblack pixels,730 RGB colors;79 interior pixels
have a different dominant channel. [Screenshots and exact hardware proof](../tests/m9-faa-glxgears-first-hardware-20261008/raw-input-control-fixed/README.md).
This verifies320x240 FAA geometry on a1024x768 display, not unrestricted
resolution or fine pixel/depth-edge conformance. Paused timings are not FPS.

The first run stopped at3 after its host-review window expired; the96-frame
command-pool reuse check is being run separately from a fresh GPU context.
Corrected count2 rendering and initialized-frame native integration are
still pending hardware qualification. Quake3 FAA textures remain open.

## October 8: source two-stage application route under integration

Original M9128 one-COLOR shaders use a fused two-stage route. The new
[source FIRST/depth batch proof](../tests/m9-faa-first-depth-two-source-20261008/README.md)
covers one, two and three genuine DRAWs before unread normal Lock and proves
INIT+0x110+(N-1)*0x60. It supplies no invented completion or rendered pixels.

Mesa 61c0997/16109df adds an explicit `M9X_FAA_FUSED=1` candidate with GPU
transforms, live shader constants and checked FIRST singleton/batch receipts.
DRM 452e846/423e5a9 adds two-stage code placement, actual shader availability
and live data bindings. DRM 8fb31ab integrates source-derived FIRST/append
composition, checked at N1/N2/N3 and mixed declarations through N32. Native
FIRST batch wiring and live prefix alignment support landed in ea1c091.
The four-stage route stays available.

DRM 625ca05/830d3da fixes two incorrect generalizations from the old tiny
control: secondary +296 is the literal C2 packet count 3, never vertex count;
+164/+168/+172/+176 stay 32 even with 16-byte raw records. Only +292 is the
raw fetch stride. [Original M9128 stride proof](../tests/m9-faa-fused-stride-source-20261008/README.md)
and [flat/cull state proof](../tests/m9-faa-first-depth-two-state-20261008/README.md)
are integrated. These are host/source checks, not hardware completion.

Mesa f051299 fixes duplicate depth conversion and the missing four-DWORD
prefix offset in the candidate viewport constants. Emitted head and geometry
tail checks now cover clip coordinates, screen X/Y and window depth (10,752
component comparisons plus depth boundary controls). Original DLL metadata
calculation agrees with the actual fused masks bc00/bf00. Clip-code arithmetic
is not modeled; unchanged ordinary-M9 tail bytes are not FAA pixel proof.
Native ea1c091 is now hardware-tested: all12 original glxgears draws reach
Lock5e0/Unlock600 without a wedge. The image is still incomplete:23409
nonblack pixels,4684 missing reference interior pixels,110 wrong dominant
interior colors. [Exact hardware capsule and screenshot](../tests/m9-faa-glxgears-first-hardware-20261008/fused2-first-incomplete/README.md).
The final linear COLOR/clip/window samples match draw9, rather than the
last NORMAL-array draws10/11. This identifies sampled values, not proof
that later draws were skipped. Both additional raw attributes and nonzero
shader slots remain under investigation. [Reproducible upload analysis](../tests/m9-faa-fused-first-analysis-20261008/README.md).

Authentic FIRST12 completion permits the [original initialized count2
transition proof](../tests/m9-faa-first-depth-two-later-source-20261008/README.md):
clear3 +110, no-clear +90, each extra draw +60. Mesa55d69c7 implements
initialized clear3 batch receipt handling; dedicated native later2
integration is in progress. Count2 command-pool maintenance is still gated.
No continuous FAA glxgears pass, Quake3 FAA pass, or FAA FPS claim.

## October 8: true16 constant-normal control retains the same GPU misindex

The current runtime completes constant NORMAL / NORMAL-array / constant
NORMAL at 360 vertices without a wedge. The array frame is solid. The last
constant frame still has 8,488 pixels and exactly the same eight sampled
wrong clip indices as the older padded32 control. The first four clip
records are correct; a simple uniform index division does not explain it.
[Current hardware and ISA evidence](../tests/m9-faa-glxgears-first-hardware-20261008/constant-normal-dense16/README.md).

Input padding is ruled out as the cause of this persistent failure. The
original M9128 finalizer also reproduces all three real carry bodies exactly,
including their output MOVs and finish scheduling. Current work checks the
stage interface and original one-COLOR two-stage path; complete glxgears
FAA remains unproven. Hardware stays healthy at500; paused timing is not FPS.

## October 8: dense16 integration retires, but FAA gears still lose faces

DRM dd84e6c and Mesa 0c62924 complete three original glxgears FAA frames
without a wedge. Actual POSITION-only records now use 16 bytes; the input
arena shrinks from 91,392 to 68,224 bytes. Faces still disappear: the first
frame misses 5,138 of 26,548 software-reference interior pixels.
[Full hardware capsule](../tests/m9-faa-glxgears-first-hardware-20261008/dense16-still-fragmented/README.md).

Dense records and the corrected context admission are integrated and retained.
They did not finish the rendering fix. The next diagnostic compares constant
NORMAL and a NORMAL array on this exact runtime and inspects linear GPU
output to locate the remaining indexing error. CPU ISA checks do not prove
hardware packed-output semantics. Command-pool rotation is implemented and
CPU-tested, but its optional 96-frame physical test was not run after these
images failed review. No complete FAA glxgears, Quake3 FAA or valid FAA FPS.

## October 8: missing faces isolated to dense POSITION-only record handling

A genuine OpenGL360-vertex front-face diagnostic renders identical software
images with constant NORMAL and an enabled NORMAL array. On M9128, the
array frame is solid while both constant-NORMAL frames lose triangles; all
three retire without a wedge. [Exact diagnostic and GPU output evidence](../tests/m9-faa-glxgears-first-hardware-20261008/constant-normal-isolation/README.md).

Array-path GPU coordinates match their intended CPU ISA indices. Constant
NORMAL coordinates match other actual vertices, often roughly half the
requested index. The fault is vertex indexing, not a lighting arithmetic
error or a requirement to have multiple jobs in a batch. Original vendor
[high-count and16-byte stride controls](../tests/m9-faa-high-count-source-20261008/README.md) prove dense POSITION-only records are supported.

Mesa9eed7ca removes artificial16-to32-byte padding; DRMf4a7a1c preserves live
stride through admission and arena allocation. Pure encoder support and the
combined hardware qualification are in progress. Command-pool rotation is
integrated as DRM2869cb5 with CPU lifetime/retirement tests, pending physical
rotation proof. No complete FAA glxgears or Quake3 rendering claim yet.

## October 8: live shader mask restores colored glxgears, missing faces remain

Canonical DRM e7712f7 derives carry register availability from the actual
shader instead of hardcoding ffff; test b7ab0fd covers real lighting bodies.
On M9128, three FAA glxgears frames now contain15091/19905/19887 nonblack
pixels and complete without a wedge. The prior candidate produced allzero
resolved rows. [Inputs, screenshots and hardware evidence](../tests/m9-faa-glxgears-first-hardware-20261008/carry-mask-colored/README.md).

The gears are recognizable but many triangular faces are missing against
the exact software reference. This is partial rendering, not complete FAA
glxgears support. Topology/culling and actual GPU shader outputs are under
review; continuous pool rotation and textured Quake3 remain unfinished.
Three paused320x240 frames provide no valid FPS measurement.

## October 8: three glxgears FAA frames retire; black output isolated before presentation

The combined native/runtime-mapping candidate now completes three real
frames without preparation failure or wedge: Lock/Unlock8e0/900,
1130/1150,1980/19a0. Every frame contains12 jobs. Canonical DRMf595ad0 plus
2a7d35d preserve used attachment metadata while validating live mappings.
[Exact input, intermediate and pixel evidence](../tests/m9-faa-glxgears-first-hardware-20261008/three-retired-black/README.md).

The resolved F021 rows are allzero before the application destination write;
X presentation is therefore not the cause of the black image. Intermediate
GPU outputs exist, but gathered COLOR values do not match the plausible
colors produced by the ISA model from captured inputs. Packet bindings,
constant ranges and output routing are under review. Three completed black
frames are not a glxgears rendering pass, and checkpoint timings are not FPS.
Command-pool rotation and textured submission remain unfinished.

## October 8: full glxgears FIRST retires; pixels and frame reuse still fail

DRMff3696f removes the false address check on vendor transition controls and
integrates initialized mixed-layout/flat/cull composition. On the actual
M9128, all12 glxgears FIRST jobs now execute: INITb0 -> Lock8e0 -> Unlock900.
The captured320x240 window is entirely black, so this is not a rendering pass.
SECOND frame preparation returns EFAULT before submission; the target stays
healthy at900. [Hardware evidence](../tests/m9-faa-glxgears-first-hardware-20261008/first-retired-black/README.md).

Next work separates the F021 readback from application presentation and
checks post-render mapping metadata during frame reuse. Strict initial
mapping publication and live mapping identity must not be conflated. The
actual-submit CPU regression (DRM7de4237) proves three ordinary SYSTEM
placements are independent of the control literals while six real FAA-owner
PTE corruptions still refuse before any upload or END.

Texture binding words are now derived from retained live owners in DRM6c9b985,
with CPU source/placement/refusal checks. Full texture submission and command
pool rotation remain unfinished. No valid FAA glxgears screenshot, continuous
run, Quake3 FAA result or FAA FPS measurement yet.

## October 8: glxgears mixed batch prepares; control words misclassified as addresses block submission

The latest target run accepts all21 glxgears draws and prepares12 native FAA
jobs (91,392 input bytes). Mesa7cebd8e and DRM8c0f6c1 resolve the clear/layout
and mixed declaration3/5 preparation blockers. Submission returns ESTALE before
code upload or any geometry END; INITb0 remains healthy through cleanup.
[Exact hardware evidence](../tests/m9-faa-glxgears-first-hardware-20261008/mixed-layout-submit-refusal/README.md).

The actual ordinary SYSTEM allocation is at GPUVA0x40047000, while submission
incorrectly requires fixed command controls0x40004000 and0x40008000 to fall
inside that allocation. The first such admission necessarily rejects the
frame. The original M9128 DLL writes both values as instruction immediates;
existing transition/query decoding also treats them as controls. Rebasing
these words from the ordinary SYSTEM owner would be unsupported. The fix
removes their false address classification while retaining checks for actual
address-bearing fields and live owners. Hardware qualification is pending.

Initialized mixed-layout/flat/cull support and command-pool rotation remain
under integration. Texture fragment code is CPU-qualified; native texture
binding remains gated. There is no valid FAA glxgears screenshot, continuous
run, Quake3 FAA result or FAA FPS measurement yet.

## October 8: mixed flat/smooth FAA now passes hardware

The next single hardware run passes flat368 →smooth400 →flat368 with the
native deduplicated instruction page: INITb0/Lock340/F021/Unlock360, healthy
delayed and cleanup snapshots. The independent pixel oracle passes23,492
interiors,7,697 overlaps,zero errors and777 partial pixels; the raw image is
identical to the previous all-flat pass. [Exact mixed-program evidence](../tests/m9-faa-first-batch-hardware-20261008/mixed-pass/README.md).
These constant-color inputs do not qualify varying-color interpolation.

Mesa36d9fc8 now tracks authenticated initialized-depth state and explicit
FRAME_INITIALIZEDbit2, preserving flags0's old +1a0 contract. Its full20-object
build and client checks pass. Pool primary/secondary managers share one
actual-completion timeline (DRMb4b6df0/e321d0e); native allocation/publication
hooks remain under implementation. The80-byte BATCH ABI now names a separate
maintenance_point output (db01829), so real pool maintenance cannot silently
rewrite the previous application Unlock history. No repeated-frame/glxgears
hardware pass yet.

## October 8: repaired flat/CW multi-draw FAA passes hardware

The34-owner correction now passes the exact three-draw test that previously
lost the machine: INITb0 →Lock340/read/Unlock360, all14/17 ENDs retired,
generation0/queue0/wedged0 through cleanup. Independent pixels pass23,492
interiors,7,697 depth-overlap checks,zero errors and777 partial-coverage
pixels. [Hardware receipts and actual image](../tests/m9-faa-first-batch-hardware-20261008/ownerfix-pass/README.md).
Frozen5dcaa82 contains the owner fix and WC barrier; canonical equivalents
are6170da1 and2717bf4. The preceding failure was a reproduced kernel stack
overwrite before batch submission. Its test stub had hidden the capacity
mismatch; the test now calls the actual production fill.

This is one same-program flat/CW frame, not continuous glxgears. Mixed shader
native/Mesa code is integrated but awaits hardware. Initialized later-frame
integration and observed-retirement command-pool rotation remain active.

## October 8: batch failure traced to CPU stack overwrite

A zero-delay phase trace with the WC-publication fix reaches INITb0 and
successful first-frame preparation, then loses the host before batch
composition or any batch END. Source inspection found a concrete kernel
stack overwrite: the batch allocated31 owner descriptors but the production
fill writes34, including depth attachments. DRM `6170da1` fixes the capacity;
the updated test uses the real fill and reproduces the old overflow under
ASan. Its earlier31-role stub had masked this bug. The WC barrier correction
`2717bf4` is retained independently. The corrected hardware test is pending.
[Host-streamed phase evidence](../tests/m9-faa-first-batch-hardware-20261008/wc-trace-failure/README.md).
Do not characterize this observed failure as a proved GPU-command wedge.

Initialized later-frame source controls are integrated as RE `42361ee2`:
clear3/depth7 reuses bound attachments and reaches unread Lock370 from
native Unlock220; no-clear reaches Lock2f0. The clear prefix includes a
separate existing-sentinel operation. Native integration and pool rotation
remain open; glxgears FAA is not yet verified.

## October 8: combined flat/CW batch fails after successful INIT

The frozen native candidate `d2a5c03` (root `b1e5ba3`) now pairs flat32
programs with their full fragment metadata and common cull state. Mesa
`970d205` forwards those profiles. Host checks and full builds pass, but the
physical M9128 test **fails**: INIT retires at b0, then three flat368/CW draws
with distinct CB colors and overlapping depth enter FRAME_BATCH; the target
becomes unreachable before the expected Lock340 or any pixel readback.
The exact failing GPU END is not yet known. [Candidate hashes, inputs,
runner and host-streamed failure](../tests/m9-faa-first-batch-hardware-20261008/README.md).
The earlier single smooth six-vertex FIRST pass remains valid and narrower.

An initial unobserved failure was repeated once with durable staging and
host-streamed phase markers; the repeat establishes the batch transaction
as the boundary. No optimization or new feature was reverted. Reviews have
not found a concrete C3 relocation or input-lifetime defect. A complete
original flat/CW N1/N2/N3 comparison now passes against frozen actual C
(`d8d50832`), including whole primary/secondary, owner placement and all
fourteen ENDs. It does not qualify the live one-output hardware adaptation.
The recovered target is on boot4640313a with no candidate module loaded;
opt-in submission phase tracing is being added to locate the failing END.

Separate progress remains host-only: native immutable code-slot/C2 lowering
`e68f6be`, executable counted-pool ownership `2b3e755`, retired CPU-frame
reclamation `9282eee`, and flat-interface classification compatibility
`1d977db`. Mixed-program native execution is now integrated as `89e9b86` with an
owned preloaded instruction page and per-job C2/flat metadata; its host
mixed400/368/832/1024 producer and failure tests pass. It has no new GPU
qualification. Repeated initialized-depth frames and sustained pool wiring
remain open. Glxgears FAA and later Quake3
GL1, glmark2, piglet and Quake3 GL2 objectives are not complete.

## October 8: native frame batches and actual flat shader frontend

DRM `2520b48` integrates FIRST FRAME_BATCH execution for 1..32 same-program
draws. Every draw retains separate CB/header/raw inputs; the final Lock is
INIT+0x150+(N-1)*0xa0 with fourteen END posts. Source N2/N3 comparisons,
actual preparation N1..32, and actual submission/failure host checks pass.
Hardware qualification of multiple draws is pending. Initially admitted
batches use full black/depth-one clear, LESS/write, smooth no-cull state.

Mesa `e0083b7` sends queued application frames to that typed batch API with
one final presentation and authenticated count-derived receipt. `f675b1a`
fixes a concrete flat-shading bug: the live frontend previously emitted the
smooth64 shader for flat state. It now emits the original flat32 ISA and
utility ordering, including legacy fixed-function interpolation. Smooth
uploads remain unchanged; GPU transformation and lighting remain in the
carry shader. The full twenty-object Mesa build and live/fixed-function
host checks pass. Native flat metadata pairing and hardware pixels remain
pending; shader byte length alone is not sufficient admission.

The original FIRST source proves the corresponding compiler usage mask,
EF contribution and varying interpolation metadata. Its earlier retained
flat control had reused a cached compiled shader, so the raster-bit-only
inference was incomplete. See [FIRST raster/metadata proof](parhelia-m9-faa-first-raster-controls.md).
Continuous glxgears still needs flat/cull batches, real program switching,
initialized-depth later frames, and observed-retirement-based pool reuse.
[Counted pool source proof](../tests/m9-faa-pool-counted-source-20261008/README.md)
now distinguishes pending CPU references, submitted pool points and actual
GPU completion; it does not authorize recycling from submission alone.

## October 8: fresh FIRST clear/depth also passes physical M9128

DRM `c1962dc` (sealed candidate `43d80b0`) now runs directly from authentic
INITb0 through full sampled color/depth reset and a live single-COLOR GPU
program with six vertices: Lock200/readF021/Unlock220. All14 initial ENDs
and17 after Unlock retire; delayed/cleanup snapshots remain queue0/wedged0.
The image passes12,302 colored/638 partial pixels and9,936 interior checks
with zero pixel errors. Its entire raw readback is byte-identical to the
previous passing retained full-clear image. [Exact hardware evidence and
screenshot](../tests/m9-faa-first-depth-hardware-20261008/README.md).

Mesa `617e812` owns all draw snapshots until flush/readback, target change,
or a later clear, instead of presenting each draw separately. `63a0835`
authenticates FIRST+150 and retained+1a0 depth receipts with exact metadata
and an independent wait. The full20-object driver build and focused host
checks pass. Actual directional/positional fixed-function uploads also pass
the fresh native encoder on CPU; this is not GPU lighting qualification.

Glxgears FAA remains open: FIRST flat/cull, complete native multiple-draw
composition for live application shaders, initialized-depth later frames,
and retirement-aware command-pool reuse. The corrected pure appender
`af5137c` compares the whole composed primary to vendor output; its earlier
footer-only version was incomplete and was never posted. At that earlier checkpoint native batch
execution remained gated; the new host-tested integration is described above. Pool RE proves primary rotation through an unused
slot and a separate pool-counter reuse dependency; Unlock alone does not
authorize rewinding active pools. See [pool contract](parhelia-m9-faa-pool-reuse-contract.md).
No glxgears/Quake3 screenshot, FPS, or sustained rendering claim yet.

## October 8: full sampled clear/depth FRAME passes physical M9128

DRM `cf43aa2` completes live first3 Lock190/read/Unlock1b0, then full black
color/depth-one clear plus LESS/write and changed live6 Lock350/read/Unlock370.
The single hardware run and cleanup stay generation0/queue0/wedged0.
The next image passes all checked interior/exterior/channel tests; **exactly
six pixels differ from the previous failing image, removing all six stale
old-color edge contributions**. It contains12,302 colored/638 partial pixels
and9,936 checked interiors. [Hardware receipts, raw data and screenshot](../tests/m9-faa-depth-frame-hardware-20261008/README.md).

The new COLOR attachment prefix derives live page masks and its distinct
source prologue; the draw suffix derives live shader/CB/geometry and the
actual retained queue epoch. Flat/CW/CCW packet construction matches six
complete vendor source controls, but those raster variants have no new
hardware qualification. Real Mesa directional and positional fixed-function
lighting uploads also pass the native depth encoder's host tests.

At this earlier retained-frame checkpoint, fresh FIRST, multiple draws
and sustained pool reuse were still open. FIRST is now superseded by the
hardware pass above; the remaining application gaps are listed there. Original vendor FIRST now passes the
finite25270 dependency-query branch without a fabricated completion; its
normal Lock path is being integrated. Separate source DP2 callers reuse bound
attachments within a frame without clearing, so application submission must
preserve that frame lifetime. The older six-stale-pixel results below are
historical and superseded by this full-clear hardware test.

## October 7: genuine retained flow executes; same six stale edges remain

DRM `6c3fdd4` replaces the reused first-draw secondary with the actual vendor
retained command structure (primary3624/secondary2536 for two declaration
groups), preserving live resource and shader derivation. Independent source
comparisons pass128 placement/state/count/declaration cases. One complete
hardware batch retires first3 → clear → next6 → both Lock/read/Unlock cycles
and cleanup with generation0/queue0/wedged0. Both images are byte-identical
to the prior build: first image passes; second still has six stale old-color
edge samples. The correction is retained; full FAA sample reset is open.
[Exact hardware evidence](../tests/m9-faa-retained-flow-hardware-20261007/README.md).

Mesa `0dba5b2` preserves the retained graph across same-layout back/front
retargets and requires a real clear for a new destination. `dcc14df`/`5e6f59c`
add flat COLOR frontend/topology and correct FIRST/LAST provoking vertices.
`9fa80ea` uses the authenticated160-byte FRAME ABI for both first and repeated
application draws. `14f1fbe` combines Mesa fixed-function lighting's split RGB
and alpha stores on an owned NIR clone: original Mesa directional/positional
producer controls pass4,096 independent GPU transform/lighting components,
within existing instruction/constant limits. These are host proofs; actual
glxgears has not passed. Full clear, depth and native flat/cull state are still
gated pending their complete source-derived implementation and hardware test.
DRM `a918988`/`376cee8` prepare bound attachments from authentic INIT and encode
live HZ/depth clear bodies; the complete bound clear/draw sequence is unfinished.

The subsequent clear implementation is now committed: DRM `6df69cd` emits
complete sampled b5e70 (3516/3520 bytes with live page masks), and `48fe227`
emits bound attachment/HZ/depth setup (1160/1164 bytes and five ENDs).
Both pass complete source comparison, varying placements/alignments and
atomic-refusal host checks. This code has **not** been hardware-tested yet;
the stale-edge hardware result above predates these full clear producers.
The apparent pointer0x40888889 is proved to be an immutable constructor float,
not an allocation address; [source evidence](../tests/m9-faa-gl-one-color-20261007/sampled-clear/README.md).

Mesa `363c0f3` also admits the original fixed-function fragment COLOR0 legacy
interpolation, resolving its smooth/flat mode from live Gallium state.
`2a861dd` fixes FAA clear dispatch: journal the request and stop before ordinary
clear GPU submission, preserving retained FAA command state. The production
wrapper test passes. Complete native FRAME sequencing, one-stream depth draw,
cull/flat packets, and sustained command-pool reuse remain unfinished; no
working glxgears or ioquake3 FAA run is claimed. Further hardware testing will
combine these ready changes instead of rebooting for each small edit.

Mesa `ea4be47` preserves pending color/depth values across separate clear
calls and merges component masks. `c5caa09` retains the exact authenticated
Lock increment for subsequent Unlock/NEXT validation. `802ab27` forwards a
bounded full black/depth-one reset plus live depth/cull/flat state, consuming
its clear journal only after successful presentation. Production-function
host tests and the complete20-object Gallium build pass; the FRAME client
still refuses unimplemented native profiles before submission.

DRM `0ab1f57` integrates the bounded depth DRAW/Lock suffix and `a4df024` the
source-derived flat/CW/CCW packet encoder. `a31eb3b` integrates the fenced
13-END depth submission lifecycle, **with public activation still gated**.
The retained suffix is not yet proved valid for a fresh INIT first draw, and
its fixed source shader profile still needs the live COLOR adapter. The
source epoch difference is explained by retained queue state, not shader
identity: the control finalizer changes the next queue epoch. Runtime commands
must derive that epoch from the retained queue; neither historical delta is
a portable replacement. [Queue/source evidence](../captures/m9-faa-graph-public-vs-20261007/next-state-controls/BOUND-REPEAT.md).
No new hardware result supersedes the six stale edge pixels above.

## October 7: six-vertex FAA draw executes; frame reset still fails

DRM `102a969` corrects real launch counts instead of overwriting packet lengths.
The M9128 test completes first3 then changed6 vertices through Lock/read/Unlock,
release and file close without a wedge. Both new triangles render, with9,936
checked interior pixels and zero interior/exterior errors, but six edge pixels
retain old color. This is batch execution progress, not full frame correctness.
Mesa `73b22bb` integrates the authenticated repeated-draw client; `46d4f02`
adds the genuine six-vertex shader fixture. Mesa `b871c33` now wires opt-in
application draws through live upload/read/Unlock/presentation, and `cb1a062`
adds smooth quads/quad strips without CPU transforms. Pending clear, depth,
culling and flat shading remain explicitly refused. DRM `1fc1972` retains the
three distinct bound color/HZ/depth allocations; their clear/draw activation
is still unfinished. Focused host checks and complete builds pass, but this
new application/depth code has no hardware qualification.
[Exact batch evidence and images](../tests/m9-faa-live-color-six-20261007/README.md).

The original two-output vendor COLOR4 profile (360-byte program, gather40)
also passes an authentic Lock190/read/Unlock1b0 cycle and the pixel oracle,
producing the exact same first image. Its real receipt now permits the
matching post-Unlock vendor continuation; repeated clearing is still open.
[Original-profile receipt and image](../tests/m9-faa-original-color4-hardware-20261007/README.md).

## October7: repeated live COLOR completes, frame reset has three stale edge pixels

Native ade4713 and the full-cycle helper complete first Lock190/Unlock1b0,
black TARGET clear, changed live CB0/geometry and second Lock290/Unlock2b0,
release and file close with a healthy empty queue. Frame2 has9,801 colored
pixels and zero checked interior/exterior errors, but three edge pixels
retain old-frame color contributions. Complete frame-reset correctness is
therefore still unproved. The single-frame GL COLOR pixel success below
remains valid. Mesa7f90fcc also adds an unactivated presentation helper that
copies F021 rows, authenticates Unlock, then writes the live destination.
[Exact repeated-frame receipt, images and failing pixel oracle](../tests/m9-faa-live-color-next-20261007/README.md).

## October 7 latest: live GL COLOR triangle now passes hardware

The combined Mesa5c7fc89 output-arity fix and DRMfa9f2f6 raster/gather
connection fix render the live GL triangle correctly:9,801 colored pixels,
440 partial edges, zero checked interior/exterior/channel errors. Full
Lock190/readback/Unlock1b0/release/file-close passes with an empty healthy
queue. GPU observations confirm the caller color and GL half-depth transform.
Application repeated draws and presentation are still being implemented;
this is not a full glxgears/ioquake3 qualification.
[Original image, inputs and hardware proof](../tests/m9-faa-live-color-20261007/README.md).
The earlier black-image results below are retained as historical evidence.

## October 7 resumed: live COLOR hardware result

FAA work resumed at the user's request, with parallel application, native and
vendor-source agents. The first live COLOR four-stage upload and the build
with corrected packed-record readers both retire through Lock190, F021 read,
Unlock1b0 and cleanup, helper exit0; generation0/queue0/wedged0 remain healthy.
Both images are entirely black. Full application FAA remains unfinished.
Canonical DRM1fd825f contains the reader fix and current-frame authentication;
the unchanged live GL upload comes from Mesa fc5cfc1. Further work now targets
the remaining producer/consumer joins and application lifecycle.
[Exact two-run evidence](../tests/m9-faa-live-color-20261007/README.md).
Earlier wind-down notes below are historical and superseded by this resume.

## October 7 wind-down: current authoritative boundary

Work is paused at the user's request. Two retained live FAA triangle draws,
changed geometry, both readbacks/Unlocks and complete release/file-close pass
on M9128 with healthy transport. This supersedes the earlier single-draw and
helper-timeout notes below. [Two-draw hardware proof](../tests/m9-faa-public-vs-next-20261007/README.md).

The variable COLOR client, shader-bound live GL upload and COLOR0 FS frontend
pass host checks and the complete Mesa build. The new native COLOR launcher
has no hardware result. Full glxgears/ioquake3 FAA, generalized state,
continuous frames and presentation remain unfinished. See the
[final handoff](parhelia-m9-wind-down-20261007.md).

Nano is confirmed M9128 PCI `102b:0540` rev1, with1GiB installed VRAM.
Ordinary textured rendering and the deployed driver are validated. FAA
application rendering is not yet validated. This note is separate from P690
`102b:2539` evidence.

## 2026-10-07: corrected original public draw flow

The first live-shader FAA image is now hardware verified on M9128. The full
INIT b0 → HEAD c0 → DRAW160 → Lock190 path reaches raw191/end8580/posts6
without a wedge. F021 readback produces the requested cyan triangle:9801
colored pixels,9361 full pixels and440 partial-coverage pixels, with zero
interior/exterior/channel errors. The live FS binding fix9cf4af6 derives the
constant span and pair count from the retained upload layout, avoiding the
preceding red utility constants. See [the image and hardware evidence](../tests/m9-faa-public-vs-lock-20261007/README.md).

The helper's complete session still failed its20-second cap after writing the
raw image; buffered output did not identify the terminal fsync/release/close
stage. The next batch records those stages and exercises the source-identical
Unlock encoder. Continuous frames, generalized GL shader/vertex uploads and
glxgears/ioquake3 FAA remain unfinished.

Application FAA remains unfinished. The source HEAD hardware run now passes
INIT b0 then c0/state12 on the same M9128 boot, with rawc1, generation0,
queue0 and no wedge immediately and two seconds later. It posts only the
vendor's first64 primary bytes twice and keeps all six buffers fenced after
this early observation. See [the hardware receipt](../tests/m9-faa-public-vs-head-20261007/README.md).

The complete original DRAW now also passes on hardware: INIT b0 → HEAD c0 →
DRAW160/state13, raw161, primary endpoint5008 and three total posts. It uploads
the fully closed2688-byte secondary block before its first fetch. The same
boot stays generation0, queue0 and not wedged immediately, after two seconds,
and after token release. The counter161 occurs before the final raster, so
both retained fences remain unsignaled; this is not yet pixel completion.
See [the complete DRAW hardware evidence](../tests/m9-faa-public-vs-draw-20261007/README.md).
DRM implementation0d4f9d9 and Mesa clientd1276a2 are committed; the Mesa
client's actual receipt/error controls and full19-object build pass.

The earlier public100→130 diagnostic wedged. Its trace harness incorrectly
classified original function25270 as a blocking CPU wait; the vendor function
instead emits a GPU dependency when the observed counter is below its ticket.
An inserted independent finisher submitted an unfinished block, then the
continuation changed that block after submission. This is a source ordering
error, not proof of a device incompatibility. The corrected original c0
observation allows the emulator to follow the real conditional branch. The
vendor DRAW then returns and submits one complete source block with closed
length fields and no inserted finisher. Normal Lock190 and the first live
pixels are now verified as described above; generalized GL shader uploads,
continuous frames and application presentation remain unverified.
See [the source analysis](parhelia-m9-faa-public-vs-open-block.md).

## 2026-10-07: real M9128 Lock, Unlock and second Lock

The private diagnostic now completes a second normal Lock at actual `0x1c0`
after Lock `0x180` and Unlock `0x1a0`. The [first hardware round trip](../tests/m9-faa-relock-hardware-20261007/README.md)
reached all three fences with `wedged=0`, but an optional F022 CPU aperture
read inserted between Unlock and second Lock changed 1,833 words in the
returned F021 image. A [fresh-boot source-order control](../tests/m9-faa-relock-nopeek-hardware-20261007/README.md)
using the same module, helper, plan and GPU commands omitted only that read.
Its first and second returned F021 images are **byte-identical** clean FAA
triangles (9,801 covered pixels; SHA `ac5a1f13…`). Both runs stayed queue-empty
and healthy through `0x1c0`. This isolates the inserted diagnostic observation
as the relevant difference; the precise aperture-state mechanism remains
under review. The helper now rejects that observation during a relock run.
The [receipt-gated vendor continuation](../tests/m9-faa-relock-cached-return-source-20261007/README.md)
confirms that the second Lock returns the same F021 staging mapper, descriptor
and alias as the first. All 75 visible pages still refer to the retained
primary, and C0/C4/C8/CC are not republished; after the `0x1c0` receipt the
only new MMIO write is frontend `0x314=1`. This is why a separate direct F022
diagnostic aperture read is not a passive observation of the vendor flow.
This is still a private 320×240 diagnostic, not Mesa application FAA or final
display presentation.

The [real six-owner graph preparation test](../tests/m9-faa-graph-prep-hardware-20261007/README.md)
now passes on M9128 boot `48e17497-8d39-4703-8ff2-682bf1543c14`. Its first
module returned `EINVAL`: Linux 7.2 rejects `drm_exec_prepare_obj(..., 0)`
because it reserves zero fence slots. Kernel `7ecd89a` changed graph locking
to `drm_exec_lock_obj`, after which PREPARE returned graph ID 1, generation 0,
`init_point=0`. Duplicate PREPARE returned `EBUSY`; release with a wrong
generation returned `ENOENT`; valid RELEASE still worked after all six GEM
handles were closed, then duplicate RELEASE returned `ENOENT`. The transport
remained submitted/completed 0, queue empty and `wedged=0`. Kernel `e646797`
also provides a CPU-tested binding helper that retains these prepared owners
and derives their live VRAM addresses; it has not been hardware-exercised.

The [fresh first-context source INIT plan](../tests/m9-faa-graph-init-source-20261007/README.md)
has 4,880 primary bytes and natural target `0xb0`; it borrows the six graph
roles and retains separate private owners. Strict regeneration, refusal and
native encoder checks pass. Kernel `f8db007` turned the plan into an executable
first-context graph INIT; its first hardware attempt exposed ordinary MMU
layout selection and refused before a GPU post. Kernel `c3873e8` selected the
source FAA secondary-root layout for the graph profile. The
[locked M9128 hardware round trip](../tests/m9-faa-graph-init-hardware-20261007/README.md)
then retired actual point `0xb0` with a live six-owner graph, 28 retained
context owners, and no wedge. A subsequent same-boot run with the bounded
receipt helper `5d34a125` returned graph state `INITIALIZED`/point `0xb0` and
independent `WAIT_POINT` completion `0xb0`. Both immediate and two-second
transport samples show raw `0xb1`, submitted/completed `0xb0`, queue 0 and
`wedged=0`. PREPARE still leaves the graph uninitialized. This is real graph
INIT, not an FAA draw, resolve, display image or application result.

Mesa M9 `e327638` adds an explicit `M9X_FAA_INIT=1` opt-in on top of
`M9X_FAA_PREPARE=1`. It sends the exact graph ID and generation through the
new INIT ioctl and records `graph.initialized` only for a matching state 2
receipt with a nonzero point; initialized file-token RELEASE leaves the
kernel's native owner references intact. Linked graph tests pass 5,211 cases
in normal and sanitizer builds. This bridge does not yet select FAA draw or
presentation commands for applications.
The [post-INIT native seam review](parhelia-m9-faa-post-init-native-seam.md)
documents why the retained virtual primary requires a separate public draw
continuation and exact graph identity, rather than ordinary `M9_SUBMIT`.

Kernel `e933dad` guards the direct F022 diagnostic aperture path after the
source-qualified normal-Lock replay starts; its actual-C tests cover three
admission points, two races and F021/control preservation. Kernel `53b953d`
adds a file-owned six-buffer FAA graph PREPARE/RELEASE transaction, returning
only a PREPARED token with `init_point=0`. It posts no GPU initialization.
The [fresh-boot integrated regression](../tests/m9-faa-f022-guard-hardware-20261007/README.md)
loaded that combined module and again retired `0x180/0x1a0/0x1c0` on M9128
with byte-identical clean F021 images and `wedged=0`. It validates that the
new admission code did not regress the private source-order path. A subsequent
same-boot direct F022 probe on another color BO returned `EBUSY` before any
new post, with all transport fields unchanged except sample time. It does not
exercise graph PREPARE or establish application FAA; the separate graph test
above exercises PREPARE and RELEASE.

Kernel `9eed0a9` continues the source-proven primary only after a successful
public F021 Lock read. The Lock returned actual point180 and the same clean
9,801-pixel diagnostic triangle. Separate normal Unlock retired actual point1a0,
copying from the F021 staging view into F022 on the same retained color BO.
The queue stayed empty, generation0 and `wedged=0` immediately and two seconds
later. [The archived same-boot result](../tests/m9-faa-unlock-hardware-20261007/README.md)
includes both 307,200-byte views, health receipts and a checker. Host
ASAN/UBSAN controls cover 163 split/resume/fence cases and reject five
regression mutants in canonical kernel `a6c2cce`.

The post-Unlock F022 raw CPU view in the intervening-read experiment differs
at 1,833 words and is stippled in three-byte visualization. The vendor source
qualifies F021 as the returned Lock image, but does not qualify this F022 view
as resolved display pixels. Its appearance alone cannot establish a copyback
defect or presentation success. The [next genuine public Lock source](../tests/m9-faa-prepared-relock-source-20261007/README.md)
first consumes the actual `0x1a0` Unlock receipt, then emits a reverse
320×240 primary F022 to staging F021 GPU copy and reaches a new natural
`0x1c0` wait. Its first 8,744 source command bytes are unchanged; the new
suffix is 100 bytes. Real M9128 hardware has now retired `0x1c0` and returned
the byte-identical second F021 image in the source-order control. The Mesa
context-owned FAA graph remains open; no glxgears
or Quake FAA result follows from this test.

## 2026-10-07: first real private FAA pixels on M9128

Kernel 4a9ecd6 and RE d3ec0974 now validate the *private diagnostic*
320×240 FAA triangle through the actual source-returned F0210140/00010100
normal Lock view. The exact prepared180 command stream retired on M9128
102b:0540 rev01/subsystem22c0, and a guarded read of the same retained
sampled-primary BO returned a formatted linear image. It contains 9,801
covered pixels (9,361 full and 440 partial) within the expected triangle
vertices x80..240/y60..180; conservative interior, exterior and channel
checks have zero errors. The queue was empty and the GPU was not wedged
immediately or two seconds later. [Screenshot and same-boot receipts](../tests/m9-faa-lock-view-hardware-20261007/README.md)
include a checker, PNG, Lock-view bytes and separate raw tiled BAR bytes.
This result supersedes the older no-pixel statements below for the private
prepared180 path only. Mesa's application FAA graph is still uninitialized;
textured, depth-tested app draws and final display publication remain open.

Latest hardware checkpoint: kernel b011551 retires the second finite
replacement-format barrier at250/raw251 on a fresh M9128 boot. All eight phases
complete; empty queue/wedged0 persists after two seconds. Main, companion and
sampled-primary contents match prior230; pool state changes. Ten retained BAR
roles and exact counters are archived and verified. The original240 emulator
continuation can proceed. No ordinary-output or FAA pixel pass is established.
[Finite format barrier hardware evidence](../tests/m9-faa-public-replacement-submit-hardware-20261007/README.md).

Latest hardware checkpoint: corrected kernel da3e710 retires the original
public caller's replacement-preparation prefix at230/raw231 on a fresh M9128
boot. All seven boundaries complete; queue empty/wedged0 persists after two
seconds. Main, companion and sampled-primary hashes match prior1f0; pool state
changes. All ten retained BAR roles are archived and verified. This permits
the original220 emulator continuation, not an FAA image claim. Ordinary output
is absent and the helper verifies ENODATA rather than inspecting FAA pixels.
[Replacement preparation hardware evidence](../tests/m9-faa-public-replacement-hardware-20261007/README.md).

Latest code review: kernel da3e710 fixes a new pre-deployment regression in
65cafcc/ce402c0: encoder24886 also represents standalone resident-database
bindings, so requiring a C3 partner for every occurrence rejected valid FAA
profiles at the c0 callback. Validation now identifies the program owner from
complete pairs before checking its orphan bases. All seven actual generated
tables pass ASAN/UBSAN, missing-half controls still refuse, and the isolated
replacement-profile module builds. The old staged ce402c0 module must not be
deployed. There is no new hardware result from this audit. See the
[logical review](parhelia-m9-faa-native-logical-audit.md).

Latest hardware checkpoint: kernel55ffd76 retires the whole returned public
FAA flush at1f0/raw1f1 with all six waits preserved, an empty queue and wedged=0
immediately and two seconds later. This includes the source-emitted cleanup
and fragment-pointer reset previously absent after resolve dispatch. Main,
companion and sampled-primary hashes remain unchanged; pool state changes.
Ten retained BAR roles and exact counters are checked and archived. Source
still requires its staging-copy path; no FAA pixel pass is established.
[Returned flush hardware evidence](../tests/m9-faa-public-returned-hardware-20261007/README.md).

Earlier hardware checkpoint: kernel151e438 now retires the genuine public
mode9 resolve prefix at1d0/raw1d1 on fresh M9128 boot, with an empty queue and
wedged=0 immediately and two seconds later. All five endpoints are preserved.
Main, companion and sampled-primary hashes remain unchanged; pool state changes.
Ten retained BAR roles and exact counters are archived and checked. This is
not a pixel pass. The actual receipt now permits original1c0 cleanup to
continue toward the true aa750 return.
[Public resolve hardware evidence](../tests/m9-faa-public-resolve-hardware-20261007/README.md).

Earlier logical review fixes two concrete defects: kernel e5af628 removes
an obsolete four-phase cap while preserving all waits, and Mesa c192362
rejects companion format aliases before allocation/layout selection. Focused
sanitizer controls and full builds pass; no new GPU result follows from those
checks. Kernel151e438 now admits the genuinely emitted public mode9 resolve
continuation (source9424/final1d0), with128 native-placement controls and
strict source checks passing. Its subsequent hardware retirement is recorded above; rendering remains unproved.
See [the logical audit](parhelia-m9-faa-native-logical-audit.md) and
[the public dispatch proof](parhelia-m9-faa-public-resolve-dispatch-wait.md).

Latest hardware checkpoint: kernel e316f3f retires the distinct public
shared-depth dirty-flush prefix at1a0/raw1a1, with an empty queue and wedged=0
immediately and two seconds later. Its generated phases are c0/120/140/1a0;
the140 pause is diagnostic at a genuine post, not an original CPU wait.
The sampled primary remains unchanged; no FAA pixel pass is established.
This source-specific receipt permits the original public190 continuation.
[Public-flow hardware evidence](../tests/m9-faa-public-flush-hardware-20261007/README.md).
It includes the sampled-color pin/reference retention fix9dee327, so later
readback retains the same owner after the helper exits. The separate inplace
continuation reached missing PDEV+d4/d8 helpers; their genuine86660 constructor
has been identified, but conversion readiness and final pixels remain open.

Earlier continuation: kernel d249a3e retires the whole sampled copy-wait prefix
at1a0/raw1a1, with an empty queue and wedged=0 immediately and two seconds
later. It preserves c0/120/170/1a0 phases and executes the C2 access kick atc0.
Main/companion/sampled-primary hashes remain unchanged; pool state changes.
The ordinary-output guard remains ENODATA. This qualifies the next original
190 CPU-preparation continuation, not an FAA image.
[Exact1a0 hardware receipt and raw buffers](../tests/m9-faa-inplace-copy-hardware-20261007/README.md).
Mesa776e5bf also fixes diagnostic store reporting before shared GPU use retires;
its CPU checks and full DSO build pass, with no new application pixel claim.

Earlier hardware checkpoint, 2026-10-07: kernel6fe8ead retires the complete180
stream with phased waits, deferred output publication and the source-backed
program-access kick. Output remains entirely black. Main601 nonzero words,
companion1404 and sampled primary1308 match the prior phased run. The F022
sampled CPU read succeeds but retains the same nonzero word multiset as the
raw primary; no resolved FAA pixels are proved.
[Actual program-kick hardware evidence](../tests/m9-faa-program-kick-hardware-20261007/README.md).
Collapsing the waits was separately isolated as a real ordering defect: the
same-module single-post control left main/companion untouched. Keep the phased
ordering; neither late output publication nor the program kick alone fixes
the image. The [logical audit](parhelia-m9-faa-native-logical-audit.md) and
[encoder proof](parhelia-m9-faa-complete-address-encoding.md) distinguish fixed
defects, guarded diagnostic constants and remaining lifecycle gaps.

Kernel3b39c98 additionally admits the source-derived sampled-destination
pending prefix after genuine temporary-allocation failure. Its independent
finish is170; the original160 wait and rectangle launch remain pending.
Host source/128-placement checks and module build pass. Its distinct hardware
run now retires170/raw171, with an empty queue and wedged=0 immediately and
two seconds later. Main/companion/sampled-primary observations remain unchanged;
ordinary output correctly refuses with ENODATA. The receipt qualifies this
prefix, not the pending rectangle or an FAA image.
[Actual170 hardware evidence](../tests/m9-faa-inplace-prefix-hardware-20261007/README.md).
This is a diagnostic branch, not completed application FAA.

Historical hardware checkpoint: kernel6367f4f completes the whole pending-output
prefix at0x160/raw0x161, next0x170, empty queue and wedged=0 immediately/+2s.
The public clear and triangle have returned; the original output path is still
at its format-transition150 wait. The real output allocation stays unreadable
(ENODATA) until the remaining resolve is proved. Pool stores change to659
nonzero bytes/200words; this does not establish an FAA image. Five full BAR
roles and all four counter channels are archived before recovery.
[Actual160 hardware evidence](../tests/m9-faa-preclear-output-hardware-20261007/README.md).
The preceding120 preprocessing buffer also independently matches all36 checked
color/screen/clip components. Normal-driver recovery follows the private test.

Preceding FAA diagnostic, 2026-10-07: kernel c7db56b physically retires the
completed public black clear and original triangle's first preprocessing
launch at `0x120` (raw `0x121`), with an empty queue and wedged=0 immediately
and two seconds later. The preprocessing allocation contains 55 nonzero bytes
in 32 words; main and companion remain zero. Five complete GPU buffer roles
were read through a bounds-checked read-only mapping and archived before
power cycling. There is still no final output owner or FAA pixel proof.
[Actual preprocessing evidence](../tests/m9-faa-preclear-draw-hardware-20261007/README.md).

The early vendor observer corrects the earlier clear interpretation: public
CLEAR42 returns before the original triangle reaches the first wait. The
whole public draw request remains pending. Earlier `0xc0` retirement remains
valid; the statement that the clear handler had not completed was incorrect.
Only common counter +00 advances to raw `0x121`; +20/+40/+60 stay `0x10/0`.
The emulator continuation must use these exact channel-specific observations.

Normal FAA-off qualification with kernel e1703f9 and Mesa f31f2ec passes
16 GPU draws, zero CPU fallback and zero of 1,572,864 wrong pixels. One
matching fullscreen 1024x768 Q3 basic-GL2 run gives 40.7 FPS versus 40.8,
effectively flat, with healthy retirement. The constant reuse improvement
is retained. [Normal hardware proof](../tests/m9-constant-broadcast-hardware-20261007/README.md).
After archiving and power cycling, normal e1703f9/modulebbe7bca4 is restored
on verified boot e0e83f87-34ff-4e73-859a-ee3d5b4f4dc5: empty queue, unarmed,
wedged=0, next=0x20.
Historical checkpoints below retain their original boot and module identities.

Earlier backend-status test,2026-10-07: unchanged516990f on freshbootd3f40c04
still produces the identical black image after actual140 retirement and the
post-wait kick. All before/after samples have vendor drain mask30000 clear;
no persistent backend drain was observed. Both primary IN_FORMATS blobs now
physically publish five formats for all three modifiers. Complete proof is in
[the status hardware archive](../tests/m9-faa-status-hardware-20261007/README.md).
After copying proof, a second power cycle recovered network boot; normal73
is restored on boot4636e49f-790e-46ce-88df-8c107cf63889 with privateFAAoff,
user submissions on and an empty unarmed healthy queue. Kernel source951e6a2
fixes the atomic-driver property-getter warning; its source/host proof is
separate from the restored normal73 binary.

Previous deep-review hardware checkpoint,2026-10-07: kernel516990f repairs
the genuine output mapper's omitted post-wait314frontend kick. Original
ac1f0 stack continuation proves that ordering with the same-profile actual
retired counters; it emits no additional command-buffer words or backend
status reads. One freshboot4692a5d2 public-red test executes the new kick
after real140 retirement but still reads an entirely black image. The
source-proven ordering repair is retained; it is insufficient to fix FAA.
[Actual post-wait kick qualification](../tests/m9-faa-postwait-kick-hardware-20261007/README.md)
and [original CPU continuation](parhelia-m9-faa-output-postwait-continuation.md).
After copying proof and mandatory power cycle, root restored validatednormal
observer73/module997c1e0e on freshboot6d6d22e6, user submissions enabled,
privateFAAoff, next20/raw10/10, emptyqueue/unarmed/busmaster0/wedged0.

Subsequent normal GL2 testing on that same boot completed84ac0/raw84ac1;
the latest read-only check retains an empty queue and wedged0. The archived
[normal status control](../tests/m9-faa-status-normal-20261007/observation.json)
is FAA-off evidence only, not a sampled-render fault diagnosis.
The [BM program audit](parhelia-m9-faa-bm-program-options.md) closes actual
locked defaults and retained program owners: both options are one, while
the final mode9 resolver explicitly emits four inline instruction pairs.
No missing resident firmware initialization follows from those names.

The public SURFACEBLT prototype reaches its genuine normal copy dispatcher
but has not reached `aa750` or a new FAA output sequence. Root's static
review distinguishes `aa750`'s AA mode1f from `abc20`'s output mode1d;
retaining a sentinel in the former does not prove it is the required output
operation. Actual graph direction and public admission remain under review.
No new FAA hardware candidate or working image is claimed by this trace.

The board-specific cold audit independently executes original Windows and
Linux MC producers on verified physical M9128 PInS: all76writes/4delays
match. Current observations match17directMCfields and all desired system
clock selectors; f14upper readback semantics remain unclassified. This
narrows cold-state discrepancies but proves no FAA-specific prerequisite
or cold-register fix. [Actual board cold-state evidence](parhelia-m9-faa-cold-mc-writes.md).

Latest physical FAA checkpoint,2026-10-07: kernel2ede11f adds only bounded
raw prefixes from existing owner observation reads. Exclusive public-red
module06aa7d9b/blob2cccc5f1/helper55d0c901 on freshbootdf6def67 reaches
real140 retirement, but all327680ordinary SYSTEM output bytes remain zero.
CPU positive control passes76800pixels; strict FAA image remains blackFAIL.
All30 observed retained-owner prefixes and actual PPM/logs are archived in
[the exact saved-state hardware proof](../tests/m9-faa-prefix-hardware-20261007/README.md).
External caller color is excluded from the31spec observer array.

The live sentinel preserves its indexed header and acquires saved words
24a/0/0 at offsets8/c/10; its filler at4 becomes zero. These are actual
produced-state observations, not a decoded FAA record ABI or corruption
claim. A source correction distinguishes the actual triangle sentinel20B
range from the final ordinary-output pool64B range: this profile emits no
fresh sentinel range in final resolve.
[Exact draw/save/resolve source joins](parhelia-m9-faa-draw-resolve-views.md).
Independent review finds all eight shared shader/RS/constants/geometry
images byte-identical to the physically passing ordinary SYSTEM red control,
and all nonzero callbacks uniquely bounded.
[Independent input review](../tests/m9-faa-red-shared-input-review-20261007/README.md).
The genuine PCI/MM→MD flags2 publisher also generates byte-identical GPU
inputs; it is retained as a producer correction, with no repeated image test.
[Flags source proof](parhelia-m9-faa-pdev-flags-publisher.md).

Root copied all proof before mandatory power-off/power-on and restored the
validated normal observer73 on freshe6d15c3d, privateFAAoff/user submissions
on, empty unarmed queue and wedged0. Deep review continues actual pool
save/restore, the original post-wait output flow and M9128 cold prerequisites.
No sufficient FAA fix or application FAA rendering is established.

Current physical checkpoint,2026-10-07: the genuine **ordinary SYSTEM red**
control physically PASSES on M9128. It has no FAA activation, sampled store,
companion clear or resolve. Public source admission uses its actual target
constructor and manager publisher; all addresses come from live retained owners.
The native frame retiresd0,9520pixels are red/full, zero interior/exterior/
wrongchannel errors, partial0 as expected for single-sample rendering. Entire
SYSTEMoutput327680bytes changes from allzero to19040nonzero bytes/9520words.
Positive CPU mapping control passes76800pixels. Full23owner/domain/extent/
derivedtimeline/actualPPM joins independently pass. Exact modulea6c0b5e,
firmware7c64b4af and helper55d0c901 are retained in the
[hardware proof](../tests/m9-ordinary-system-red-hardware-20261007/README.md).

This qualifies the ordinary SYSTEM target-write and readback contract for
this source descriptor. The previous sampled FAA red control still has a
black final output. Its remaining investigation is the sampled draw/resolve
consumer flow and prerequisites; the ordinary control is not an FAA demo.
The explicit `--expect-ordinary-system-red` readback option omits only the
FAA partial-coverage minimum. Existing FAA green/red modes retain their
strict geometry, channel and partial-edge tests.24actualproduction CPU
oracle controls pass. No captured output pixels seed native memory.
After archiving the proof, root power-cycled and restored normalobserver73
with user submissions on/private FAA off, freshbootc44237c9.

Previous physical checkpoint: complete source init-only and post-public-draw
controls kernel d2821c3 retire B0 and120 respectively without a GPU timeout.
Init-only main/AA/pool remain zero; post-draw main1041/AA1404 changes already
occur before any output resolve. Complete main/AA/preprocessing bytes equal
the older full FAA sequence, rather than only matching its hashes. These
opaque writes do not yet prove shader colors or correct fragments. Full FAA
ordinary output remains black. See [init-only evidence](../tests/m9-faa-init-only-hardware-20261006/README.md)
and [post-draw evidence](../tests/m9-faa-post-triangle-hardware-20261006/README.md).
The fresh public constant-red hardware control also retires140 without a
GPU timeout, but ordinary output remains entirely zero. Main changes from
green1041 to601 nonzero DWORDs:880 bytes change in440 aligned groups.
Complete AA and preprocessing buffers remain byte-identical. This proves
color-dependent main-store behavior, not its record ABI or correct resolved
pixels. The actual source profiles differ only at four raster-c0 bytes;
remaining resolve/program/pool prerequisites are under audit; the ordinary
SYSTEM write/read contract is separately qualified above.
See [complete red hardware evidence](../tests/m9-faa-constant-red-hardware-20261006/README.md).
The hardware readback helper accepts `DEVICE OUTPUT.ppm --expect-red` to
select its independent expected channel; this option does not change the
shader, submission or GPU stores. The default green oracle and strict
interior/exterior/partial-coverage requirements remain. Actual oracle
statement checks cover synthetic CPU-only red/green, wrong colors, black
interior, partial edges and exterior rejection; no GPU result is invented.

Previous physical checkpoint: whole public constant-green control kernel6adb92f
CONTROL=y, M9128 boot6750cc38. The source-owned context reaches real140
with verified WC mappings, but the image is still entirely black. Main/AA
hashes match the interpolating baseline exactly; all ordinary SYSTEMoutput
bytes remain zero. CPU mapping control passes76800pixels, no GPUtimeout.
The complete source constant shader replacement did not fix pixels, weakening
interpolation/setup as the sole cause. Full vendor cold initialization and
remaining resolve state/ownership dependencies are still under source audit.
FAA stays disabled for apps; no successful FAA picture is claimed.
See [the actual constant shader control](../tests/m9-faa-constant-green-hardware-20261006/README.md),
[its source and separate profile](parhelia-m9-faa-output-constant-green.md),
[previous source-init test](../tests/m9-faa-source-init-20261006/README.md), and
[bounded raw-store interpretation](parhelia-m9-faa-store-record-audit.md).

Earlier sampled-destination tests below are historical evidence.

Current authoritative private replay and hardware evidence are in
[the native replay note](parhelia-m9-native-faa-replay.md). Kernel `bcccb66`
includes the complete normal-surface capture with 30 owners, 53 typed addresses,
26 timeline fields and the actual 5676-byte primary and 604/772-byte counted
secondaries. The latest physical M9128 run on boot `96a79ac0` passes all
76800 positive CPU pattern pixels through the exact vendor F0220140 view,
verifies all 117648 known VRAM input bytes, and reaches GPU retirement `0x1e0`.
The image still fails: 278 scattered partial pixels, no full green pixels,
8326 bad interior pixels, 185 bad exterior pixels and 823 non-green pixels.
App FAA remains disabled; there is no FAA-on application screenshot.

The direct CPU-root mapping is repaired and physically verified. The latest
run retains the bounded 256 MiB direct-root initialization and 3072 vendor
virtual-address tags, and adds the coherent optional 16-byte companion
mapping profile and source-known DMA metadata. The main fragment store now
changes from zero to 1041 nonzero DWORDs and the AA companion to1404.
Actual retired BAR bytes agree with kernel observations; preprocessing has
correct triangle positions and green varyings. See
`tests/m9-faa-companion-20261006` for the latest failed image and raw resource
evidence; earlier root-join and virtual-restore archives retain historical
results. The remaining failure is downstream of preprocessing: packed FAA
data, resolve and destination/CPU-view interpretation are being compared
with the actual vendor flow. GPU writes are not proof of correct pixels.

The latest run additionally corrects the allocation-dependent companion
clear words emitted outside the relocation callback. The native 151-page
extent requires20 words rather than captured19; the primary is rebuilt to
5680 bytes after its other operand patches. Actual vendor instructions and
the native helper agree at all eight page alignments. The resulting GPU
image still fails, with unchanged fragment/companion observation hashes.
This proven command fix is retained. See `tests/m9-faa-clear-20261006` and
[the mandatory capture replay policy](gpu-capture-replay-policy.md).

The earlier manually joined frame below was superseded by the coherent
normal-surface capture, whose second real launcher retains sampledFAAstate.
It is historical evidence of an admission error, not the current replay.
The current private stream includes companionHZ/pools/sentinel/resolve and
reaches hardware completion; rendered fragments remain unproven, while the
exact CPU view now passes. The historical AD/AF smoke instrument below is separate from this
full trusted private kernelreplay and does not enableFAAforapplications.

`tools/m9-faa-smoke.sh` runs the current instrument at 320x240, its fixed
measurement size. Application performance checks remain at 1024x768. Run it
under the target owner's common lock and both historical M9 locks:

```sh
flock /tmp/parhelia-hw.lock flock /tmp/m9-hw.lock \
  flock /root/tmp/m9-hw.lock env M9X_MESA=/root/parhelia-m9x-mesa \
  M9X_OUT=/root/tmp/m9-faa-smoke sh /root/parhelia-m9x-tools/m9-faa-smoke.sh
```

The smoke runs `texture`, `faarect`, `faaedge` and `faapos` with FAA off,
then the three FAA instruments with only `M9X_FAA=1`, followed by ordinary
texture rendering again. Each child gets a clean environment: inherited
mode overrides, alpha-to-coverage, arbitrary bind indices and the software
multisample calibration option cannot contaminate the run. New kernel
timeouts, a nonzero child exit or a different renderer stop the sequence.
The paired phase proves only current AD/AF packet plumbing. True FAA needs
nonzero partial edge levels, a full 48000-pixel integer-edge rectangle,
valid sample staircases and GPU retirement; a process exit of zero does
not prove any of them. The captured historical mode sweeps below did not
produce antialiasing.

The initial canonical `m9x` baseline `6780573` already had the diagnostic store
allocator, the `bc/bd` fragment-store bindings, optional `b8/b9` companion
binding and `RESET_FRAG_PTR`. Kernel `m9x/m9_submit.c` requires explicit
`faa_probe_indices` permission for `bc/bd` and `a1/a3/a4`; ordinary AD/AF
and the raster `ef` word need no probe whitelist. No shipping MSAA caps are
advertised. Module dependencies are `i2c-algo-bit`, `drm_client_lib`,
`drm_dma_helper`, `drm_ttm_helper` and `drm_vram_helper` on the current target.

Previous M9128 measurements already tried mode6 plus the genuine-sized
store, pointer reset and every 128 companion format codes. The store had
zero changed dwords and the diagonal had zero partial levels. Modes6/7
reserved approximately one eighth of the pixels; some descriptor classes
restored those pixels without antialiasing. Repeating those sweeps is not
an implementation step. Alpha-to-coverage, mode bits in preprocessing, and
mode6/7 with ordinary D24S8 are established hangs.

The missing contract includes the companion's optional HZ plane and its
`ba/bb` selector12 descriptor, sentinel ownership/save and restore, admission from
the initialized vendor context, resolve and destination retirement. The
ordinary implementation binds `ba/bb` to its ordinary auxiliary depth
resource; replacing only `b8/b9` is not proof of the vendor's complete AA
companion graph. The actual AA companion request uses format5; generic
linked-kind9 recursion requires requestformat9 and does not describe this
companion. BA/BB instead depends on companion+38c/+39c with selector12.
Static recipes should not be equated with a full FAA submission.

Actual MSerX emulation has executed the AD/AF and reset emitters, but with
controlled host owners. The isolated M9 emulator worktree tracks progress
through genuine miniport initialization. A complete enabled vendor lifecycle
capture is still required to close the missing contract. The older main
FAA note's absence of this new bootstrap progress is historical, not a
statement that no M9 emulation was run.

## Additional real store-binding operation found during this round

Static MSerX `MxMD.dll` emitter `0xfb920` contains a larger conditional
store-binding operation than the existing native `bc/bd` pair. Dirty bit
`0x40` at `0xfbca4` tests a backing-address pair in shadow `+0x9c/+0xa0`
against the global null pair `0x20d51c/0x20d520`. If different, it emits:

1. Index `0x24`, using genuine relocation kind `0x2b` on that address pair
   (`0xfbd09` header `0x45454524`; callback at `0xfbd29`).
2. Three `e6` words: `0x6dc00000`, `1`, `1`
   (`0xfbd4b` header `0x45e6e6e6`, values at `0xfbd55/0xfbd5f/0xfbd65`).
3. The familiar `bc/bd` bindings (`0xfbd8e` header `0x4545bdbc`).

This is conditional vendor code, not an enabled FAA API capture. The native
path emits neither index24 nor this e6 triple. The current kernel also
rejects index24 unless explicitly listed as a probe index, and rejects
`0x6dc00000` and `1` as e6 controls. Whitelisting the index alone does not
admit the operation.

The AA binder `0xaa750` does fill the additional address pair. At `0xaad07`
it selects a device-owned resource at `hw + 0xfed8 + slot*144`, then calls
that resource's virtual method `+0x5c` with descriptor kind5 and destination
shadow `+0x9c` (`0xaad13..0xaad1c`). `hw` is `[device+8]`, and `slot` is
`[device+0x24]`, with `device` read from the AA surface `+0x238`. Thus this
is a separate per-slot owned resource; it is not a justified alias of the
main fragment store or the companion.

Helper `0x756f0` returns the 144-byte slot entry at `hw+0xfebc`, placing
that resource at entry `+0x1c`. Allocator `0x76540` reuses it only on its
pool6 path; when absent it calls `0xa3870` with an allocation request and
publishes the result to entry `+0x1c` at `0x7661a`. Its request uses flags
`0x8002`, final request-format0 (the earlier `0x1a` write at `0x7656e`
is overwritten at `0x765e3` before allocation), width derived from
`(bytes+3)>>2`, and
rows derived from `(width+1023)>>10`. The request-format and descriptor-kind
namespaces must not be conflated with texture format-table IDs.

Required next proof is the resource's genuine address method, allocation
size/domain and initialized owner lifetime, followed by the operation's
dispatch/retirement semantics. Replaying a guessed index24 address would
not answer those questions. The emulator agent is executing the complete
conditional emitter with recorded callback arguments separately.

The real interface getter is also identified. Resource-interface constructor
`0xa32a0` installs vtable `0x1f3a90`, whose slot `+0x5c` is `0xa6740`.
Its descriptor-kind5 dispatch entry `0xa68fc+5*4` targets `0xa6776`, which
selects layout selector2. It invokes descriptor builder `0x467e0` on
`[interface+0xc0]+0x3c` if the selector's valid bit is absent, then reads
that selector's address at layout `+0x20`. For an ordinary directly backed
resource (`interface+0xa4/+0xa8` zero), it returns an address pair
`{0, address}`. The indirect branch returns `{0x40000000, &interface+0x74}`,
with that reference carrying the allocation object and address offset
(`0xa6811..0xa682b`). A relocation recorder must resolve that branch through
the genuine allocation owner; treating its host pointer as a GPU address
would fabricate a binding. This identifies the getter contract statically,
but not the per-slot resource's purpose, initialized lifetime or physical
extent.

## Is vendor FAA reachable through normal options?

The saved retail MSerX binaries contain a normal configuration-to-pixel-format
path, beyond the debug strings and controlled emitters. This is static
reachability evidence, not proof of an initialized M9128 completing FAA:

* The MxMD configuration constructor reads `OGL\Multisample` with default0
  at `0xb9f49`. The real `DrvEscape` query `0x80040610` handler `0xd4180`
  returns that option at output `+0x20` (configuration wrapper `+0x2ec`,
  virtual getter at `0xd429a`). The same response carries `3D\AAMode & 1`
  at output `+4` via the actual getter `0x64760`. Default AAMode
  `0x80000842` has that bit clear.
* The ICD's actual pixel-format enumeration function `0x10043900` calls
  configuration query `0x10048d10` at `0x1004393c`. The query combines the
  two response values into global `0x108aa200` at `0x10048d78..0x10048d8e`.
* The pixel-format builder requires color depth greater than24 at
  `0x10043f18`. If that combined global is nonzero, `0x10043f32` marks
  the pixel format multisampled at `pf+0x74`. The normal WGL attribute
  getter reports eight samples for that format at `0x100470f6..0x10047100`.
* The real MxMD AA allocator `0xac7e0` has direct call sites `0x51ef2`,
  `0xa787d` and `0xb6a19`. Surface construction at the first site tests
  a request flag `0x20000000` or retained multisample flag, positive sample
  count `surface+0x270`, resource class2 at `[surface+0x23c]+4` and absent
  existing AA storage before invoking the allocator.

Therefore configuring both options and choosing a multisample 32-bit WGL
pixel format is a real retail-code route to investigate. Merely setting
`GL_MULTISAMPLE` on an ordinary single-sample pixel format is not that route.
The registry root and an exposed PowerDesk UI control have not been verified
in this round. No initialized normal-API allocation, enabled submission or
native M9 antialiased image has yet been established by this static chain.


### Dynamic normal-option proof (2026-10-06)

`tools/m9-vendor-faa-options.py` now executes the actual MSerX binaries,
verifying both SHA256s before running. Its CPU host is isolated in
`tools/faaemu/m9_options_host.py`. The committed compact result is
[the four-case matrix](traces/m9-vendor-faa-options-20261006.json); the full
output additionally records every pixel format and published sample value.

The complete raw configuration constructor `0xb8ea0` builds 97 descriptors;
the complete derived-owner constructor `0x5fa70` succeeds. Their actual
DWORD getters issue IOCTL `0x232104` with keys `3D.AAMode` and
`OGL.Multisample`. The fixture supplies bounded Windows registry responses,
including explicit missing-property errors that exercise real defaults.
The actual descriptor and derived-wrapper code, MD dispatch
`0xd3e70 -> 0xd4180`, and ICD query `0x10048d10` all execute. Neither the
query response nor ICD AA globals are supplied by a host callback.

| AAMode | OGL Multisample | Actual ICD FAA enable gate |
| --- | --- | --- |
| missing (default80000842) | missing (default0) | 0 |
| 80000842 | 1 | 0 |
| 80000843 | 0 | 0 |
| 80000843 | 1 | 1 |

The genuine ICD CRT attach `0x1072ada8`, device-manager constructor
`0x1002ae30`, complete ordinary pixel-format enumeration `0x10043900` and
WGL attribute construction `0x10046f20` also finish. However, this manager
has not acquired genuine adapter capabilities. Its hardware-format branch
`0x10043f18/0x10043f2a/0x10043f32` is never reached. Enumeration returns
39 baseline/fallback formats, all actually publishing WGL sample buffers0
and samples0 in all four cases. Three appended formats have `pf+0x74=1`
when OGL Multisample is enabled, while still publishing samples0. Therefore
that flag alone must not be treated as proof of multisample advertisement.

This closes dynamic option parsing and the real configuration query, not
normal enabled FAA pixel-format admission. Host services cover ordinary CPU
allocation/TLS/charset and a 1024x768, 32-bit GDI display. The MD query owner
and per-process default snapshot are controlled ABI fixtures; no initialized
GPU, created AA surface, submission, or retirement is represented. Genuine
ICD capability acquisition and the specific hardware-candidate rejection
must be traced next to distinguish an uninitialized fixture from an actual
M9128 capability restriction. Do not infer retail M9 FAA support merely from
the enabled ICD option gate, and do not infer permanent M9128 rejection from
this incomplete owner.


### Exact fallback rejection and first normal capability request

The option helper now records the actual capability source and rejection.
In each case `0x10044330` leaves the mask at builder stack `+0x0c` zero;
six hardware candidates execute `test cl,1` at `0x10043d8f` and branch
away at `0x10043d92`. They never reach the AA gate. The producer traverses
the ICD manager's display/capability collection `+0x1b8/+0x1dc`; the
constructor-only fixture has not populated that collection. This is
incomplete capability acquisition, not a demonstrated PCI0540 restriction.

Run `tools/m9-vendor-faa-options.py --capability-boundary` to continue the
actual normal acquisition function `0x1002d4b0` with a bounded CPU display
inventory matching Nano's M9128 (PCI102b:0540rev1). This supplies ordinary
EnumDisplayDevices/GDI monitor metadata, not hardware capabilities. The
actual code reaches its first adapter query and deliberately stops before
any reply. The committed request is
[the capability boundary capture](traces/m9-vendor-faa-capability-boundary-20261006.json):

* Escape `0x186a0` (100000), input32 bytes, output244 (`0xf4`) bytes.
* Input hex:
  `020000000100000000000000000000000000000000d9dadb0000000000000000`.
* `MxMD` ordinary `DrvEscape` at `0x3d7e0` routes this to `0x972e0`.
  Request operation2 dispatches to `0x966a0`. Its actual response producer
  reads derived configuration `hw+0xfe70`, the registered card-owner list
  `0x20d1cc`, per-process options and an actual miniport IOCTL `0x23231c`.
  Those owners must be initialized before returning a response.
* The actual card registration method `0x27230` prepends its argument
  through `device+0x69c0` to `0x20d1cc`. It is an entry in the ordinary
  paired module initialization table (`0x1f2638`), invoked by the startup
  loop `0x2e03a..0x2e056`; it is not a standalone proof of initialized
  adapter capabilities or a virtual-method table.

No hardware capability reply is fabricated. The first missing reply is now
an exact normal API request with an identified retail-code producer. Native
Linux Nano cannot directly supply a Windows vendor DrvEscape result; the
emulated display-device initialization and miniport must provide its genuine
ownership and query inputs. The current capture does not establish that
M9128 admits or permanently rejects normal FAA pixel formats.

### Actual ordinary module configuration ownership

`tools/m9-vendor-config-owner.py` and reusable `faaemu/m9_config_owner.py`
execute the real ordinary module initializer `0x61f70`, rather than manually
attaching its two individual constructors. The initializer allocates,
refreshes and publishes raw configuration at `hw+0xfe74`, derived
configuration at `hw+0xfe70`, and per-device configuration at `device+0x5c`.
Its 72 real registry requests produce the expected default FAA842/0 or
explicitly configured843/1 values. Both runs pass.

`--profiles --register` additionally executes actual allocation-profile
publisher `0x4ed00` and actual card registrar `0x27230`. The former publishes
the same real profile owner at `hw+0x1015c` and `device+0x69a8`; the latter
publishes the device through the real registered-card list. This enabled
configuration run also passes. These are selected genuine module initializers
on controlled CPU ABI owners, not a completed hardware startup.

Configuration startup additionally requests miniport property IOCTL `0x23212c`
with selectors5,31,32,22 (input8/output8 bytes, second input DWORD0).
The original helper explicitly returns an unsupported error, exercising
vendor defaults; it accepts a separate query handler. These requests were
initially called feature queries, but actual miniport descriptor mapping below
proves they are registry properties. No hardware-format capability, MMIO
status, command completion or FAA admission is supplied by this helper.

`--process` additionally executes the real module initializer `0x64800`,
which calls `0xbb800` and the actual default-option decoder `0x64530`.
It publishes the default snapshot at `hw+0x10514 -> device+0x14` and
its configuration owner at `hw+0x10518 -> device+0x18`. This closes the
missing per-process owner used by query2 `0x966a0`, without a hand-built
snapshot. The real default AA token changes from1 with842 to2 with843.
Both default and enabled runs pass; the enabled run also includes profiles
and registration. See [the compact owner capture](traces/m9-vendor-config-owner-process-20261006.json).

The remaining shared owner at `device+0x10` is acquired by actual MD
`0x84890` through miniport IOCTL `0x233038` (input20/output20 bytes).
Its reply `+0x0c` supplies an owner declaring size `0x21f068`; MD publishes
it at `0x84928`. Genuine MM dispatch `0x71a7c -> 0x71d8e -> 0x61e82`
returns the miniport owner `+8` here. MM publisher `0x15551` obtains its
mask from actual type14, 0x208-byte configuration record via `0x26d9d` /
`0x28e5f`, copying record `+0x204` to shared owner `+0x21f040`. Query2
uses bit5 of that mask; the ICD itself has not reached a PCI0540 restriction.
The actual record is now decoded from the captured M9128 EEPROM and
published by0x15551, as described below; full query2 still needs its
remaining normal owners and allocations.

### Hardware-format mask includes actual surface creation

Static continuation of the exact rejection producer `0x10044330` shows
its low mask bits are not simply a hardware feature mask returned by query2.
For every actual display owner, bit0 requires an existing primary surface
at `+4`, or successful creation by `0x1003a5e0 -> 0x1000aac0`. That normal
method constructs request type9, changing it to `0x02000009` when the
genuine OGL Multisample option is enabled; the 32-bit format is0x15.
Bit1 similarly requires the depth surface at `+0x0c`, or creation via
`0x1003a6d0` (type0x0a / enabled `0x0200000a`). Bit2 requires the next
surface at `+0x1c`, or creation via `0x1003a900`. The producer clears all
three low bits if any required surface creation fails.

Therefore the full normal FAA admission proof must include genuine surface
allocation after adapter capability acquisition. Returning an enabled mask
or only filling the manager collection would skip this real admission test.
The current fixture has an empty collection and never attempts these
allocations, so its early rejection still proves no M9128 restriction.

`tools/m9-vendor-faa-options.py --surface-boundary` now dynamically executes
the actual primary caller `0x1003a5e0`, its real surface-ID allocator,
`0x1000aac0`, manager dispatch `0x10030c70 -> 0x10030bb0`, and actual
request virtual method `0x1003b270 -> 0x1000b8c0`. It supplies a controlled
display DC and 1024x768 desktop rectangle, without inserting an adapter into
the collection or supplying successful resource allocation. The real request
stops at ExtEscape186a0, input0x48/output0x10, operation0x10, before any
device response. The resource descriptor at input+0x18 contains
`[type,0,1,0x15,1024,768,0,0]`.

[The four-case dynamic capture](traces/m9-vendor-faa-primary-allocation-boundary-20261006.json)
shows type9 when OGL.Multisample=0 and type0x02000009 when it is1. This
allocation type follows the OGL option alone, even when AAMode's enable bit
is off; the later pixel-format enable still requires both options. No
hardware capability, allocation reply, WGL multisampled format or complete
draw is admitted by this controlled caller fixture. It proves the exact
normal resource-request envelope to join to MD's real allocation handler.

Actual MD normal operation0x10 dispatch is
`DrvEscape0x3d7e0 -> 0x972e0 -> 0x921c0 -> 0x7cc50`. The operation table
entry targets `0x97523`. Full dispatch still requires a genuine ICD client
owner (display `+0x70`, request `+8`) and initialized adapter admission;
the controlled display caller above does not supply that owner.

`tools/m9-vendor-faa-allocation-translation.py` feeds the exact ICD-produced
descriptor into real MD leaf `0x7cc50`, with profiles genuinely published
by `0x4ed00`. Actual `0xc4d90 -> 0xc51c0` translation reaches `0xc6220`
before allocation. [The dynamic off/on capture](traces/m9-vendor-faa-primary-md-translation-20261006.json)
shows allocation flags2 versus0x20000002, allocation type7, width1024,
height768 and profile index12 (class2, hardware format0x66, 32 bits).
The surface descriptor flags are0x44 versus0x400044. Thus the request AA
bit0x02000000 survives real conversion as allocator flag0x20000000;
it does not disappear in ordinary translation.

This is a controlled client-wrapper CPU leaf proof, not full DrvEscape or
resource admission. It stops before `0xc6220` and supplies no allocation
reply. Static continuation of the primary allocation first requires actual
device `+0x69bc`, its display/resource owner `+0x54`, and device `+0x69a4`
constructor owner `+0x1074`. These ordinary owner initializers remain to
be joined to genuine hardware startup before a complete FAA sequence claim.

### Exact normal display and client owner publishers

Device `+0x69bc` aliases `hardware+0xfebc+head_index*0x90`, published
by genuine MD `0x84890` after IOCTL233038 and also at `0x2dfea` during
ordinary device initialization. Ordinary module `0x7abc0`, present in the
real startup table at `0x1f2628`, allocates0xf8, executes real `0x7aca0`
and publishes the display/resource owner at head `+0x54` (`0x7ac2a`).
Its initial surface pointers are genuinely zero until real allocation.

Device `+0x69a4` is published by ordinary module `0x1f990`, startup entry
`0x1f2600`. It allocates0x8368 and executes constructor `0x1cb90` before
publishing the completed owner. Exploration with the actual configured
CPU owner originally reached `0x1e1f0 -> 0x12340` and stopped because the
shared miniport owner at device `+0x10` was absent. That reader needs the
current configuration index at shared `+0x21f038`, its record type and
the runtime collection pointer at `+0x21f05c`. The later normal-runtime capture below closes this prerequisite with the
genuine initialized CPU runtime and completes the resource constructor.
Complete device initialization and FAA surface allocation remain separate.

The primary factory at resource owner `+0x1074` now has an exact producer.
It is `owner+0xfe4+0x90`: constructor `0x1cb90` installs the typed factory
block through actual `0x5f550`, then its successful tail `0x1de20` calls
`0x5f740`. The latter publishes resident pointers and passes them through
real typed virtual getters. With an empty engine inventory, the actual
result is owner `+0x1034`, vtable `0x1f40c0`, slot0 `0xb78f0`; its delegate
is owner `+0x1058`, vtable `0x1f3310`, slot0 `0xb6850`.
`tools/m9-vendor-resource-factory.py` executes those genuine CPU leaves and
asserts both factory identities, the constructor/getter visits, and that
device `+0x69a4` remains unpublished. The
[factory capture](traces/m9-vendor-resource-factory-20261006.json) is a
conditional ownership proof; it does not complete or bypass the earlier
shared-runtime requirement in the full resource constructor.

The other primary prerequisite, device `+0x6990`, also has a normal startup
publisher: module `0x273a0` at startup-table `0x1f25a0` calls `0x273c0`,
which assigns it at `0x273ff`. Before that assignment, actual `0x12340`
reads the same genuine device `+0x10` shared runtime owner. Thus both
resource branches depend on initialized runtime configuration;
neither requires an invented factory pointer or an extra FAA registry gate.

Actual M9128 bootstrap now retains the genuine decoded EEPROM record and
miniport object through `tools/m9emu/m9128_bootstrap.py`. On that exact
object, `tools/m9-vendor-runtime-owner.py` executes real MM `0x15551`,
which copies record fields `+0x204/+0x1ac/+0x1b0` into shared
`+0x21f040/+0x21f044/+0x21f048`: the actual values are2/0x1b/1.
Real `0x5de58` allocates the ordinary0x1ee400 CPU runtime owner and
`0x5fa80` publishes its collection at shared `+0x21f05c`. The
[runtime ownership capture](traces/m9-vendor-runtime-owner-20261006.json)
asserts exact copied fields and the genuine resident pointer, with no
manufactured capabilities or populated configuration profile.

Full ordinary runtime initialization is `0x568e7`. The early bounded run
stopped at its host-chipset PCI inventory request. The later complete
same-boot Nano inventory and bounded NT CPU contracts now let this actual
initializer return1, as shown below. The uncaptured transmitter-reset GPIO
in complete FindAdapter startup remains a separate hardware boundary.

The genuine ICD client publisher is `0x1003a2a0`. Actual
`0x1003d480 -> 0x1003b270 -> 0x1003d510` sends operation4, ExtEscape186a0
input0x38/output0x10; only a successful actual reply would publish client
handles into display `+0x70/+0x74` at `0x1003a39b`. The normal MD operation
table selects `0x97461 -> 0x960b0`; that handler uses head `+0x78` and
real constructors `0x873e0 -> 0x87590`, returning the actual client pointer
at output+8. These handles are the dependency used by later operation0x10.

`tools/m9-vendor-faa-options.py --client-boundary` now executes the real
ICD caller after genuine enabled option decoding and captures
[the exact operation4 envelope](traces/m9-vendor-display-client-boundary-20261006.json).
The fixture provides a controlled display DC and ordinary CPU process
pseudo-handle; it stops before any device reply or returned handles, and
does not admit a capability collection. Reserved/high-half stack bytes in
the request remain unspecified and must not be interpreted as device flags.

Both client and primary boundary fixtures now construct the display owner
with actual `0x10039d10`, using the vendor's0x218 allocation size, rather
than treating a zeroed wrapper as its constructor. The ordinary manager
`0x1002d010` allocates it and calls that exact constructor at `0x1002d1c3`;
subsequent `0x1003a180` metadata initialization and collection insertion
require the genuine query2 reply retained in manager `+0x220`. These
fixtures deliberately stop short of that hardware admission: only the DC
and desktop rectangle are controlled CPU caller inputs. All three boundary
captures pass with the real display constructor; no successful device reply
or capability flags are added.

### Real miniport property dispatch closes the four configuration queries

`--miniport-defaults` executes actual MD `0x61f70` and forwards its four
IOCTL `0x23212c` requests into a separate genuine MM emulator. Actual MM
`0x6ca3e` initializes each property from the real descriptor table
`0x55450`; real normal dispatch `0x70eab -> 0x71428 -> 0x5a228 -> 0x6cbbd`
performs registry access. The host explicitly reports missing Windows registry
keys; it supplies no values. Actual MM callback `0x7279d` and default logic
produce these replies, which are copied verbatim back to MD:

| Selector | Real property | Actual default DWORD | Reply second DWORD |
| --- | --- | --- | --- |
| 5 | System.BusMastering | ffffffff | 1 |
| 31 | User.LargeCursor | 0 | 1 |
| 32 | User.ForceHardwareColorCursor | 1 | 1 |
| 22 | System.MmEqualUnit | 0 | 1 |

Both FAA option cases complete the ordinary MD configuration/process owner
initializers; the enabled case also completes profile publication and card
registration. [The dynamic capture](traces/m9-vendor-miniport-property-defaults-20261006.json)
records actual default replies and actual registry keys. These four replies
are now obtained from vendor code rather than replacing them with unsupported
errors. The controlled miniport CPU owners are still not hardware initialized.

Static MM dispatch also resolves MD query2's IOCTL `0x23231c`, selector1:
`0x5d927 -> 0x5dc7b -> 0x5e0eb` reads runtime profile property0x19 and
returns owner `+0x48`, updated when that property's `+0x14` value is4.
This is a runtime policy state, not a direct GPU capability register. The
normal-runtime initialization below now populates its actual collection; this
query still needs a full dynamic join before an admission claim.

The normal option-matrix helper now uses these actual module publications
too: `0x61f70` and `0x64800`, with genuine miniport property dispatch, supply
the configuration queried by actual MD `0xd3e70 -> 0xd4180` and ICD
`0x10048d10`. Its earlier manual raw/derived attachment and zeroed process
snapshot have been removed. [The updated four-case capture](traces/m9-vendor-faa-options-owned-20261006.json)
reproduces enable gates0/0/0/1, 39 fallback formats, samples0 and the same
empty-collection rejection. The default process policy fields in the MD
response now come from the real decoder as well. The active query-owner
wrapper remains a controlled ABI input, and full hardware startup, display
collection population and real surface allocation remain unproved. The
capability-boundary mode still stops at the identical genuine query2 request.

### Exact serial source of the miniport capability record

The sole direct capability-record initializer call is MM `0x2e1c0` in
`0x2e0c5`. It creates a 128-byte type13 source and 0x208-byte type14 result,
copies the PCI device word into result `+4`, then obtains serial source
kind2 through `0x2f70c -> 0x2f9d4`. `0x2f682` reads offsets0..127 and counts
successful reads. `0x31f90` receives that real source and count, accepting
its extended profile only for at least128 bytes and header `0x000825d3`.
The accepted source supplies chip enumeration at `+0x0a` and retained flags;
the actual `0x3265f` fills result `+8/+0x1ac/+0x1b0/+0x204` by chip branch.
These producers now execute with the actual128-byte GPIO EEPROM capture.
The genuine decoded record has header3, chipWORD34, +1ac=1b, +1b0=1,
+204=2. Earlier pre-EEPROM snapshots contained only header/PCI and were
not negative hardware-capability evidence.

An unsuccessful or absent extended source has an actual vendor fallback
for PCI0540 to enumeration0x29. This is a conditional default branch, not
the observed M9128 result: its actual extended source was captured and
accepted. Do not
replace its hardware transaction with a synthetic absent profile. The only
`d3250800` occurrence in the captured M9128 ROM, offset0xb057, is the
immediate operand of a BIOS compare instruction, not a captured profile blob.


### Pool6 backing boundary and corrected companion recipe

The new [bounded resource proof](parhelia-m9-faa-companion-resource-20261006.md)
executes the actual pool6 request through genuine low-level geometry creation,
ending before its absent shared backing owner. It establishes a linear320x1,
1280-byte resource with no admitted physical address, not a successful pool6
allocation. It also corrects the older linked-kind9 companion claim: the actual
AA profile49 is class4/hardware3d/64-bit and its request-format5 refuses that
recursion. BA/BB names optional HZ through selector12.

Conditional real layout/descriptor builders produce B9=7a220140 and extent96020
for320x240/sample2, and B9=7a220400/extent600020 for1024x768. The existing native
diagnostic forces layout1 and reserves width*height*8 without those end guards;
odd/small dimensions also require the vendor's alignment arithmetic. Actual
sample2 admission, physical backing/content and complete command lifecycle
remain necessary before promoting this recipe to a native enabled FAA result.
### Normal miniport runtime initialization completes with actual PCI inputs

The [normal-runtime capture](traces/m9-vendor-runtime-normal-20261006.json)
executes actual `0x568e7` to return1, after real capability publication
`0x15551`. Its inputs retain the actual M9128 EEPROM-derived record and the
complete same-boot Nano PCI device and assigned-bus catalogs. The PCI host
returns captured configuration bytes, explicit absent device vendor IDs, or
absent-bus status; it stops on uncaptured bytes. It supplies no PCI writes.
Actual vendor code issues8250 PCI reads and1190 registry requests. The initially
empty CPU registry retains only the driver's own generated12-byte D00 value.

The resulting collection at202672b4 is published from owner202672b0. Runtime
type/index0 are actual initializer outputs. The sibling's bounded OS MDL host
provides synthetic system-RAM pages and reports the optional Windows Extio
manager absent; those pages are not GPU or VRAM addresses. This establishes
normal CPU runtime construction, including the missing configuration records
used by MD resource constructors. Complete adapter initialization, surface
allocation, draw and FAA resolve remain unproved.

Reproduce with `tools/m9-vendor-runtime-owner.py --normal --pci-inventory
PATH --bootstrap-tools PATH`, using the captured actual PCI inventory and
`tools/m9emu` bootstrap/NT-host dependencies (434e1ed9 or its integration).

### Real MD resource and engine constructors now admit the retained runtime

The [MD runtime-owner capture](traces/m9-vendor-md-runtime-owners-20261006.json)
retains the real MM heap addresses and forwards the MD configuration's four
property requests through that initialized miniport's actual0x70eab dispatch.
The initial MD device is a controlled CPU caller ABI object. Actual MM
0x71d8e handles the normal233038 request, and actual MD0x84890 publishes
its hardware/platform/shared pointers. The platform is the retained real MM
profile (OS500, BAR0 d0000000/10000000). Its256MiB aperture is separate
from installed1GiB VRAM. No startup success, GPU status or caps are supplied.

With that real shared runtime, actual MD0x273a0 returns1 and publishes
resident engine `device+6990`. Actual0x1f990 executes the complete0x1cb90
resource constructor and publishes `device+69a4`, including the real typed
primary factory at owner+1074: wrapper0xb78f0 delegates to0xb6850. This
supersedes the earlier runtime-null stops for these constructors. The
resource module's later platform policy call0x1fca0→0x62170 now completes
with that genuine platform owner. Actual normal policy-owner module0x849a0
constructs and publishes hardware+35fc; it is separate from the memory
manager at+35f8. Executing0x849a0 before the other resource modules lets
all four complete with return1:0x849a0,0x273a0,0x7abc0,0x1f990. Actual
0x7abc0 publishes head+54; full0x1f990 publishes its actual typed factory.
These selected ordinary modules do not complete all adapter startup.


Actual derived config+338 virtual getter returns101f8; actual admission
helper0x477f0 for hardwareformat3d returns1, confirming the companion's
required config masks and format-table flag from vendor code. This admission
is not allocation success. The AA surface constructor, VRAM backing, full
normal pixel-format admission, render and resolve remain to be joined.

### Main and auxiliary stores have exact linear rows and a32-byte guard

`tools/m9-vendor-faa-store-requests.py` executes actual0xac7e0 with a real
vendor-constructed CPU surface and controlled caller geometry, using the
retained normal MM runtime and actual published MD profiles. The
[request capture](traces/m9-vendor-faa-store-requests-20261006.json) stops
before its first0x51870 allocation. Main request flags8002, format8 and
profile12 (class2/hardware66/32bits) are actual constructor outputs.

The main budget is `min(floor(width*height*344*0.38499999046325684)>>3,
32MiB)`. Actual request width is1024; rows are
`max(1,floor((budget+1023)/4096))`. The320x242 case has budget1282019,
remainder4067, and requests313 rows: simply flooring budget/4096 would
incorrectly give312. Tiny1x1 and2x2 requests both preserve one row.

The [layout capture](traces/m9-vendor-faa-store-layout-20261006.json)
executes the real0x19560 scalar prefix against the captured M9128 cleanroom
postinit f04=0448803e, then actual0xac7e0→0x51990→0x45eb0 until layout
completes, before physical backing. Real manager+24/+28 become2/1. This
is an explicit CPU MMIO model of the actual cleanroom baseline, not a
claim about vendor cold state or completed memory-manager initialization.
The resulting store is linear (layoutflag1), with exact requested rows and
extent `1024*rows*4+32`. At1024x768 this is13017120 bytes, and the
32MiB budget cap still requires33554464 bytes including the guard.

Auxiliary static producer0xac8ec..0xac94d uses the same profile/width,
with budget `main_budget>>4`, or fixed2MiB when device+1c bit0 is set.
The capture executes its genuine0x45eb0 layout separately with those
conditional inputs, without fabricating a successful main allocation to
reach the later constructor. At1024x768 the ordinary auxiliary layout is
1024x198/811040 bytes; the fixed branch is1024x512/2097184 bytes.
Both include the32-byte guard. These are conditional builder proofs;
full auxiliary allocation, contents and lifetime remain unproved.

The independent CPU request fixtures explicitly set x87 control word027f:
53-bit precision, round to nearest, exceptions masked. The vendor then saves
and temporarily changes its rounding mode for conversion. This caller FP ABI
is controlled and recorded, not recovered hardware state. Leaving Unicorn's
zero control word active introduced24-bit precision carry between interrupted
queries; it changed some byte budgets by one while preserving these row counts.
Microsoft documents the distinct x86 precision controls in
[the CRT control-word API](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/control87-controlfp-control87-2?view=msvc-170).
The corrected six-case captures are nonempty parsed JSON, including tiny cases,
the rounding boundary and both conditional auxiliary branches. The earlier
empty capture checkpoint is superseded.

Native source7443ff6 now corrects main/ordinary auxiliary extents together,
with source regression d8819db retaining guards after BO page rounding.
Actual-C six-case vendor comparison, ownership ASAN/UBSAN, full batch suite
and DSO build pass. Root independently reran the genuine vendor layout
producer with retained current-board PCI inputs. This source now passes current-M9128 same-context guarded BO growth with
AA raster mode off, then ordinary texture; see
[resource proof](../tests/m9-faa-guards-native-20261006/README.md).
Backing/init/completeFAA command state remain open.

### Two E6 operations and the cached sentinel address are distinct

The fb920 binding operation's actual raw E6 GPU payload is
`[6dc00000,1,1]`; the complete context restore path's actual raw payload is
`[6dc00000,42000000,a1]`. Both have passed the real25680/24870 encoders in
the integrated resource-address/context-fragment-state captures. Their
trailing values are distinct GPU words, not interchangeable recorder metadata.
The shared first opcode does not justify substituting one complete operation
for the other.

Actual AA binder0xaa750 obtains the control resource passed at binder
array+0x0c through its kind5 getter and places that address
at context shadow+74/+78 (aa819..aa82d). It obtains the independent per-head
pool6 resource at hardware+fed8+head*144 and places that address at shadow
+84/+88 (aad07..aad68), also duplicating it into AA state+9c/+a0 for fb920.
The real restore emitter0xfcb60 later copies the current sentinel74/78 into
cached7c/80 (fce05..fce39). The save emitter0xfce50 passes cached7c/80 and
pool6 84/88 into0xfc8f0. These are separate resource references; the store,
sentinel and per-head pool6 must preserve their own ownership and lifetime.
This resolves the static shadow producer split. The caller-array ownership
join to the sentinel allocated by0xac7e0 at its interface+18 still requires
the complete resource graph; no pointer alias is assumed. Full allocated
resource admission remains unproved.

### Actual caller reference publication closes the sentinel array position

`tools/m9-vendor-faa-binder-graph.py` executes real MxMD0xa7200 with explicitly
controlled CPU wrapper/reference inputs. The actual code returns1 and copies
primary, companion, main, sentinel into both participating AAouter arrays at
+8,+c,+10,+14, then marks+18 active. At0xa72d2/+0xa72ed it obtains main and
sentinel from the source AAouter+24/+34: these are exactly the owning
ac7e0 interface+8/+18 because that interface begins at outer+1c. This closes
the caller-array position left conditional above: actualaa750(outer+8)
receives the main resource at array+8 and the sentinel at array+c.

The [bounded reference-copy capture](traces/m9-vendor-faa-binder-graph-20261006.json)
is CPU ABI evidence. Its wrapper pointers and source ownership flag are supplied
caller inputs; it supplies no successful GPU allocator outputs and proves no
normal display admission, backing allocation, or rendered result. The separate
per-head pool6 binding at context84/88 remains a different resource.

### Genuine optional HZ request and initialization boundary

`tools/m9-vendor-faa-hz-request.py` executes real0x54be0 against the actual
configured/runtime graph and controlled companion geometry/profile fields.
The genuine configuration334 getter returns7, admitting bit4 for the 64-bit
companion. The [request capture](traces/m9-vendor-faa-hz-requests-20261006.json)
shows flags2, logical format0xb, profile56(class5/hardwareformat0x78/32bits),
width ceil(companionwidth/8), height ceil(companionheight/4), units/depth1.
320x240 produces40x60;1024x768 produces128x192. No backing reply is supplied.

The ordinary constructor invokes0x54be0 at0x51d68 when profileclass4 and
resourceformat5 match, setting companion+39c first. If the helper fails, actual
0x51d74 clears that flag and continues; this is a vendor optional-resource
fallback, not proof of the present native renderer. On success0x54cd3 invokes
0x55070, which calls0x54e80 then0xb4fe0 to issue a clear using depth0..1 and
mask4. Capturing that real initialization command stream still requires the
returned shared allocator and resource backing; the request proof does not
claim HZ contents, completion or FAA rendering.

### Returned allocator and genuine allocated FAA resources

`tools/m9-vendor-faa-owned-resources.py` uses the sibling reusable
`m9_allocator_fixture.initialize`: real shared allocator0x19420 returns1,
publishing device6984/6988/698c. The actual OS-owned aperture mapping is an
explicit CPU caller input; its normal85d50 refresh still has not returned.
No GPU completion is supplied. Actualac7e0 then allocates the main, auxiliary,
companion and HZ resources before stopping at the first real HZ GPU-clear
0xb4fe0. The [owned-resource capture](traces/m9-vendor-faa-owned-resources-20261006.json)
contains the real objects, backing descriptors, input/return layout stages,
and separate sentinel/pool6 constructors and lifetime checks.

At320x240 the stages are distinct:

| Resource | Actual45eb0 raw return/flags | Stored aligned extent/allocated flags |
|---|---:|---:|
| Main1024x310 |136020/1|136080/3|
| Auxiliary1024x19 |13020/1|13080/3|
| Companion320x240/sample2 |96020/9|97000/b|
| HZ40x60 |2d00/1|3000/3|

Values are hexadecimal. The earlier32-byte geometry guard remains correct:
actual45eb0 returns the raw protected size in EAX, then at464ca..464d9
final-aligns the stored descriptor extent+180 using alignment+174 (128 bytes
for these linear resources,4096 for tiled resources). These stored extents
already exist before backing. Later4dfe0 publishes address15c and marks
allocated layoutbit2. A native allocation rounded to full pages already covers
this padding. Distinguish the raw function return, stored aligned extent and
subsequent allocated-address publication.

An independent actuala3870 sentinel request uses the same flags8202/profile56/
format0/5x1 asac7e0. The genuineac7e0 CPU initialization prefixaca7b..acb06
maps the real allocated wrapper, writes exactly
`a4a3a1a1 28041968 00000000 00000000 00000000`, and unmaps. Its actual backing
extent is80hex with row8. This independent prefix does not return a fake
successful clear to the interruptedac7e0 frame.

Real751f0 constructs the pool6 CPU owner; explicitly controlled owner fields
select the actual hardware/device/head0. Actual76540 allocates the0x500-byte
request with extent580hex, then reuses the identical interface on the second
call. Wrapper reference count increases2→3 (constructor baseline1). The real
kind5 getter returns a direct address pair for pool6, while the flags8202
sentinel returns tag40000000 and a pointer to its real wrapper relocation
record: actuala6740 selects this record when wrapper+a8 is set. These are
separate binding forms and separate resources.

This checkpoint proves actual vendor allocation outputs and sentinel CPU
initialization on modeled OS-owned storage. Pool6 semantic initialization,
HZ/companion GPU clears, enabled FAA draw/resolve and normal display admission
remain open. No rendered FAA fragment is claimed.

### Owned addresses translated by actual command emitters

The owned-resource runner now also executes actualfb920/fce50 and the real
stream relocation routines25680/24870 using the allocated references above.
Stream storage and its reference-recording callback are controlled CPU caller
ABI inputs; the callback supplies no resource allocation or GPU completion.
`tools/check-m9-faa-owned-commands.py` verifies the backing provenance and
encoded address offsets against the generated capture.

Actualfb920 binds pool6 at index24 using kind2b, and main/auxiliary at BC/BD
using kinds9/a. All three payloads preserve the allocated GPU address's low
bit1 tag. It emits E6 payload `6dc00000 00000001 00000001`.

Actualfce50 uses distinct cached references: sentinel at shadow7c/80, pool6 at
84/88. Pool6 restoration produces kind2c/2f payloads `(pool6+284)|1` and
`(pool6+2c4)|1`. The A1/A3/A4 fragment-pointer save uses kind30 and sentinel
offsets8/c/10. The sentinel's input pairs contain tag40000000 plus the offset,
and point to the real wrapper+74 relocation record. Actual25680 reads that
record's cached encoded GPU address and produces `(sentinel+offset)|1`.
The CPU wrapper/backing addresses therefore do not enter the GPU stream.
This context path's E6 payload is `6dc00000 42000000 000000a1`, followed by
the final save E6 `40008000`; it differs from the fb920 binding operation.

The sentinel's second initialization word28041968 is a literal immediate:
pinned instruction0xacae1 bytes `c7 40 04 68 19 04 28` encode
`mov [eax+4],0x28041968`. It is reused by the reset path at0xacb9b. Its
numerical overlap with this model's heap range does not make it an owner
pointer or an address relocation; the native initialization should retain
the literal value. This proof closes command address translation and the
sentinel literal provenance, while HZ clear construction and rendered FAA
remain separate outstanding requirements.

### First-use sentinel restore precedes cached state save

The same owned-resource capture now executes actualfbfc0 fragment construction,
then fce50 before any restore, actualfcb60 with the allocated current sentinel,
and finally fce50 using the cache published by that real restore. Context
construction initializes current74/78, cached7c/80 and pool684/88 to null.
The fixture supplies the
real allocated pool6 pair as an explicit caller binding.

Before the first restore, fce50 emits zero bytes despite its dirtybit8 being
requested: actualfc8f0 rejects a null cached sentinel. First fcb60 uses the
current sentinel and emits20 actual raw words. Its real common-counter fence
packet begins `4545e3e6 1 21` in this capture. The remainder contains E6
`42000000`, eight literal28041968 index44 writes, E6 `3 3`, then index2e/2f
references using kind2c/2e. These encode `sentinel|1` and `sentinel+14`:
kind2e clears the low two address bits, unlike the tagged kind2c path.
No pool6 address appears in this first sentinel-restore command stream.

Only after these commands does actualfcb60 copy current74/78 into cached7c/80
atfce05..fce39. Subsequentfce50 emits the29-word pool6/context save plus
A1/A3/A4 sentinel save sequence already proved above. Thus the previous
capture's preseeded cached sentinel was a command-encoding fixture, not a
first-use policy. Normal context switching atf8fd0 saves the old owner only
if one exists, then restores a new owner's sentinel when dirtybit4 is set;
thefc342 dispatcher similarly processes savebit8 before restorebit4.

This checkpoint executes actual CPU constructors, counter/reference handling
and command production. It supplies no GPU completion or fabricated counter
readback. The commands have not been executed by hardware here, so pool6 GPU
state acquisition semantics and complete enabled draw/resolve remain open.

Constructor attribution correction: the initial first-use fixture called
fce90, the large shader-state constructor. The reproducible runner and capture
now use the actual fragment-control constructorfbfc0, whose ordinary callers
are1edf6/1f226/1f650. This also establishes a null initial pool6 pair, rather
than interpreting unrelated shader default words at84/88 as resource state.
The actualfcb60/fce50 command encoding and cached-sentinel behavior are unchanged.

### Real allocated AA binder joins surface and fragment controls

With `--binder`, the owned-resource runner executes ordinary1a260/1ff90
queue/context publishers and realaa750 with the actual allocated FAA graph.
A separate genuinea3870 request creates the primary resource using explicit
profile12/format0/geometry caller inputs. The binder stops at0xaadcc after
publishing CPU shadows and before queue activation; it neither resumes the
interrupted HZ clear nor supplies successful GPU results.

The actual main+414 and companion+38c links identify the allocated auxiliary
and HZ objects. Aa750 copies the actual sentinel to current74/78, leaves cached
7c/80 null, and copies actual pool6 to84/88. This independently joins the
first-use policy above to genuine binder output, rather than pre-seeding a
cached resource. The runner then executes actualfb920 andfc0e0 with explicit
stream storage and a reference-recording CPU callback only.

At320x240 actualfb920 produces41 raw GPU words: a genuine counter packet,
primary B0/B1, companion B8/B9 (`address|1`, `7a220140`), HZ BA/BB
(`address|1`, `f0010140`), index24/E6/BCBD, and remaining surface state.
The four AD writes hold `9b04`, the actual main stored extent136080 divided
by32, while the AF write is0 in this capture. These are capacity/surface
fields. They must not be conflated with fragment-control shadow20/28.

Actualfc0e0 consumes those separate fragment fields and emits
`45d7eff0 e0000400 20007000 00000000`: the controls target **F0/EF**, with
D7 last. Calling the shadow28/20 values AD/AF would name the wrong registers.
The independent [1024x768 capture](traces/m9-vendor-faa-owned-binder-1024x768-20261006.json)
and the320x240 owned-resource capture both pass the address/provenance checker.
The emitted queued counter values are model-session outputs, not native
constants or fabricated completed counters.

This establishes the allocated CPU binder and actual packet mapping. Queue
activation, HZ initialization execution, kernel-private indirect state
ownership, enabled draw/resolve and hardware FAA rendering remain open.

### Owned FAA output publisher and ordinary compiler order (2026-10-06)

The actual ordinary copy/map path reaches `a74b0→ac1f0` at `a7528`.
When the AA outer owner has companion/main objects, `ac1f0` allocates a
temporary profile14/format6 destination and calls `abc20` at `ac2d4`, then
uses ordinary `afbe0` to copy that output to the requested destination. Its
allocation-failure path calls the same publisher for 64×64 tiles. This is
the concrete caller provenance for the FAA output/resolve candidate;
`abc20` is not an otherwise unreachable emitter.

[Owned resolve capture](traces/m9-vendor-faa-owned-resolve-20261006.json)
executes real `abc20(destination,companion,main,x0,y0,x1,y1)` on returned
vendor allocator objects. It stops at `ac0ac`, after actual shadow publication
and before actual queue dispatch at `ac0e3`. The real main→auxiliary and
companion→HZ links are required. Mode `AAshadow+14` changes from draw `1f`
to output `1d`, `+20=ffff`, and main/auxiliary/pool6 address pairs remain
the genuine owned tagged addresses. Fragment `+28=e0000400` and
`+20=20007000` still target **F0/EF**, and the requested rectangle publishes
fragment `+30=origin`, `+34=width|(height<<16)`.

`tools/faaemu/m9_owned_faa_resolve.py` retains that bounded default and can
execute the same actual call without its boundary once the genuine queue is
available. It never supplies a successful dispatch or GPU completion. The
post-dispatch backing-lifetime updates (`ac101..ac131`) and fragment save bit
(`ac139`) have not executed in this capture. This capture does not prove a
completed FAA resolve, normal pixel-format admission or hardware rendering.

The actual ordinary state compiler `fadf0` emits **`fc0e0` before `fb920`**
(`fae02`, `fae0e`), followed by `f4f50`, `f6f20`, `fb260`. Earlier independent
emitter fixtures intentionally captured each encoder separately; their JSON
field order is not the normal submission order. The complete-frame trace
must run the genuine queue/compiler and preserve this order, including the
first-use sentinel restore before the pool6 producer.

Correction to preliminary caller investigation: `a7390` calls interface+8
virtual+6c, whose actual constructor-published vtable is `1f3c50`; slot
`1f3cbc` contains **`a7440`**, the AA binder refresh. `a7390` then calls
`ac150` to clear AA ownership flags. Neither that flag clear nor this
virtual call is the `ac1f0→abc20` output path.

The pinned ordinary compiler switch provides the remaining trigger location:
`abc20` publishes context `+10=9`; real `f85c0` dispatches modes7..10 through
table `f8fac`, whose mode9 entry is `f8e36`. That branch calls `fadf0`, then
emits `45fcf3ec,0,fragment+30,fragment+34` at `f8e94..f8ed7` before the usual
pending-resource bookkeeping. This branch and write are **static pinned image
provenance**, not executed compiler evidence in the bounded output capture.
Capturing FAA bindings alone omits this rectangle operation; the full-frame
queue trace must also reach the actual trigger and retirement boundary.

## Normal D3D option and CPU frontend prerequisites (2026-10-06)

The ordinary raw descriptor at `b9557` names `DirectX.D3DEnable`; default0
leaves the `78480→77130` callback advertisement disabled. Supplying the real
registry option1 during genuine `61f70` enables callback flags0x0c and the
actual `DrawPrimitives2=beeb0` entry. Enabling that option also causes a real
`23231c`, selector1 runtime-policy query during configuration. Ordinary D3D
module setup subsequently queries selector2. Both execute actual MM
`5d927→5dc7b→5e0eb` on the retained miniport owner and return0, four bytes,
value0 in the current actual runtime. These queries belong to `5d927`, **not**
the registry/property dispatcher `70eab`; no capability reply is substituted.
This is normal option/callback reachability, not a complete FAA frame or GPU
rendering claim.

The real `bec70` context callback uses the registry owners published by
ordinary module `79ed0` (`head+20/+24`). Its `beb40` call creates typed
resource collections through genuine `7a170/c04e0`, then passes them to
`753e0`. The returned queue TaskMgr permits actual `11b9b0` vertex/backend
construction. That construction calls `7b3e0`, whose genuine CPU allocator
ABI is vtable+8 `(size,flags,tag)`; bounded host support now retains those CPU
allocations (observed flags2, tag0x58544d44). The queue itself previously
needed vtable+10 `(size,file,line)` from actual `f829e`.

An initial `f5969` failure in both clear and context construction was a fixture
ordering defect: shader setup `1ff90→1e490→f5d60` consumes `queue+30`.
The genuine required order is `1a260→230f0(return1)→1ff90`; constructing
shader state before activation passed a null TaskMgr. The corrected full
clear/draw/output capture is still the integration gate. No null shader
owner, successful vendor return or GPU retirement is supplied by hand.

### Joined minimal FAA frame, 2026-10-06

`tools/m9-vendor-faa-frame.py` now executes the returned queue and public D3D
context, all three real `ac7e0` initialization clears, `aa750` FAA binding,
public `beeb0` DP2 XYZRHW/diffuse triangle lowering through `17e480`, and
`abc20` output through dispatch and `ac139` lifetime bookkeeping. Actual
`fb920` observes FAA mode `0x1f` for the triangle and `0x1d` for output. The
triangle emits `0x23000001`; the output compiler emits `0x45fcf3ec`.
`docs/traces/m9-vendor-faa-frame-20261006.json` records the joined frame.
`tools/check-m9-faa-frame.py` verifies successful public context/draw callers,
ordered compiler modes, resource links, typed relocations, and exact CPU-written
DMA images against all 32 live OS buffer bounds. Capture starts before queue
construction, includes context initialization, and exports only written bytes.
Every exported byte must equal the final actual CPU write for that stage;
untouched emulator memory is never evidence of an initialized GPU program.

Pool6's actual kind-5 address pair is `{offset=0, tagged_base=GPU_address|1}`,
cached at fragment `+0x84/+0x88`. The real getter and all actual kind-`0x2b`
translations agree. The earlier interpretation `+0x88/+0x8c` was a shifted
scalar read, not an address pair. Kind-`0x2c`/`0x2f` save-state endpoints use
pool6 offsets `0x284`/`0x2c4`; kind-`0x30` writes sentinel slots `8/c/10`.
Sentinel's second initial word `0x28041968` is a pinned literal instruction,
not a CPU heap pointer. Its actual CPU initializer is `[a4a3a1a1,28041968,0,0,0]`.

This proves CPU command production with genuine returned resource/queue owners
and controlled Windows CPU API inputs. It does not prove normal display mapper
admission, GPU execution, retirement, antialiasing, or a Linux screenshot.
Opaque indirect saved-state backing must remain kernel-owned and cannot be
admitted as client-written VRAM. Pool6 initial GPU-written state and additional
shader/constant resource write provenance remain explicit translation work.
The sibling retirement trace executes the same returned frame through the final
counter/flush and stops before the first GPU status read; it supplies no completion.

### Surface admission and triangle coherence correction

The preceding joined CPU-caller trace is **not a coherent FAA rendering frame**.
Although the shared AA shadow keeps `0x1f`, the actual public triangle compiler
replaces the previously bound FAA companion/HZ with ordinary depth state and
selects default MRT outputs. It emits `AD=20`, `AF=0`, `F0=f0000400`,
`EF=00288800`, and a direct `23000001` launch. Those writes cannot establish
an antialiased final color draw. Do not replay that image as an FAA hardware test.

The missing normal graph publication is now dynamically recovered separately:
`tools/m9-vendor-faa-surface-activation.py` executes real `a77c0` on a genuine
sampled color wrapper (`01000002`, profile12/class2/hfmt66/32-bit). It returns1,
sets surface flags `+428 bit8`, `+270=1`, and owner `+420/+3c=1`; real `a787d`
calls `ac7e0` with the graph destination **surface-owner+1c**, and real `a78cc`
executes backing registry insertion. No graph field is hand-published.
`docs/traces/m9-vendor-faa-surface-activation-20261006.json` retains all five
returned resources and exact caller/owner addresses. Primary profile12 is a
valid class2 color profile; its previous use was not by itself the admission bug.

The activated wrapper still has public resource handle `+bc=0`, and public
`753e0` consequently cannot select it through initial `2d740` target binding.
The normal `7a6e0→a3780→885e0` surface association/handle publisher is the next
join. Actual DP2 render-state and viewport requests are supported as explicit
caller inputs in the draw fixture; they do not replace normal resource owners.
A direct GPU-address lookup in BAR0 validity is not sufficient to classify
pool6/sentinel initialization: the GPU virtual address must first be joined to
its actual CPU mapping/physical aperture domain. No empty validity mask is
currently admitted as evidence that the resources are uninitialized.

The handle/current-target join now succeeds through bounded genuine `7a770`:
real `beb40/c04a0` creates and locates the DD-local registry, then
`7a770→a3710→a9ab0→885e0→7a7d7` publishes wrapper handle1. Public `bec70`
context creation reuses these exact OS DD-local/global pointers, returns1 with
HRESULT0, and retains both the actual registry table and handle1 at
`context+34/+8` and `+c4`. The reusable context fixture fails if either owner
changes. `tools/m9-vendor-faa-associated-context.py` reproduces this contract;
the associated-context trace records the genuine addresses and ownership.

This bounded producer is below public `bf0e0`'s still-unjoined display-enabled
Boolean at PDEV+30; it neither sets that field nor claims public surface-callback
admission. The first real draw now reaches render-target validation `75788`,
which requires the ordinary `hardware+fea8[head]` owner. The earlier handle0
fixture skipped this branch. Triangle emission and coherent FAA rendering are
still unproved at this checkpoint; no executable Linux FAA frame is admitted.

### Coherent surface-owned FAA triangle command production

The genuine ordinary `20810` target-owner publisher now returns1 after real
MM `2322a0` dispatch using measured M9128 DC-bank inputs. The normal frame
fixture activates the actual primary-owned graph through `a77c0`, associates
its actual handle through bounded `7a770`, and reuses those DD owners through
public `bec70` context creation and DP2 drawing. It supplies the real DP2
viewport and render-state requests. It does not call the old standalone
graph initializer or manually insert an AA binder between context and drawing.

The real triangle has **two ordered launches**, both `23000001`: an ordinary
helper operation with count4, then the FAA operation with count3. The second
operation retains primary `B0=e81001`, companion `B8=1021001/B9=7a220140`,
HZ `BA=10b9001/BB=f0010140`, main `BC=ed1001`, auxiliary `BD=1008001`, and
`AD=9b04` (returned main extent `136080 >> 5`). Its actual mode words are
`F0=c4008600`, `EF=f8004055`, `A8=38038e`, and `AF=0`. A `63000001` launch
must not be assumed mandatory when the genuine vendor producer emits these
two `23000001` operations.

The companion descriptor comes from real `aab42→467e0` **selector13**, cached
at layout `a4/a8/ac` with validity bit `200000`. The HZ descriptor uses
selector12 at `98/9c/a0` with bit `100000`. Requiring companion selector12
would reject the genuine producer despite correct graph bindings.

`tools/check-m9-faa-coherence.py` validates exact actual CPU-write provenance
for every exported byte, real registry/handle retention, both ordered launches,
the final target/FAA graph addresses, companion13/HZ12 descriptors, capacity,
and the actual second AA shadow. The normal-frame and normal-coherence traces
retain the positive receipt. This proves coherent **CPU command production**
through the real output/resolve caller. Public `bf0e0` display admission,
kernel executable translation, GPU execution/retirement, antialiasing and a
Linux screenshot remain separate validation gates.
