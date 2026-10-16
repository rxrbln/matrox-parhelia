# M9 multisampling (FAA): static closure, captures, and what the silicon did

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

## 2026-10-08 latest: later CLEAR continuation retires; COLOR-pass still fails pixels

Canonical DRM5f07050 integrates source-native CLEAR3 continuation0x24 and
explicit COLOR-pass diagnostic0x21 flag1. A genuine same-fd INITb0/FIRST1c0/
Unlock1e0/prefix260/resumedLock340/Unlock360 completes on the M9128 with
generation0, empty queue and no wedge. Both returned F021 views are entirely
zero: this is transport evidence, not a rendered triangle. Strict receipt
sealing currently refuses duplicate same-boot dmesg from an earlier failed
firmware deployment; original raw logs remain preserved pending scoped review.

The independent SELECTARG2 COLOR-pass control then retires Lock200/Unlock220
but produces the same single wrong origin pixel as MODULATE. PRE sampled view
was zero; post-Lock sampled view has exactly one nonzero DWORD. This failure
occurs without texture sampling, narrowing the next source review to shared
preprocessing/raster/retained state. MAIN has five nonzero DWORDs, AUX none;
metadata is not established coverage. No textured application FAA pass.
[Exact raw proof and failing pixel oracle](../tests/m9-faa-dual-colorpass-source-20261008/hardware-20261008/README.txt).

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

Latest boundary, October7wind-down: two retained live-shader FAA triangles,
changed geometry, readbacks/Unlocks and cleanup pass on M9128. New variable
COLOR upload/fragment compiler work has host proof only; full application FAA
remains unfinished. Work is paused by user request. See the
[current handoff](parhelia-m9-wind-down-20261007.md); older milestones below
remain historical.

Current milestone (2026-10-07): the retained source-derived graph renders a
correct real M9128 antialiased red triangle through normal Lock/readback:
9361fully red+440partial pixels, no interior/exterior/channel errors, no wedge.
See the [hardware image and proof](../tests/m9-faa-normal-lock-view-20261007/README.md).
This is a bounded320×240 diagnostic; application FAA remains open. Older
sections below retain historical findings and must not override this receipt.


Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note closes the multisampling investigation for M9 (PCI `102b:0540`, M9128/M9148, `MSerX` images)
as far as it can be closed without booting Windows, and records exactly what
the M9128 did when the recovered state was written to it.

It supersedes the multisampling parts of
`parhelia-m9-gigacolor-faa-static-closure.md` where the two disagree; that
note's GigaColor half stands.  `parhelia-antialiasing.md` describes the
**older Parhelia-512** fragment buffer and is kept separate on purpose: its
register meanings are not imported here, and section 5 below says which of
its constructs exist on the M-series and which do not.

Every statement carries a label:

* **STATIC** - read out of a saved vendor binary; binary, address and bytes
  are given.
* **CAPTURED** - counted over saved vendor command streams by
  `tools/m9-faa-capture-evidence.py`.
* **MEASURED** - run on the M9128 in this driver, with the readback that
  shows it.
* **DERIVED** - a deterministic consequence of labelled facts above it.
* **OPEN** - not established, with the experiment that would establish it.

Images, with the address model verified before anything was read from them:

| image | base | file offset |
|---|---|---|
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll` | `0x10000` | VA - 0x10000 |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMGLX.dll` | `0x10000000` | VA - 0x10000000 |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMM.sys` | `0x10000` | VA - 0x10000 |

Run the two deterministic verifiers from the repository root:

```sh
python3 tools/m9-gigacolor-faa-static-evidence.py
python3 tools/m9-faa-capture-evidence.py
```

## 1. The shape of the feature

The M-series multisample state the vendor's Direct3D user-mode driver builds
is **two dwords and nothing else**.  Compiler `0x139240` writes them to
validator `+0x368` and `+0x36c`; lowerer `0x181fa0` copies them into the
surface shadow at `+0x28`/`+0x2c` (`0x1826a5`, `0x1826ae`); emitter `0xfb920`
writes them into the command stream [STATIC]:

```
000fbe09  c7 40 04 ad ad ad ad   header 0xadadadad   ; four values, index ad
000fbe13..000fbe31               four writes of [edi+0x28]
000fbe37  c7 40 18 af 45 45 45   header 0x454545af   ; one value, index af
000fbe41  8b 5f 2c / 89 58 1c    one write of [edi+0x2c]
```

`0x45` is the pad index of this command grammar, so those headers are four
writes to index `ad` and one to index `af`.  The byte patterns
`c74004adadadad` and `c74018af454545` occur exactly once in `MxMD.dll` and in
no other saved binary [STATIC].  This note calls the two dwords **AD** and
**AF** after those indices.

Neither index takes a relocation in this project's kernel validator, so no
address reaches the unit through them [STATIC, `m9_submit.c`
`m9_index_reloc_kinds`].

## 2. AD - the colour allocation extent, and nothing else

### 2.1 Construction [STATIC]

Before the backed surface is known, `0x1392e4..0x139304` seeds AD:

```
001392e4  8b b7 68 03 00 00            mov esi, [edi+0x368]
001392ea  81 a7 bc 01 00 00 8f 7f ff ff  and dword ptr [edi+0x1bc], 0xffff7f8f
001392f4  81 e6 20 00 e0 ff            and esi, 0xffe00020
001392fa  83 ce 20                     or  esi, 0x20
00139304  89 b7 68 03 00 00            mov [edi+0x368], esi
```

On a backed surface AD is then **zeroed first** and rebuilt:

```
001395aa  c7 87 68 03 00 00 00 00 00 00   mov dword ptr [edi+0x368], 0
001395b4  8b 01 / 8b 50 60 / ff d2        call surface vtable +0x60 (0xa6d00)
001395bf  c1 e8 05                        shr eax, 5
001395c2  3d ff ff 0f 00 / 72 05 / b8 ff ff 0f 00   eax = min(len>>5, 0xfffff)
001395ce  8b 8f 68 03 00 00 / 33 c8 / 81 e1 ff ff 1f 00 / 31 8f 68 03 00 00
```

so the final AD on a backed surface is exactly `min(allocation_bytes >> 5,
0xfffff)`, with every upper bit clear - the seed does not survive.  The
existing closure note's `AD = (AD & ~0x1fffff) | min(...)` is numerically
right only because the preserved field happens to be zero there [DERIVED].

When the length getter returns zero there are exactly two reachable forms,
both `0x20` from the nested initializer at `0x12e589` [STATIC].

### 2.2 There is no sample-count multiplier [STATIC, clean negative]

Resource-layout constructor `0x45eb0` computes the allocation size at
`0x46470..0x464d9` from aligned width, aligned height, depth/array count,
bits per pixel, a guard band and an alignment round-up.  There is no further
multiplicand and no second term.  The total at `+0x180` is copied unchanged
into the allocation request (`0x5c386 8b 91 80 01 00 00`, `0x5c391 89 57 1c`).
All 27 callers of `0x45eb0` were listed; none reaches it through a
multisample type or quality field.

CAPTURED confirms it from the other side: over 1206 decoded vendor passes no
captured surface descriptor carries a colour span an allocation factor would
enlarge, and the one submission whose span is twice its clip width is an
ordinary 256-wide surface drawn into a sub-window, the same descriptor
appearing 273 times with a full 256x256 clip.

### 2.3 AD is inert on the M9128 [MEASURED]

Fixture `faarect` in `tools/m9-mesa-glsl.c`, one rectangle on integer pixel
edges into a 320x240 RGBA8 target, interior pixel count read back:

| AD written | interior pixels |
|---|---|
| off form `0x20` | 48000 |
| `0x00`, `0x10`, `0x20`, `0x30`, `0x40`, `0x50`, `0x60`, `0x70` | 48000 each |
| `0x01`, `0x1fffff` | 48000 each |
| single bit 21..30 set beside the true extent | 48000 each |

Twenty distinct AD values, including the true allocation extent
(`0x2800` for this target, traced by the driver), all render identically and
none stops the engine.  **AD carries no enable and no observable behaviour on
its own.**

This closes gate 1 of the earlier note as a negative: `allocation_byte_length`
implies neither a sample-count multiplier nor a physical fragment
permutation.

## 3. AF - a sample-coverage word, fully named

### 3.1 The construction, corrected [STATIC]

`0x139310..0x139359`, gated on nested `+0x1b0 != 0` and validator `+0x174`
bit 14:

```
00139310  8b 87 8c 00 00 00      mov eax, [edi+0x8c]          ; v8c
00139316  d9 87 70 01 00 00      fld  dword ptr [edi+0x170]   ; v170
0013931c  dc 05 c8 82 20 00      fadd qword ptr [0x2082c8]    ; 1.5 * 2^44
00139322  8b f0 / c1 ee 02 / 23 f2     esi = (v8c >> 2) & 0x20000
00139332  25 00 00 04 00 / 0b f0       esi |= v8c & 0x40000
0013933c  c1 ee 03                     shr esi, 3
0013933f  81 e1 00 00 01 00 / 0b f1    esi |= v174 & 0x10000
00139347  c1 ee 05                     shr esi, 5
0013934a  81 e2 00 f0 ff ff / 0b f2    esi |= old_AF & 0xfffff000
00139352  25 ff 01 00 00 / 0b f0       esi |= bias & 0x1ff
00139359  89 b7 6c 03 00 00            mov [edi+0x36c], esi
```

The two shifts give

```c
AF = (old_AF & 0xfffff000)
   | (round(v170 * 256.0f) & 0x1ff)   /* AF[8:0]  */
   | ((v8c  >> 19 & 1) << 9)          /* AF[9]  */
   | ((v8c  >> 18 & 1) << 10)         /* AF[10] */
   | ((v174 >> 16 & 1) << 11);        /* AF[11] */
```

The magic constant at `0x2082c8` is `26388279066624.0 = 1.5 * 2^44`, so the
double's ulp there is `1/256` and the low dword after the add is
`round(x * 256)`; the reset value `1.0f` (stored at `0x12dfbf`) gives `0x100`
[DERIVED].

**The earlier closure note is wrong by three bit positions** - it places the
three flags at 11/12/13 and says the old AF bits survive.  It missed
`0013933c c1 ee 03`.  Old AF bits 11:0 are all discarded.

### 3.2 What each field is [STATIC, joined across two binaries]

The float and the three flags arrive through the reserved DP2 opcode-5
escape, which carries 8-byte `{id, value}` records to auxiliary-state
dispatcher `0x104ce0` (ids 1..31, target table `0x1052b8`).  The three ids
that matter:

| id | handler | destination |
|---:|---|---|
| 23 | `0x10510c` | value bit0 -> frontend `+0xf4` b14; bit1 -> `+0xf4` b16; bit2 -> `+0x0c` b18; bit3 -> `+0xf4` b15 |
| 24 | `0x1051d8` | value bit0 -> frontend `+0x0c` b19 |
| 25 | `0x1051b9` | value as float -> frontend `+0xf0` |

`MxMGLX.dll` names all three.  Its opcode-5 sender is `0x100a5070`
(`6a 08 / 6a 05` - payload eight bytes, opcode five), and the multisample
flush at `0x1009ee81..0x1009ef3e` emits exactly this triple from the GL
multisample attribute group:

```
1009eede..1009eefd   packed = (ALPHA_TO_ONE<<3)|(SAMPLE_COVERAGE<<2)
                            |(ALPHA_TO_COVERAGE<<1)|MULTISAMPLE_enable
1009ef02  6a 17      push 0x17   ; id 23  <- that bitfield
1009ef14  d9 41 10   fld [ms+0x10]
1009ef20  6a 19      push 0x19   ; id 25  <- GL_SAMPLE_COVERAGE_VALUE, raw f32
1009ef30  0f b6 51 14 movzx edx,[ms+0x14]
1009ef37  6a 18      push 0x18   ; id 24  <- GL_SAMPLE_COVERAGE_INVERT
```

`glSampleCoverage` is `0x100a9e50`, which clamps to `[0,1]`, stores the
invert byte at `+0x14` and the value float at `+0x10` of that object; the
object's constructor (`0x1008d6e0`, and the GL context copy at
`0x100afb66..0x100afbb2`) sets value `1.0f` and invert `GL_FALSE`.

Joining the two tables gives AF in full [DERIVED]:

| AF field | GL state |
|---|---|
| `[8:0]` | `GL_SAMPLE_COVERAGE_VALUE` x 256, 1.8 fixed point; reset 1.0 -> `0x100` |
| `9` | `GL_SAMPLE_COVERAGE_INVERT` |
| `10` | `GL_SAMPLE_COVERAGE` enable |
| `11` | `GL_SAMPLE_ALPHA_TO_COVERAGE` |
| `[31:12]` | not written by the construction |

This closes gate 3: auxiliary state 25 is the **sample coverage value**.  It
is not a sample count, not a coverage inversion and not a sample-pattern
selector - the earlier note was right to refuse all three.

Two GL multisample bits deliberately do **not** reach AF: the master
`GL_MULTISAMPLE` enable (auxiliary 23 bit 0 -> frontend `+0xf4` bit 14) and
`GL_SAMPLE_ALPHA_TO_ONE` (bit 3 -> `+0xf4` bit 15).  They only feed the
compiler's decision to build the pair at all.  **The master enable therefore
reaches the hardware through some other dword** [DERIVED] - see section 4.

### 3.3 What the M9128 did [MEASURED]

Every bit of AF was written on its own beside a valid AD, and `faarect` read
back:

| AF written | result |
|---|---|
| `0x000`, `0x080`, `0x100`, `0x1ff` (coverage 0, 0.5, 1.0, 1.99) | 48000 interior, unchanged |
| `0x200` (invert), `0x300`, `0x400` (coverage enable), `0x480`, `0x500`, `0x600`, `0x700` | 48000 interior, unchanged |
| **`0x800` (alpha to coverage)** | **0 interior, engine stream timeout** |
| `0x1000`, and single bits 13,14,15,16,17,18,19,20,24,28,31 | 48000 interior, unchanged |

The `0x800` result was reproduced twice, each time needing a reboot; after
each reboot the known-good `texture` fixture was rerun and passed before any
further measurement was trusted.

So **exactly one bit of AF is live silicon, and it is the top bit of the
field the vendor constructs.**  That is the strongest single result of this
pass: the unit exists and responds, and what is missing is its enable, not
the AF construction.  Writing alpha-to-coverage while the unit is disabled is
an invalid combination that stops the engine.

## 4. The enable, and the dword this driver does not write

The earlier note attributed a three-bit field at bits 6:4 to AD.  That is a
misattribution [STATIC]: `0x139521..0x139594` writes `[edi+0x1bc]`, which is
nested compiled state `+0x04`, a different hardware dword.  AD is
`[edi+0x368]` = nested `+0x1b0`.  The seed block's
`and dword ptr [edi+0x1bc], 0xffff7f8f` clears bits 6:4 and bit 15 of that
other word every compile, and the branches then set them:

| AA enable A | depth companion `+0x18` | flag B | allocation length | `+0x1bc` bits 6:4 |
|---|---|---|---|---|
| 0 | - | 0 | - | 1 |
| 0 | - | 1 | nonzero | 7 |
| 0 | - | 1 | zero | 3 |
| 1 | nonzero | 0 | - | 7 |
| 1 | nonzero | 1 | nonzero | 5 |
| 1 | nonzero | 1 | zero | 3 |
| 1 | zero | 0 | - | 6 |
| 1 | zero | 1 | nonzero | 4 |
| 1 | zero | 1 | zero | 3 |

`A` is the AA-enable decision at `0x139250..0x1392dd`: set by AAMode bit 0,
by D3D render state 161 `MULTISAMPLEANTIALIAS`, by validator `+0x174` bit 14
(the master `GL_MULTISAMPLE` enable of section 3.2), or by two conditions on
`+0x174` bits 12 and 13; and cleared again when `+0x8c` bit 17 (auxiliary
state 9) is set beside a surface capability bit.

**This is the first field whose value depends on the AA enable and which is
not AD or AF.**  Which command index carries nested `+0x04` is [OPEN]; it is
the single most useful thing left to recover, because the M9128 says the unit
is there and only its enable is missing.  The experiment that would close it:
find the lowerer and emitter of nested `+0x00`/`+0x04` the way `0x1826a5` and
`0xfbe09` were found for `+0x1b0`/`+0x1b4`, then write that index with the
field values above and rerun `faaedge`.

`a1`, `a3` and `a4` are the fragment-pointer registers themselves, and they
never carry an address [STATIC, section 4c].  `MxMD.dll` contains exactly one
packet name string, `RESET_FRAG_PTR` at `0x1f4924`, emitted by `0xfab30` as
`e6 = 3`, `e6 = 3`, then `a1 = 0`, `a3 = 0`, `a4 = 0`; a sibling at `0x764e6`
writes `a1 = 0x28041968`, which is a don't-care filler constant and not an
address (see section 4c).  Index `0xad` shares a static index-group table
entry with `0xa1/0xa3/0xa4` (`0x65514 c7 05 d0 f5 20 00 a1 a3 a4 ad`).  The
emitter is slot 7 of vtable `0x1f48e8`, installed by the constructor at
`0xf7eb0`, and `MxMD.dll` contains no direct call to it [STATIC]; the
M-series Linux driver has the same function at `0x889e50`, reached only from
a dispatch table at `0x121ecf0`.  `0xa0..0xa6` are outside this project's
kernel command whitelist, so testing it needed a kernel change; that change
was made, bounded, and the answer is a clean negative (section 4c).

**The captures give that lead no support at all** [CAPTURED, item 0 of
`tools/m9-faa-capture-evidence.py --stream`]: over 1206 decoded vendor
submissions the *only* index in `a0..a6` ever written is `a5`, eight times,
always zero, and always in the same place - inside the four occlusion-query
captures, paired with `d3` in a single `4545d3a5` packet behind a dependency
marker, which is the query sequence and not this feature.  `a1`, `a3` and
`a4` appear in **no** captured stream.  That is consistent with the vendor's
shipping configuration having multisampling off (section 6), and it means a
whitelist change for those indices rests on the static evidence alone, with
no captured packet to check an emitted one against.

## 4b. The enable is index ef bits 31:29, and it is live [MEASURED]

The lowering of that field is now exact [STATIC].  Lowerer `0x181fa0` shifts
nested `+0x04` left by 25 into surface shadow `+0xb8`:

```
0018233e  c1e219                shl edx, 0x19
00182358  8911                  mov [ecx], edx        ; shadow+0xb8 [31:29] = nested04 [6:4]
00182367  c1e20f                shl edx, 0xf          ; shadow+0xb4 bit 15 = nested04 bit 0
001823ad  8986b4000000          mov [esi+0xb4], eax   ; shadow+0xb4 bit 24 = (nested04 & 6) != 0
```

`0xfae60` folds `+0xb8` into combined state `+0x20` (`0x faef6`/`0xfaefc`) and
`+0xb4` into `+0x28`, and emitter `0xfc0e0` writes them:

```
000fc1ed  c74004f0efd745        mov dword ptr [eax+4], 0x45d7eff0
000fc1f7  payload 1 = combined+0x28      ; index f0
000fc200  payload 2 = combined+0x20      ; index ef
000fc209  payload 3 = combined+0x24      ; index d7
```

`0x45d7eff0` is indices `f0`, `ef`, `d7` with the `0x45` pad - the packet this
driver already emits in both its ranges.  **So the AA mode field is index `ef`
bits 31:29.**

And the gate matters as much as the field [STATIC]:

```
00182637  c1e804 / 83e007       eax = nested04 bits 6:4
0018263d  je 0x18275e                     ; field == 0 -> do not lower ad/af
00182646  je 0x18275e                     ; field == 3 -> do not lower ad/af
001826a5  ad -> shadow+0x28
001826ae  af -> shadow+0x2c
```

The vendor emits the `ad`/`af` pair **only** when the mode field is neither 0
nor 3.  This driver left `ef[31:29]` at zero, so the pair the engine was being
given in sections 2.3 and 3.3 is one the vendor would never have sent - which
is exactly why `ad` was inert and why `af` bit 11 was a protocol violation
rather than a feature.

### What the M9128 does with it [MEASURED]

The field is live, and where it is written matters:

| where | value | result |
|---|---|---|
| preprocessing range | 1 | engine stream timeout |
| raster range | 1, 4 | engine stream timeout |
| raster range | **7** | **renders, and renders differently** |

Writing it in the preprocessing range stops the engine at the very start of
the submission, which is consistent with the field belonging to the surface
shadow and so to the raster range alone; the driver now edits it only there.

With the field written in the raster range as 7 - the value the vendor's own
compiler produces with multisampling **off** and a backed target - the M9128
renders, and drops pixels deterministically:

| probe | multisampling off | `ef[31:29] = 7` |
|---|---|---|
| `faarect`, 240x200 on integer edges | 48000 interior | **42040** interior |
| `faaedge`, the 240x200 diagonal triangle | 24000 interior | **21170** interior |
| `faapos`, `faaposy` staircases | 18 steps at 255, then 0 | identical, unchanged |
| partial levels, either shape | 0 | **0** |

42040 is 240 x 200 x 7/8 within 40 pixels, and the loss scales with area
between the two shapes (5960 and 2830, a ratio of 2.11 for shapes of area
ratio 2).  The staircases are unchanged, so nothing has moved sub-pixel.  It
is therefore a **deterministic drop of roughly one pixel in eight, with no
blending** [MEASURED].

### Which pixels, exactly [MEASURED]

The probe reports the positions of the pixels that were not written.  They
are not scattered:

```
faarect, 240x200 at (8,8), ef[31:29] = 7
  missing 5960 of 48000; row counts 180..240
  distinct row counts    180:60  181:40  240:100
  missing x mod 32  795 0 0 0  795 0 0 0  794 0 0 0  795 0 0 0  695 0 0 0 ...
  missing y mod 32  417 0 417 0  417 0 417 0  358 0 358 0  357 0 358 0 ...
  skipped pixel values  ff000000:5960   (clear ff000000)
```

Every missing pixel has **x a multiple of four and y even**, in absolute
screen coordinates; half the rows are untouched at 240 and the other half
carry 180 or 181.  The same phase appears inside the diagonal triangle - a
120x100 region wholly inside it is missing 1500 of 12000 at exactly the same
offsets - so the pattern is fixed in screen space and is not a property of
the primitive.

The skipped pixels hold the clear value byte for byte, so nothing was written
there: **the engine reserves one lane of every 4x2 pixel block and writes
nothing into it.**  That is what a fragment unit does when it has been
switched on and has nowhere to put fragments.

### Which mode values are live [MEASURED]

| `ef[31:29]` | what the vendor's compiler means by it | M9128 |
|---:|---|---|
| 0 | not an AA state; the vendor refuses to lower ad/af | (this driver's previous behaviour) |
| 1, 2, 3 | AA off, or an unbacked target | renders unchanged, 48000 |
| 4, 5 | AA on, master multisample enable clear | renders unchanged, 48000 |
| **6** | AA on, master enable set, no depth companion | **drops the 4x2 lane** |
| **7** | AA on, master enable set, depth companion | **drops the 4x2 lane** |

The two values the silicon obeys are exactly the two the vendor's compiler
produces when the master `GL_MULTISAMPLE` enable is set (the truth table of
section 4 takes its `B = 0` rows only when validator `+0x174` bit 14 is set),
which is an independent check that the field has been identified correctly.

### What the unit still refuses [MEASURED]

Two combinations stop the engine even with the mode set, and both name the
storage that is missing:

| combination | result |
|---|---|
| mode 6 or 7 with `af` bit 11, alpha to coverage | stream timeout |
| mode 6 with a D24S8 depth/stencil attachment | stream timeout, nothing rendered |
| **mode 7 with a D24S8 depth/stencil attachment** | **stream timeout, nothing rendered** |

The depth refusal is not a property of the mode *value*.  Mode 7 is the value
the vendor's compiler produces when a depth companion exists, and it stops
the engine beside an ordinary depth attachment exactly as mode 6 does
[MEASURED, `faarectd`, one reboot].  So what the unit wants is the companion
surface itself, not a different mode selector.

The depth case is the more telling of the two: the mode value itself is
chosen by whether a *depth companion* surface exists (`[edi+0x18]` in section
4), and the vendor OpenGL driver has a function named
`CreateDepthAA64Buffer` for making one.  Binding an ordinary depth attachment
while the unit is on, with no such companion, stops the engine.  A stopped
engine is not cleared by a module reload; each of these measurements cost a
reboot and was retaken only after `m9-mesa-glsl texture` passed again.

The drop is independent of everything else the pair carries: the same 42040
comes back for coverage values 0, 32, 64, 128, 192, 255 and 256 of 256, with
the invert bit, with the sample-coverage bit, and with `ad` forced to zero
(eleven runs, one sweep, guarded by a known-good fixture at the start).  It
is caused by the mode field **alone**: with `ad` and `af` forced back to
their off forms, `0x20` and `0`, and only `ef[31:29] = 6` written, the same
42040 and the same 5960 skipped pixels come back [MEASURED].  So the mode
field is the enable and the pair is state the enabled unit consumes, not the
other way round.

So `ef[31:29]` is the first field of this feature that the M9128 demonstrably
obeys, and the coverage word still has no effect - which is what one expects
if the unit is enabled but has no storage to put fragments in.  The vendor
allocates that storage separately: `MxMD.dll 0xb69ce..0xb6a19` calls the
allocator `0xac7e0` when the AAMode-bit-0 path has set flag `+0x428` bit 3 and
the surface's sample field `[surface+0x270]` is nonzero, and `0xac7e0` sizes
its main buffer as

```
000ac837  0faf8844020000  imul ecx, dword ptr [eax+0x244]   ; width * height
000ac83e  69c958010000    imul ecx, ecx, 0x158              ; * 344
000ac854  dc0df8551f00    fmul qword ptr [0x1f55f8]         ; * 0.38499999046325684
000ac872  c1e803          shr eax, 3
000ac875  3d00000002      cmp eax, 0x2000000                ; capped at 32 MiB
```

about 16.55 bytes a pixel, plus a second buffer of a sixteenth that size, plus
a surface of the same width and height, plus the resource into which the
`a1/a3/a4` sentinel block is written [STATIC].  Sections 4c and 4d allocate
all of that, bind it where the vendor binds it, and report what the M9128
then did.

## 4c. The store, its four objects and where they are bound

### The allocator, fully decomposed [STATIC]

`0xac7e0` makes **four** objects, through allocation-request structs at
`[ebp-0x3c]` whose fields are `+0x00` flags, `+0x04` pool, `+0x08` format,
`+0x18` width, `+0x1c` height:

| # | result field | flags | format | shape |
|---|---|---|---|---|
| 1 | `[esi+0x08]` main fragment store | `0x8002` | 8 | `0x400` per row, rows from the byte count rounded to 1 KiB |
| 2 | `[esi+0x10]` second buffer | `0x8002` | 8 | the same, on `size >> 4` - or a flat 2 MiB when device flag `[dev+0x1c]` bit 0 is set (`0xac8f2`) |
| 3 | `[esi+0x04]` AA companion surface | `0x1000002` | 5 | the source surface's own width and height |
| 4 | `[esi+0x18]` sentinel resource | `0x8202` | 0 | 5 dwords by 1 |

The second buffer is hung off the first (`0xac974` writes it into
`[main+0x414]`, `0xac968` sets `[main+0x418] = 1`).  Allocation flag bit 24 of
the companion sets `surface+0x234 = 2` and `surface+0x3d0 = 1` through the
capability check at `0x477f0`, which is a sample-count field [DERIVED].
**Correction (2026-10-06):** the linked request-kind9 recursion is conditional
on request-format9 and height1 in `517f0`. This AA request has format5 and
does not take that branch. Its profile is entry49 (offset1c54), class4,
hardware format3d, 64 stored bits. The optional BA/BB resource is its HZ
plane at `+38c`, enabled by `+39c`, rather than the linked request-kind9
resource. See [the bounded companion proof](parhelia-m9-faa-companion-resource-20261006.md).

An earlier attribution used the Linux surviving assert string
`virtual Pnx::RsFragPtrsBuffer* LinuxDevice::CreateFragPtrsBuffer(MUINT32,
Pnx::RsResourceType::Pool)` to name this main output. That attribution is
incorrect. The exact factory and consumer joins identify a separate cached
shader-state helper pool, initially `0x200` backing bytes, published through
selector `6c`. They do not name `ac7e0`'s format8 main output or establish its
GPU record layout. See [the constructor proof](parhelia-m9-selector6c-constructor-static-closeout.md)
and [the retained output audit](parhelia-m9-faa-store-record-audit.md).

### The indices that bind it [STATIC]

Emitter `0xfb920` - the same function that writes the `ad`/`af` pair - builds
the surface packets from a hardware shadow block at `[unit+0xb4]`
(`0x1e97b` fixes it at `devstate+0xc98+0x3768`), which `0xaa750` fills from
the four objects above:

```
000fbbbf  c7 40 04 b8 b9 45 45   header 0x4545b9b8
          reloc kind 6, pair [edi+0x68]   -> b8 = AA companion surface address
          [edi+0x70]                      -> b9 = its descriptor word
000fbc18  c7 40 04 ba bb 45 45   header 0x4545bbba   (kind 7, [edi+0x74])
000fbc74  c7 40 04 be 45 45 45   header 0x454545be   (kind 8, [edi+0x8c])
000fbd8e  c7 40 04 bc bd 45 45   header 0x4545bdbc
000fbd96  6a 09 / 8d 87 80...    reloc kind 9,    pair [edi+0x80] -> bc = store
000fbdc4  6a 0a / 8d 8f 94...    reloc kind 0x0a, pair [edi+0x94] -> bd = second buffer
```

`0xaa986` files the main store under descriptor kind `0x0e` into shadow
`+0x80/+0x84/+0x88`, `0xaa9fa` the second buffer under the same kind into
`+0x94/+0x98`, and `0xaab42` the companion under kind `0x0d` into
`+0x68/+0x6c/+0x70`.  The M-series Linux driver emits both packets with the
same indices and the same relocation kinds (`0x4b09a3` and `0x4b077f`).

So the binding is:

| index | carries |
|---|---|
| `b8` / `b9` | the AA companion surface and its descriptor - **the slot this driver binds the ordinary depth surface in** |
| `ba` / `bb` | the AA companion's optional HZ plane and selector12 descriptor |
| `bc` | the fragment store's address |
| `bd` | the second, sixteenth-size buffer's address |
| `a1`, `a3`, `a4` | the fragment-pointer registers themselves |

That last row is the correction to section 4's second candidate.  `0xfc8f0`
emits three records of header `0x4545421a` whose index-`0x42` payloads are
`0xa1000009`, `0xa3000009` and `0xa4000009` beside a relocated address at
index `0x1a`, at offsets `+8`, `+0xc` and `+0x10` of the 20-byte sentinel
resource - a save and restore block for those three registers [STATIC,
`0xfca1f`, `0xfcaa2`, `0xfcb0c`; the same code is in the Linux driver at
`0x498c16`].  `0x28041968` is the vendor's don't-care filler payload, a date,
used the same way at `0xfcd2c` beside `0x26041981` and `0x07111983`
[STATIC]; **no index in `a0..a6` ever receives an address in any vendor
image.**

### What the M9128 did with all of it [MEASURED]

Everything below was run with the enable set, `ef[31:29] = 6`, the store
allocated at the vendor's own size and the fragment probe reporting how many
of its dwords the engine changed.  Every dword of the store is filled with
its own index before the draw, so a write anywhere in it is identifiable, and
a known-good fixture runs before and after every case.

| what was bound | faarect interior | partial levels | store dwords written |
|---|---:|---:|---:|
| nothing (the enable alone) | 42040 of 48000 | 0 | - |
| `RESET_FRAG_PTR`: `e6 = 3`, `e6 = 3`, `a1 = a3 = a4 = 0` | 42040 | 0 | - |
| the store's address at `a0`, `a1`, `a2`, `a3`, `a4`, `a5`, `a6`, one at a time | 42040 each | 0 | **0 of 318464** each |
| the store at `bc` and the second buffer at `bd` | 42040 | 0 | **0 of 318464** |
| the same, with a companion surface at `b8`/`b9`, every one of the 128 descriptor format codes | 42040 for 120 of them, **48000** for `0x38`..`0x3f` | 0 | **0 of 318464** each |

No case was refused by the kernel and none stopped the engine.

The one thing in that table that is not flat is the last row, and it is worth
stating precisely because it is easy to misread as success.  **Descriptor
format codes `0x38`..`0x3f` at `b8` make the reserved lane come back**: with
the enable set to 6 the rectangle renders all 48000 of its pixels again and
the diagonal triangle 24120.  But `faaedge` still reports **zero** partial
levels, `faapos` and `faaposy` still print the single-sampled staircase (18
steps at 255, then 0), and the store is still untouched, with or without it
bound.  So that format class does not make the unit antialias; it makes the
unit stop reserving, which is what one expects of a surface class the depth
and fragment stage treats as absent.  It is a fact about the lane, not about
multisampling: **what is bound at `b8` decides whether the lane is reserved,
and no value of it has yet made the unit accumulate anything.**  So the two
addresses `0xfbd8e` names are **necessary but not sufficient**: the unit
accepts them, keeps reserving its lane, and accumulates nothing.  What is
still missing is whatever tells it the surfaces are multisampled - the
allocation-flag bit 24 of the companion reaches `surface+0x234`, which the
descriptor builder `0x467e0` does **not** read, so it must leave the driver
through some other word [DERIVED].

A blind search for that word was run and came back empty.  The store's
address was written, one index at a time, at every index outside the kernel
whitelist in the 3D state neighbourhood - `6d`, `6e`, `6f`, `8b`, `bf`, `c0`,
`c7`..`d6`, `d8`..`e2`, `e4`, `e5`, `e7`..`e9`, `ed`, `ee`, `f2`, `f3`, `f7`
- and **not one produced a single write into the store** [MEASURED].  Three
of them stop the engine (`f8`, `f9`, `fa`) and six suppress the draw entirely
without stopping it (`c8`..`cd`); the rest are inert.

### The capture that would close it

One vendor stream captured with multisampling actually enabled.  On Windows
that is two registry values (section 6); on the M-series Linux driver the
`FragSize` option exists but the allocator does not, so Windows is the only
side that can produce it.  In the saved DMA buffer, the header dword
`0x4545bdbc` is followed by the store and second-buffer addresses and the
header `0x4545b9b8` by the companion surface and its descriptor; every other
word of that submission is then the answer to what this note still calls
[OPEN].  Without it the remaining fields are a search over the whole command
space, and the search above shows that a single address at a single index is
not enough to find them.

## 5. What the M-series does not have

These are clean negatives with their search extent, and they matter because
`parhelia-antialiasing.md` documents all of them for the older Parhelia-512
and it would be easy to carry them across.

| Parhelia-512 construct | M-series |
|---|---|
| fragment-buffer size formula `FragSize * max(w*h/786432, sqrt(w*w+h*h)/1280) * 1 MiB` | **absent.** Its three float constants appear only in `MtxPar/MTXPARD.dll` (`0x19ca10`, `0x19ca0c`, `0x19c9e8`); zero occurrences in `MxMD.dll`, `MxMGLX.dll`, `MxMDI.dll`, `MxMM.sys`, `MxMKDI.sys`.  `MxMM.sys` contains no `fsqrt` in 130363 disassembled instructions and imports no float runtime [STATIC] |
| 2 MiB retry step, 6 MiB floor, 8/16/32 MiB clamps | **absent** [STATIC, operand-anchored search over the whole miniport and UMD] |
| `DWGCTL 0x1300 = 0x20a / 0x00a` | **absent.** CAPTURED: index `c0` is written 558 times in the M9 streams, always zero, never `0x20a` or `0x00a`.  On Parhelia-512 these are the Z-unit tile-tracking commands anyway, corrected in `parhelia-antialiasing.md` section 9a.0 |
| `ThresholdAA` at MMIO `0x12f8` holding `0x007fe000` | **does not carry.** CAPTURED: on Parhelia-512 index `be` holds exactly `02800000` (1722) and `007fe000` (801) across 52 section dumps; on the M9 the same index holds a VRAM backing address, 0 of 2633 writes carry either word, and **no M9 register anywhere is written with `0x007fe000`**.  This corrects `parhelia-gigacolor-faa-hardware-status.md` |
| PLNWT `0x1324` handling | **absent** from `MxMM.sys` [STATIC] |
| any FAA string, allocator or command in the miniport | **absent.** Regex over all 468992 bytes of `MxMM.sys`, ASCII and UTF-16, for `FAA|Frag|AA|multisample|sample|coverage|resolve|antialias|AAMode`: zero hits [STATIC] |

`MxMD.dll` has no nonzero `MULTISAMPLEMASK` consumer either.  Frontend
`+0xe8` has two writers (the render-state 162 handler at `0x10363b` and the
initializer at `0x12de96`) and **no readers**; validator `+0x168` has exactly
one consumer in the whole image, the zero-mask test at `0x138be2`.  The
16-bit word at `+0x358 + 2*target` that it clears is the per-target **colour
write mask**, four identical ARGB nibbles built at `0x138b78..0x138bc8`
(`0x4444` red, `0x2222` green, `0x1111` blue, `0x8888` alpha), not an AA
output mask - a second correction to the earlier note.  So gate 5 is closed
as far as this driver goes: arbitrary per-sample coverage does not exist in
it, and inversion is the AF bit of section 3.2.

## 6. The vendor's own switch

`MxMGLX.dll` does advertise multisample pixel formats, conditionally
[STATIC].  `0x10046f20` publishes `WGL_SAMPLE_BUFFERS_ARB` as
`(pf->0x74 != 0) ? 1 : 0` and `WGL_SAMPLES_ARB` as `(pf->0x74 != 0) ? 8 : 0`
(`0x100470f6 8b 4a 74 f7 d9 1b c9 83 e1 08`), and `GL_SAMPLES` is written as
the literal 8 at `0x100041c0`.  `pf->0x74` is set at `0x10043f32` from a
global filled by driver escape `0x80040610` at `0x10048d10`, which `MxMD`
answers at `0xd4180` from two configuration values:

```
g_msEnable = (OGL\Multisample != 0) ? (3D\AAMode & 1) : 0
```

`OGL\Multisample` is a third configuration name the earlier note does not
carry - string at `0x1e74c0`, read at `0xb9f49`, default **0**.  With
`AAMode` defaulting to `0x80000842` (bit 0 clear) the shipping driver reports
zero sample buffers, and turning multisampling on in the vendor stack is a
two-registry-value change.  Neither name appears in any shipped `.inf`,
`.ini`, `.cfg` or `.msi`; they are code defaults only [STATIC, greps over all
20 INFs and both MSIs].

The saved M-series **Linux** driver
(`m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/*/m9x_drv.so`) is
the same codebase - it carries the C++ signature strings, including
`GLboolean WGLMonitor::CreateDepthAA64Buffer(GLhandle, GLContext*, ULONG,
ULONG)` - but its option-name table holds `FragSize` and **neither
`Multisample` nor `AAMode`** (whole-word string search).  So the same switch
is not reachable through `xorg.conf` on this hardware [STATIC].

Three diagnostics the earlier notes lean on are **dead strings**: the
`CreateDepthAA64Buffer failed`, `ARB_MULTISAMPLE not supported for OGL
independent buffer` and gigacolor page-flipping messages have **zero**
4-byte references anywhere in the 9465856 bytes of `MxMGLX.dll`, and so do
the `__FILE__` strings of the two source files they belong to; the retail
build compiled the trace macro out [STATIC].  They establish that the source
had those code paths, not that this binary runs them.

## 7. The occlusion counter

[MEASURED] With multisampling off, the M9 counter counts **pixels exactly**:
fixture `faaocc` reports 48000 for a rectangle whose readback shows 48000
covered pixels, and 24000 for a triangle whose readback shows 24000.

[STATIC] The vendor agrees.  `MxMGLX 0x100b96f0` sums the four counter slots
through `0x10097240` and then, when the multisample-available byte at
`[ctx+0x258]+0x20` is 1, multiplies the sum by `GL_SAMPLES` at
`[ctx+0x258]+0x08`:

```
100b97ff  80 7e 20 01      cmp byte [esi+0x20], 1
100b9805  8b 46 08         mov eax, [esi+8]
100b9808  0f af 01         imul eax, [result]
```

A driver that multiplies a hardware count by the sample count in order to
report samples believes the hardware counts **pixels**.  `MxMD`'s Direct3D
type-9 result applies no such scaling (`0x1199ef`).

### With the unit on [MEASURED]

`faaocc` attaches depth and so cannot run with the unit on.  `faaoccnz` is
the same measurement with no depth attachment, which is the only way to ask
the question at all.  Both arms are deterministic over three runs:

| probe | mode | counter | pixels the readback shows written |
|---|---|---:|---:|
| rectangle 240x200 | off | 48000 | 48000 |
| diagonal triangle | off | 24000 | 24000 |
| rectangle 240x200 | `ef[31:29] = 6` | **42400** | 42040 |
| diagonal triangle | `ef[31:29] = 6` | **21170** | 21210 |

The counter is **not** multiplied by a sample count: it stays within 0.9% of
the pixels the engine actually wrote, and is nowhere near two or four times
either that or the unclipped 48000.  So with the unit enabled but no
fragment store bound, the counter still counts at pixel rate.  This is
consistent with section 7's vendor evidence - the vendor's GL layer
multiplies the hardware count by `GL_SAMPLES` precisely because the hardware
counts pixels.

It does not yet answer what the counter would do with fragments actually
accumulating, because none are: the small residuals (+360 on the rectangle,
-40 on the triangle) are the reserved lane being counted or not at block
edges, and they are the size of a rounding effect, not of a factor.  The
measurement is repeated once the store of section 4b is bound.

## 8. Sample positions

[STATIC, clean negative] No sub-pixel sample-position table exists in any
M-series binary.  Every non-code section of `MxMGLX.dll`, `MxMD.dll`,
`MxMM.sys` and `MxMDI.dll` was read as little-endian f32 looking for runs of
eight or more values in `[-1,1]` on the 1/16 grid: 9378, 324, 322 and 1160
runs respectively, and **none** with three or more distinct grid magnitudes.
A stricter 1/32 pass found one run, a boolean table.  No double-precision
variant.  There is no quality-level table and no pattern selector.

[MEASURED, method validated] The probe that recovers them exists and is known
to work.  `faapos` and `faaposy` sweep an edge across one pixel in
thirty-seconds and print the coverage staircase; on a software renderer with
a four-sample attachment (`M9_GLSL_MS=4`) they print four plateaus whose step
boundaries are that renderer's sample coordinates, `faaedge` reports exactly
three partial levels, and `faaocc` reports a count/pixels ratio of exactly
4.0.  On the M9128 with the pair written and no enable, all three report the
single-sampled answer.  So gate 6 is [OPEN] only because the enable is, and
the instrument for it is built and calibrated.

## 9. Answers to the nine gates

1. **Does the allocation length imply a sample multiplier or a fragment
   permutation?**  **CLOSED, negative, and the real storage is now found.**
   The colour layout constructor multiplies only width, height, depth and
   bits per pixel [STATIC]; no captured descriptor is enlarged [CAPTURED];
   twenty AD values including the true extent render identically [MEASURED].
   Multisample storage is a **separate buffer group** allocated by
   `MxMD.dll 0xac7e0` at about 16.55 bytes a pixel and capped at 32 MiB
   (sections 4b and 4c), not an enlargement of the colour allocation.  The
   `Pnx::RsFragPtrsBuffer` names the separate selector6c state helper pool;
   it does not establish this output buffer's name or record format.
2. **AD bits 6:4.**  **CLOSED, and the field is on hardware.**  They are not
   AD bits at all; they are bits 6:4 of nested compiled state `+0x04`, which
   the lowerer shifts into surface shadow `+0xb8` bits 31:29 and the emitter
   sends as **index `ef` bits 31:29** [STATIC, section 4b].  AD's only
   fallback form is `0x20`.  MEASURED: values 6 and 7 change what the M9128
   renders and 1 to 5 do not.
3. **Reserved opcode 5 / auxiliary ID 25.**  **CLOSED.**  It is
   `GL_SAMPLE_COVERAGE_VALUE`, named from the vendor ICD's own
   `glSampleCoverage` sink [STATIC]; AF`[8:0]` is that value in 1.8 fixed
   point.
4. **The other AF source bits and the exact AF construction.**  **CLOSED.**
   AF`[9]` is coverage invert, AF`[10]` the coverage enable, AF`[11]`
   alpha-to-coverage; all three come from auxiliary states 23 and 24, not
   from any D3D render state (221-handler exhaustive check) [STATIC].  The
   earlier reconstruction was three bits out.  [MEASURED] only bit 11 is
   live on the M9128.
5. **`MULTISAMPLEMASK`.**  **CLOSED for this driver, negative.**  No reader
   of frontend `+0xe8`, one consumer of validator `+0x168` - the zero-mask
   suppression - and the word it clears is the colour write mask [STATIC].
   Arbitrary per-sample coverage is not implemented by the vendor; inversion
   is AF bit 9.
6. **Physical fragment order and sample positions.**  **OPEN, and bounded
   from both sides.**  Hard static negative (section 8); on hardware the unit
   reserves one lane of every 4x2 pixel block at a fixed screen-space phase
   (x a multiple of four, y even) and produces no blending - `faapos` and
   `faaposy` print the single-sampled staircase, 18 steps at 255 then 0, with
   the enable set [MEASURED].  Nothing accumulates, so there is nothing to
   read positions off; the probe that would read them is built and calibrated
   against a four-sample software attachment.
7. **The resolve, and the fence before the destination is reused.**
   **OPEN.**  Nothing is accumulated to resolve.  The store is now allocated
   at the vendor's size and bound at the two indices the vendor's emitter
   names, `bc` and `bd`, with the companion surface at `b8`/`b9`; the engine
   accepts all of it and writes **zero** store dwords (section 4c)
   [MEASURED].  Statically, no call edge from multisample surface state to a
   resolve packet was recovered [STATIC, whole-`.text` header scan of both
   the Windows and the Linux image].
8. **What an occlusion query counts.**  **CLOSED as far as the hardware can
   be asked today, negative.**  With multisampling off the M9 counter counts
   pixels exactly [MEASURED]; the vendor's GL layer multiplies by
   `GL_SAMPLES`, so the vendor believes the same, and the M-series Linux
   driver does it too at `0x6bb034..0x6bb04e` [STATIC, two independent
   images].  With the unit enabled the counter is still at pixel rate -
   42400 against 42040 written pixels, 21170 against 21210 - and is not two
   or four times anything [MEASURED, `faaoccnz`, section 7].
9. **A depth format chosen for the internal 0x10/0x11 GigaColor
   resources.**  **CLOSED, negative, confirmed.**  No such branch exists.
   Depth does participate in multisampling, but as a *presence* test: the
   depth companion at `[edi+0x18]` selects between the 6/7 and 4/5 rows of
   the section 4 table.  It selects no depth *format*.  MEASURED: mode 7,
   the value that means "a depth companion exists", stops the engine beside
   an ordinary depth attachment exactly as mode 6 does, so what the unit
   wants is the companion surface of section 4c and not a mode selector.

## 9b. A driver fault this cost, and the guard against it [MEASURED]

The first run of this pass reproduced none of section 4b: with
`M9X_FAA_EF_MODE=6` the M9128 rendered **nothing at all** - 0 of 48000
interior pixels, 0 of 12000 inside the triangle - and did not stop the
engine, which reads exactly like a unit refusing to draw.

The cause was in this driver.  `m9_context.c` filled the and/or pair of
index `ae` and left those of `ef` and `f0` at zero, so an active job wrote
both of those indices as literal 0 and cleared the cull, depth and
render-format bits the pass had built.  The unit test could not see it
because it hand-filled the three and-masks itself.

The three per-index helpers are now one `m9_faa_edits()` that fills every
edited index at once, and the unit test asks that function rather than
writing masks of its own.  With that fixed, `ef[31:29] = 6` reproduces
section 4b exactly: 42040 of 48000 and 21170 of 24000, 5960 and 1500 pixels
skipped at x a multiple of four and y even.

The lesson generalises past this feature: **a state that edits several
command indices must fill them from one object**, or a later index is
silently written as zero and the hardware is blamed for it.

## 10. What this driver implements

`m9_faa.c`/`m9_faa.h` build both dwords exactly as sections 2 and 3 describe,
dispatched on the chip family, and `m9_draw.c` emits them where the captures
put them - in the preprocessing range, 819 of 819, and in no raster range
[CAPTURED].  `M9X_FAA_RASTER` restates them in the raster range so that can be
tested rather than assumed.  `m9_faa_edits()` fills every index this state
edits in one call, for the reason section 9b gives.

`m9_faa_fragment_bytes()` is the vendor's own size arithmetic in 64 bits:
`min(trunc(w * h * 344 * 0.38499999046325684) >> 3, 32 MiB)`, with the second
buffer a sixteenth of it.  That is 16.555 bytes a pixel, and it is already at
the cap by 1920x1200.

Everything is off by default and **no capability changed**:
`m9_is_format_supported` still refuses every multisampled format and
`pipe_screen` still reports no multisample support.  **The hardware has not
earned the change.**  With the unit enabled it reserves one lane of every 4x2
pixel block and writes nothing into it, `faaedge` reports zero partial levels,
the sub-pixel staircases of `faapos` and `faaposy` are bit-identical to the
single-sampled ones, and the fragment store stays untouched with every
binding the vendor's own emitter names.  A driver that advertised
`GL_SAMPLES = 2` here would lose an eighth of every primitive's pixels and
antialias nothing.  The caps change belongs in the same commit as the
measurement that shows `faaedge` reporting partial levels, and not before.

### The probes, and what each one is for

| variable | what it does |
|---|---|
| `M9X_FAA` | the master switch for this path; nothing below has any effect without it |
| `M9X_FAA_EF_MODE` | the AA mode field, index `ef` bits 31:29, in the raster range.  6 and 7 are the values the silicon obeys |
| `M9X_FAA_AD`, `M9X_FAA_AF` | force raw words, because a sweep that cannot reach a value the formula does not produce cannot test the formula |
| `M9X_FAA_COVERAGE`, `_INVERT`, `_SAMPLE_COVERAGE`, `_ALPHA_TO_COVERAGE` | the `af` fields of section 3 |
| `M9X_FAA_STORE` | bind the fragment store at `bc` and the second buffer at `bd`, as the vendor's emitter does |
| `M9X_FAA_COMPANION=<format>` | replace the depth attachment at `b8`/`b9` with a companion surface of the target's shape, described with that format code |
| `M9X_FAA_BIND_RESET` | emit `RESET_FRAG_PTR` exactly as the vendor does |
| `M9X_FAA_BIND_INDEX`, `M9X_FAA_BIND_DESC` | write the store's address at one arbitrary index, for the search of section 4c |
| `M9X_FAA_STORE_CHECK` | keep the store in the CPU-mapped window, fill every dword with its own index, and report at teardown how many the engine changed |

The kernel side is `faa_probe_indices`, a module parameter listing the command
indices the probe may write.  **It is empty by default**, so the shipping
validator is byte for byte what it was, and no index has earned a permanent
entry in `m9_index_allowed()`: `bc` and `bd` are the two the vendor uses, and
on this hardware they are accepted and inert.  `PARHELIA_M9_RELOC_FRAGMENT` is
the relocation kind a probed index may carry; it needs a read/write VRAM
object and page alignment.

**Settings that stop the engine on this hardware** [MEASURED]:
`M9X_FAA_ALPHA_TO_COVERAGE` with or without a mode; any mode value written
into the preprocessing range; and mode 6 **or 7** beside an ordinary D24S8
depth attachment.  A stopped engine is not cleared by a module reload - the
machine has to be rebooted, the module reloaded with `KO=` named explicitly,
`/dev/dri` checked, and a known-good fixture rerun before any later
measurement can be trusted.

### The next step, stated precisely

Not another sweep: section 4c ran the sweep, over `a0..a6` and over every
unallocated index in the 3D state neighbourhood, and every one of them is
inert or destructive, never productive.  What is needed is **one vendor
submission captured with multisampling actually on**.  Turning it on is two
registry values on the Windows side (section 6); the M-series Linux driver
carries the `FragSize` option but not the allocator, so it cannot produce the
capture.  In that submission, `0x4545bdbc` and `0x4545b9b8` name the two
packets this driver already emits, and every other word of it is the state
this note still calls [OPEN]: what marks a surface as multisampled, what the
resolve is, and where the sample positions come from.

## 2026-10-06 independent M9 emulator milestone

`tools/m9emu/faa-trace.py` loads the actual `MSerX/MxMD.dll` and
`MxMM.sys` in separate PE32 instances; it hashes those images plus
`MxMGLX.dll`. Its declared PCI identity guard accepts only102b:0540. This
is not a captured PCI/PCIR bootstrap and does not emulate M9 silicon. It
imports the generic PE32/Windows host services, never the Parhelia/P690
hardware model, miniport adapter or initialized snapshots. P6902539/MtxPser
and LX identities are rejected rather than treated as M9.

**EXECUTED VENDOR CODE, CONTROLLED HOST ABI:** `MxMD.dll` entry10510
returns1 and registers35 DDI callbacks. `MxMM.sys` DriverEntry55e05 returns0
and calls VideoPortInitialize with actual FindAdapter55ebd and
HwInitialize56304. FindAdapter currently stops honestly at24ba1 requesting
`MmAllocateMappingAddress` through an unimplemented system-routine lookup.
No unsupported callback is forced to succeed. Subsequent adapter hardware,
registry, resource ownership and D3D initialization remain open.

The complete actual surface emitterfb920 executes with a controlled
shadow/command-buffer owner: AD2800 is written four times, followed by
AF100 once, then its dirty bit80 clears. This is dynamic execution of the
existing static emitter contract, not an API-created multisample surface.
The complete reset emitterfab30 additionally produces exactly
`[6,4545e6e6,3,3,45a4a3a1,0,0,0]`, with its actual RESET_FRAG_PTR begin/end
label callbacks observed. The controlled callback sinks only record labels;
they do not supply GPU status or claim a valid reset dispatch. Neither
execution establishes sample allocation, FAA admission, resolve order or
physical retirement. No M9 hardware was accessed.

Run with the established Unicorn environment:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/faa-trace.py \
  --pci-id 102b:0540 --out captures/m9-faa-emulator-20261006/trace.json
python3 tools/m9emu/check-trace.py
```

Retained hashes and actual output are in the M9-named capture directory.
Next: implement the genuine OS reserved-mapping service contract, then
observe FindAdapter's actual M9 PCI/resource requests before attempting
D3D multisample allocation/binding. Missing M9 register or firmware inputs
must remain explicit boundaries; P690 register behavior must not fill them.

The optional `--os-mapping` follow-on models bounded virtual-address reservation
and ownership, independently of any M9 bus address. It resolves the reserved
MDL mapping function names to explicit stop-only endpoints: invoking either
without a real MDL/PFN model stops instead of returning fake mapping success.
With bounded host pool allocation, FindAdapter advances to the concrete
`KeInitializeMutex` import at1066d; no PCI/MMIO operation has been modeled or
executed. `os-mapping-trace.json` retains this separate boundary. The default
baseline capture still stops at the original mapping-routine query, allowing
its proof to be rerun without the optional adapter. This is host-service setup,
not initialized M9 hardware or recovered multisample allocation admission.

### NT synchronization and captured M9148 device-input progress

The independent MSerX harness now implements single-thread recursive mutexes,
notification/synchronization events and bounded mapping-address reservations.
Unsignaled event waits stop rather than fabricate GPU completion. The executable
`tools/m9emu/check-nt-host.py` checks recursion, release ownership, event reset,
zero-timeout and refusal to invent completion.

`--os-mapping --device-inputs` supplies captured M9148 PCI 102b:0540 configuration
from `m9x-cold-init-20260915-v1` and validates the genuine 128 KiB ROM shadow from
`m9x-vendor-fourhead-20260917` (matching PCIR identity, image checksum). These are
separate captures, not a single Windows startup snapshot. Assigned BAR sizes and
legacy VGA access grants model OS resource ownership; no register values or
engine completion are supplied. The miniport reaches actual VideoPortMapMemory
at caller 0x6c9cf, requesting BAR2 0xfebfe000/8192 bytes, address-space flags 4.
This supersedes the earlier routine-import stop, but FindAdapter/HwInitialize
are not yet complete. `synchronization-device-trace.json` retains exact image and
input hashes plus the request. Next required work is an independent M9 register
mapping model with captured read provenance; a P690 register model is unsuitable.
Controlled AD/AF and RESET_FRAG_PTR command producers still pass. This progress
is command generation and bootstrap evidence, not FAA admission or retirement.

The next bounded run supplies only the virtual BAR2 mapping (OS address glue)
and intercepts **every** device read/write before it can consume invented data.
`bar2-first-access-trace.json` records the first genuine access: 32-bit read at
BAR2+0x304, executing MxMM.sys PC 0x2a3c5. No value is supplied and initialization
stops there. This identifies a real M9 device-input boundary, rather than a
missing VideoPortMapMemory implementation. Existing Linux M9148 register
snapshots require explicit cold-state/register provenance before replay; their
values cannot be presumed valid Windows initialization or completion state.

### Captured initial serial control and real bootstrap rejection

Both retained M9148 `pre-mode*/mmio-live.bin` snapshots read BAR2+0x304 as zero.
The harness now supplies that captured initial control and separately models
only the serial READ protocol grounded in MxMM.sys: 0x448ef sends opcode03 and
24-bit address; 0x449ca writes MOSI bit8, 0x44a93/0x44ac1 toggle clock bit4,
0x44960/0x4499c select/deselect bit0, and 0x44a3a samples MISO bit16 MSB-first.
Unknown opcodes and reads beyond the captured ROM stop. The independent
`check-serial-rom.py` proves bit order and bounds. No engine status is modeled.

`captured-bar2-bootstrap-trace.json` records FindAdapter returning status87,
not success. The actual decision chain 0x5d68f has flags2; helper0x589b0 returns1,
0x116c1 returns0, 0x11b73 returns1, then 0x11484 returns16 at0x5d6e6. This causes
rejection; HwInitialize is correctly skipped. No serial ROM transaction has
executed in this run yet. The next concrete investigation is the 0x11484
bootstrap requirement, rather than overriding its result or admission gate.

### Serial identification boundary and control correction

Further source tracing corrects the earlier serial model: MxMM 0x44960 sets
control bit0 to **select** the flash, and 0x4499c clears it to deselect. The
previous opposite polarity supplied no flash data and produced status87; that
rejection is a harness artifact, not an unsupported M9148 revision. At0x303d5
that zero-filled firmware result reached the PInS-version check at0x1157a.
The corrected bit-order test now uses this actual select polarity.

The real FindAdapter path first calls flash identification 0x4472b. It emits
opcode0x15, samples two bytes at0x44766/0x44775 and checks first byte0x1f.
Alternative paths send opcode0xab with three zero bytes (expect0x10), then test
other identities. The harness stops at the first actual opcode0x15 rather than
inventing its response. `serial-identity-boundary-trace.json` retains this
executed boundary. The captured ROM image proves firmware contents, not flash
manufacturer/status responses; `board-eeprom-miscio.txt` is a separate slave
0xa0 EEPROM and cannot supply SPI identity. Required next genuine input is the
M9148 serial-flash identification response, or a retained vendor-process flash
object populated by these calls. No M9 target operation has been attempted.

### Genuine serial identity recovered from retained vendor-X traffic

The previous identity boundary is now crossed without a new hardware operation.
The pinned genuine M9148 `xinit-mmio.jsonl` already records MISO bit16 in
vendor GPIO read-modify-write arguments at rising clock edges. The independent
decoder `tools/m9emu/serial-inputs.py` verifies the trace hash, vendor-image hash,
and phase/bit ordering against **15 separate known ROM reads, 1123 bytes**,
including `PCIR`, PCI identity, the `0x7ffc` PInS pointer and the complete
1024-byte PInS record. Two chained-image probe reads do not match the retained
ROM shadow and are explicitly excluded from that verification. These echoes
are derived physical inputs, not directly logged read return values.

The first captured transaction (seq14) sends `15`, returning `ff ff`; the next
(seq76) sends `ab 00 00 00`, returning **`bf 49`**. Genuine MxMM recognizes
that alternative at `0x44858/0x44861` and builds the serial descriptor with
manufacturer `bf`, granularity1 and size`0x20000`. The two-byte response is
bounded; an unknown opcode or additional uncaptured identity byte still stops.
These inputs are from a historical M9148 boot, not the current nano boot, and
do not establish a current board's flash identity if its model differs.

With optional `--serial-inputs`, the actual FindAdapter executes flash identity
and ROM READs at `0`, `0x7ffc`, `0x7460`, `0x7464`, `0x7462`, `0x7460`.
It passes firmware discovery and next stops at its real **BAR2+0x318 read**
(MxMM PC`0x2a3c5`). `captured-identity-bootstrap-trace.json` retains exact
provenance, transactions and stop. The larger bounded instruction budget only
allows the bit-banged PInS copy to finish; it supplies no GPU completion or
unknown register value. FindAdapter/HwInitialize and a genuine API-created
FAA allocation/draw/resolve remain incomplete.

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/faa-trace.py \
  --pci-id 102b:0540 --os-mapping --device-inputs --serial-inputs \
  --out captures/m9-faa-emulator-20261006/captured-identity-bootstrap-trace.json
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-serial-rom.py
python3 tools/m9emu/check-captured-identity.py
```

Next inspect the genuine `0x318/0x3b0` ownership protocol. Captured baseline
values alone cannot stand in for every subsequent grant/acknowledgement state.

### Entire surface emitter: conditional index24 / E6 / store binding

`tools/m9emu/surface-packets.py` now executes the full genuine `fb920` function
with explicit controlled address/descriptor markers and a relocation sink.
The **shadow is not produced by an initialized API allocation**: this proves
the function's conditional packet order and relocation contract, not FAA
admission or the validity of marker descriptors. Owner-allocation dirty bits
remain zero; no allocator/reference callback is forced to succeed.

With surface dirty mask`0x8f0` and shadow pair`+0x9c/+0xa0` different from
the driver's global null pair`20d51c/20d520`, genuine execution emits:

1. B8/B9 (relocation6), BA/BB (7), BE (8).
2. **index24**, relocation kind`0x2b` from shadow`+0x9c`.
3. **E6 three times**, payloads`0x6dc00000`, `1`, `1`.
4. BC/BD, relocation kinds9/10 from shadow`+0x80/+0x94`.
5. AD four times and AF once.

The precise middle **software capture-envelope** words are (the leading
1/3/2 counts are not GPU command dwords; raw mode is proved below):

```text
00000001 45454524 <controlled address marker>
00000003 45e6e6e6 6dc00000 00000001 00000001
00000002 4545bdbc <controlled store marker> <controlled second marker>
```

When shadow`+0x9c/+0xa0` equals the genuine global null pair, the function
**skips the complete index24 / E6 / BCBD block** and leaves dirty bit`0x40`
set, while emitting the other dirty state. Thus the store pair is gated by
an additional bound object; emitting BC/BD alone does not reproduce this
vendor producer. The clear case retires all controlled dirty bits.

Current native M9 FAA code lacks index24 and this E6 triple; its kernel
validator also rejects these E6 payloads. **Do not broaden the whitelist or
insert a guessed index24 address.** The exact owner/resource producer and
real relocation semantics must be established first. Independent static
tracing locates shadow`+0x9c` producer in binder`aa750` at`aad13`, fetching
a device-owned per-slot resource and invoking its descriptor method`+0x5c`
with kind5; this is not an alias of BC fragment storage or B8 companion.

The conditional outputs and both relocation lists are retained in
`full-surface-packets.json`; `check-surface-packets.py` proves both branches.
No hardware operation or physical GPU completion occurred in these runs.

### Current M9128 ownership capture and bounded host replay

The root hardware owner ran `tools/m9emu/capture-hmc.c` with idle GPU/display
clients and the established target lock on the current **M9128 LP PCIe x16,
102b:0540 rev1, 102b:22c0**, boot`815d4dcc`. The cleanroom driver had already
initialized the board. The captured request changes only miscIO request bit28:

```text
318 saved70000003 -> requested60000003 -> restored70000003
3b0 before0000ffff -> acquired0000ffff -> after0000ffff
```

The helper guards PCI identity and existing ownership, completes all fallible
setup before requesting ownership, blocks asynchronous signals across the
request/read/restore operations, flushes posted writes with reads and prints
only after restoration. It sends no serial data, flash write or erase command.
Its source builds with `-std=c11 -Wall -Wextra -Werror`.

Optional `--hmc-inputs` replays only this captured bit28 request/restore and
grant state; unknown request bits, DDC requests and writes to the grant
register remain explicit stops. This modern cleanroom postinit snapshot is
**distinct from the historical M9148 vendor-cold PCI/ROM/SPI captures** used
by bootstrap; the harness records this mixed-input provenance explicitly.
It supplies no engine completion or GPIO input value.

Genuine MxMM FindAdapter now executes the `0x341bb` ownership acquisition,
clears request28 and observes the captured grant, then stops on its actual
**BAR2+0x1c4c GPIO read**. `captured-hmc-bootstrap-trace.json`, the raw
`m9128-ownership-request.json` and board identity retain the evidence.
`check-hmc-inputs.py` proves the bounded state transitions and refusal to
invent other operations. Full FindAdapter/HwInitialize remain open.

Next bootstrap input is the actual miscIO I2C/board-EEPROM protocol; a whole
live-register snapshot must not stand in for dynamic slave ACK/data. For FAA,
the newly exposed device-owned index24 resource and conditional E6/BCBD
producer remain the highest leverage packet investigation.

### Real index24 resource getter and descriptor producer

`tools/m9emu/resource-address.py` now executes the complete real interface
constructor `a32a0`, real descriptor getter `a6740` (vtable `1f3a90`, slot
`+5c`), descriptor builder `467e0` with its helper `47350`, and surface emitter
`fb920`. Inputs are explicit controlled resource/layout fixtures; this does
**not** initialize the device's pool-6 allocation or establish its lifetime,
contents, domain, size, Windows API ownership or hardware completion.

The low-level resource's `+268` contains a **pointer to a separate interface**,
not an inline interface. `a3870` loads this pointer at `a38b8`. Descriptor-kind
5 branches at `a6776` to descriptor selector **2**, not format 5. The getter
uses layout `[interface+c0]+3c`; `467e0` creates triple `layout+20/+24/+28`
and marks validity bit `400` at `layout+11c`. A second genuine getter call
uses this cache and never reenters the builder.

`47350` at `47370..47383` only admits the layout address `+15c` when layout
bit1 is set. Layout `+144` is separate metadata, **not the mapped address**.
The fixture uses format `78`, dimensions `256 x 1 x 1`, stride256, address
`06000000` and zero layout mode. Actual builder output is
`{06000000, f0000100, 00010001}`. None of those fixture values establishes
the real pool-6 allocation's descriptor format or layout.

The two real getter branches produce distinct relocation contracts:

| Input condition | Getter pair at shadow `+9c/+a0` | Additional state |
| --- | --- | --- |
| `interface+a4 == 0` and `interface+a8 == 0` | `{0, address}` | direct mapped address |
| either field nonzero | `{40000000, &interface+74}` | `+74` backing-owner pointer from resource`+1e4`; `+78` address |

Passing either pair into real `fb920` emits index24 via relocation kind`2b`,
then E6`6dc00000,1,1`, then BC/BD. The controlled relocation sink records the
direct address or follows the explicit indirect pair; it does not assign a
physical address to a real backing owner. Clearing layout bit1 makes the real
getter produce `{0,0}`; `fb920` then omits the entire block and retains dirty40.
`resource-address.json` preserves all three cases. `check-resource-address.py`
checks actual constructor/getter entries, descriptor cache, both pair forms,
exact packet order and the address-admission negative control.

Static pool-6 request correction: `76540` initially writes request`+8=1a` at
`7656e`, but **overwrites it with zero at `765e3` before calling `a3870`**.
The final request format is therefore zero, not `1a`. Pool6 flags are `8002`;
width is `ceil(bytes/4)` and height is `ceil(width/1024)`, with other dimensions
1. `a3870` at `a38e2..a38fa` derives interface`+a4/+a8` from request bits8/9,
both zero for `8002`; this selects the mapped getter branch if that request
successfully reaches a valid low-level resource. Pool-6 allocation and its
initial data are still open and must be captured before native index24 use.

`76540` has no direct `E8` callers: it is normal owner vtable `1f3850` slot
`44`. Initializer `11be20` calls that slot at `11c0be` with pool6 and byte
count `500` (1280), stores the result at owner`+78`, and passes it to `109310`.
That setter binds context`+2514` subshadow`+50` and sets its dirty bit8. This
is ordinary context initialization, not proof of an FAA-only resource.

`tools/m9emu/pool6-request.py` executes the genuine vtable method with those
arguments and an explicit hardware/cache owner, stopping **before** low-level
`a3870`. The actual final request has flags`8002`, format0, width320 at`+18`,
height1 at`+1c` and dimensions1 at`+20/+24`. The pointer at`+4` derives from
hardware`+1015c + 2060`; its selected profile entry is now constructed below,
while the physical address domain remains unproven.
Request`+c/+28` are not initialized by this producer and are omitted from
the reported initialized fields rather than treating host stack zeros as
vendor data. There is no fake successful allocation callback.

A second case uses a separately constructed cached interface at real slot
`hw+fed8+slot*144`. Two genuine method calls perform no allocation and return
interface`+58`, incrementing interface`+5c` from1 to3. This proves the cached
reference increment contract; bounded context release is also executed below,
while final hardware retirement remains unproven. Captured
`pool6-request.json` and `check-pool6-request.py` preserve both cases. Actual
allocation domain, initialized data, final hardware release and the enabled surface
resource graph remain the next requirements.

### Software capture envelopes versus raw GPU commands

The emitter's stream modes must not be conflated. At `fbb9e..fbbb3`, real
`fb920` tests stream`+10 == 1` and stream`+14 != 0`, then writes a count2
before B8/B9. Otherwise `fbbb5` subtracts4 from the cursor before the same
header/payload stores. The latter consumes the reserved count slot and
directly generates raw indexed GPU words. The index24/E6/BCBD and AD/AF
blocks use the same mode branch. The returned cursor is already exactly
after the last payload: **do not subtract4 again when exporting it**.

`surface-packets.py` now independently executes both modes of the actual
emitter; `full-surface-packets.json` keeps old envelope `cases` separately
from raw `gpu_cases`. `resource-address.py` also calls the actual emitter
again in raw mode after each real getter case and records `gpu_words` and
`gpu_relocations`. These words are not produced by Python count stripping.
Both checkers parse raw output with the existing strict
`m9-command-decode.py` indexed-packet grammar. The dirty40 block has exactly
three GPU packets and nine dwords:

```text
45454524 <one 32-bit relocated address>
45e6e6e6 6dc00000 00000001 00000001
4545bdbc <32-bit store address> <32-bit second-buffer address>
```

The first payload is relocation kind`2b`, flags1; the E6 header contains
three index-E6 writes, not a leading GPU count3. Enveloped fixtures are
intermediate software records and must never be submitted directly through
the cleanroom raw command ABI. This establishes the actual emitter's raw
grammar; a complete initialized vendor submit/export lifecycle remains open.

### Genuine relocation resolver and kind2b payload width

The new `resource-address.json` `genuine_relocation_cases` execute the real
command-stream relocation method `25680` and relocation-kind encoder `24870`.
Real stream vtable `1f2c60` slot`54` points to `25680`; the fixtures retain
controlled stream/resource/layout ownership, but replace the controlled sink
with that actual method. Passive instruction/write hooks only record arguments
and vendor payload stores. Direct and indirect getter cases produce the same
nine raw dwords, with actual `2b/9/10` relocation writes.

`25680` resolves the pair's top-three-bit address class: class0 uses the second
word directly; class1 loads resource`+1c4`; class2 loads reference`+4`. Class2
therefore consumes exactly the `{40000000,&interface+74}` pair produced by the
real getter. It separates the base address's low2 bits, sign-extends the pair's
low29-bit offset and adds that offset to the aligned base. Kind`2b` selects
jumptable byte9 at `24993`, reaching `24930`: the encoder restores the low2
bits and writes **one complete 32-bit payload**. There is no kind2b-specific
address truncation. This closes encoding and indirection, not the allocation's
physical domain, contents, lifetime or normal enabled FAA admission.

### Pool6 profile construction and bounded context release

The normal paired module startup table has profile initializer `4ed00` at
`1f25f8`. It allocates `31bc` bytes, calls `4ede0` and publishes the returned
profile collection at hardware`+1015c`. Constructor `4ede0` initializes86
entries, each `94` bytes, by calling actual `4fe00`. Pool6 offset`2060` is
therefore entry56 decimal. The fixture now executes real owner constructor
`751f0` and actual `4fe00` for that selected entry, rather than substituting
profile data. This produces class5, hardware format`78`, bits-per-element32,
metadata pointer`1e6ca0`, flags`+c=0` and sentinel`+2c=ffffffff`. These are
actual selected-entry outputs; the complete collection constructor still
requires configured-owner methods for other entries. Physical allocation and
the profile's memory-domain consequences remain open.

After two actual cached acquisitions, `pool6-request.py` now executes actual
context binder `109310` and complete context destructor `11c190` on two
controlled context owners. Each binder stores the returned subinterface at
context`+2514` subshadow`+50`, sets dirtybit8 and returns1. Destructor
`11c4a9..11c4dd` decrements subinterface`+4` (interface`+5c`), clears owner`+78`
and returns1. The two releases take refs3 to2 to1 while preserving the
device-slot cache. This proves ordinary context reference lifetime; it never
reaches the last-reference virtual teardown or establishes device shutdown,
GPU completion, allocation contents or enabled FAA surface admission.

### Separate actual M9128 PCI and legacy-ROM inputs

`captures/m9128-device-inputs-20261006/` preserves root's sole-owner, idle
read-only capture from the installed M9128 LP PCIe x16, rev1, subsystem
`102b:22c0`, boot`815d4dcc`. The4096-byte PCI config SHA is
`b45f1f8d86bd041d6824d6bf4dea11fbfa9113d3de5a199d64aee32a9ec9c7e0`.
The131072-byte legacy ROM shadow SHA is
`34064c0553954252f0d45fb78150a30c5773ea8205a1bfb8995d9711ff874cf4`.
Sysfs ROM reading failed with EINVAL without changing ROM-enable state;
`/dev/mem` legacy shadow reading succeeded. The shadow has55aa, PCIR at60
withVID/DID102b/0540, declared61440-byte image checksum0, board string
`128-E1024LAF` and serial`KFP35758`. This establishes the shadow's board
identity, not independent dynamic SPI transaction responses.

`faa-trace.py --os-mapping --m9128-inputs` uses this actual PCI/ROM/resource
set through `m9128-inputs.py`, independently of the older M9148 capture.
Actual resources put BAR2 at`fdffe000`, versus historical`febfe000`.
The genuine MSerX miniport FindAdapter reaches that actual BAR2 mapping;
the model now uses the separately captured BAR2`+304=0` baseline to clock
the genuine first SPI header, then deliberately stops at opcode15's uncaptured
identity response. It cannot borrow
M9148 initial MMIO, opcode15/AB identity responses or later completion
state. Current actual idle GPIO values are baselines, not an EEPROM/SPI
protocol trace. `check-m9128-inputs.py` verifies the board hashes, actual
mapping and this precise refusal boundary. The scalar baseline's SHA is
`7361bda8958499b6d0447316723db92767916c44e26f41982dec1dda2601a620`;
it is not a substituted whole MMIO snapshot. Physical SPI identities and
board-EEPROM slave responses remain required to continue actual-board
bootstrap.

The full actual profile publisher `4ed00` was subsequently executed with
the separately initialized real config owner `61f70`, returning1 and
publishing all86 entries at hardware`+1015c` and device`+69a8`. Its earlier
100k-instruction stop at`50af0` was a fixture instruction limit during
mask counting, not a missing capability rejection. The complete constructor
works with a1M-instruction bound. Ordinary card-list registration `27230`
also returns1, links device`+69c0` and publishes global`20d1cc`; the normal
device-init loop at`2e03a..2e056` calls it from paired module table`1f2638`.
These CPU ownership steps do not substitute for initialized physical
capability owners or prove M9128 FAA admission.

### Bounded actual-board SPI identity capture helper

`tools/m9emu/capture-spi.c` is prepared for root's review and sole-owner,
locked idle-target execution; root subsequently ran it successfully, as
documented below. The helper
requires PCI102b0540 rev1 subsystem102b22c0 and inactive CS/clock/MOSI/1000
controls. It changes only BAR2`+304`, issuing read-only flash commands15,
AB000000 (only if command15's first byte is not1f, matching vendor fallback)
and03000000 with an8-byte known-ROM read. The read protocol includes the
genuine1000 control transitions; it is not a simplified three-signal SPI loop.

Actual MSerX miniport `4472b` emits15 at`44757`, reads two bytes, then
conditionally emitsAB/three dummy bytes at`447d6..447f5` and reads two bytes.
Actual ROM producer `448ef` sends03/address24 and reads its requested count.
Their shared helpers are `44960` (clock-low, set1000, CSassert), `44aef`
(clear1000), `449ca` (eight MSB-first MOSI/clock cycles), `44a3a` (eight
clock cycles with MISO16 samples), `44b20` (set1000) and `4499c` (CSrelease).
`spi-protocol.py` independently executes those real functions with a clearly
controlled zero-MISO negative fixture. Its retained `spi-protocol.json` has
196 identity/fallback writes and230 ROM-read writes. The capture helper's
426 emitted words are checked word-for-word against that genuine execution.
These zero response bytes are **not** captured M9128 flash data.

The helper checks a1-second monotonic deadline before every write. All fallible
setup precedes writes, asynchronous signals are blocked during transactions,
and every transaction error reaches unconditional saved-control restoration
before output/unmapping. Input-only MISO bit16 is excluded from the restoration
comparison. `check-capture-spi.py` uses ordinary temporary files, never PCI:
it verifies exact vendor writes, restoration on known-ROM mismatch and forced
deadline failure within each of the three transactions, and no-write refusal
for active controls, wrong identity and already expired deadlines. Actual
target bytes must match the retained board's ROM header before identity
responses are admitted to the bootstrap model.

### Actual shared resource owner creation before VRAM inputs

Ordinary module initializer `19420` publishes device`+6988` from
hardware`+35f8` shared owner`+4`. If that shared owner is missing, it calls
actual `19560` with EAX=device. `shared-resource-owner.py` now executes real
`61f70`, `4ed00`, `27230` then `19560`, using root's separately captured idle
M9128 cleanroom postinit BAR2`+f04=0448803e` baseline (SHA
`f7ed34bdf7855e12d7e68220661209b098cca91ddb84524799f5d9949a511611`).
The captured`+3a8=00010a05` is retained but has not been read by this execution.
This is conditional execution from current cleanroom state, not a capture of
the vendor's cold hardware initialization or initialized capability owners.

Real `19560` allocates/zeros a2c-byte shared owner, publishes it at
hardware`+35f8`, then reads f04 and records fields`+24=2` and`+28=1` from its
actual bit extraction. It writes f04 unchanged, then exact ordered MMIO:
344=0,348=0,34c=0,350=0,354=03020100,334=0,338=40,33c=80,340=c0,330=1000.
Those writes are actual vendor instructions in emulator memory, never target
writes. `check-shared-resource-owner.py` verifies the exact order and values.
At `196e1` execution stops before using unproven platform`+98/+a0` and
hardware`+10` VRAM descriptor inputs. That boundary precedes real25a70/44b90
backing-allocator ownership and its device`+6988` publication. Profile
construction, address encoding and ordinary context refcounts are proved;
physical allocation domain, resource contents and enabled surface admission
remain open.

### Actual M9128 SPI responses now captured and admitted

Root ran the reviewed bounded capture under the established locks with
X/GPU clients stopped. Actual M9128 returned command15=`ffff`, AB=`bf49`
and ROM8=`55aa78eb7b17ba02`, exactly the retained board shadow's first8
bytes. The capture completed426 writes, saved/restored304=0, timeout0,
return0 and no new error on the same boot815d4dcc. Retained
`spi-identity.json` SHA is
`707744e68f7374669c42a33322d488be31591936e4451198810f84cbc4db7014`.
These replies happen to match M9148, but are independently measured on this
installed M9128 and no longer borrowed from another board.

`M9128Inputs` now validates the physical capture against board identity,
restoration and known-ROM8, admits those identity bytes, and reuses the
bounded ownership capture from this same M9128 boot. Real FindAdapter
continues through ROM header/PInS accesses at0,7ffc,7460,7464,7462,7460,
then ownership31870000003→60000003/grant3b0ffff, and stops precisely at
uncaptured GPIO1c4c. Full ROM payloads still come from the actual board's
legacy shadow; physical SPI verification covers its first8 bytes, not every
payload byte. No EEPROM ACK/data or engine completion is supplied.

At this boundary actual miniport owner`+4 +1ec` already points to a genuine
indexed configuration instance. Executing real `26d9d` finds its single
type14/208-byte record. Its initial header holds1/0540 at offsets0/4, but
fields8/1ac/1b0/204 are still zero before GPIO-dependent initialization.
`bootstrap.json` preserves its pointers and exact208 bytes under
`partial_capability_record`. This is an allocated, incompletely initialized
record; zero`+204` must **not** be used as evidence of unsupported M9128 FAA
or fed to normal ICD admission as initialized capability data.

The next trace admits only the **first** idle GPIO1c4c read from actual
`gpio-idle.txt`, returning300db once. This reveals genuine producer
`42e2b..42e4a` requesting1c4c=308d9; execution stops **before** that write.
The baseline is consumed and cannot become a synthetic slave ACK/data
response after any GPIO change. The still-open128-byte board-config read
belongs to normal type13 creation, immediately before real `31f90` initializes
type14 and its `+204` capability mask; native agent traces that consumer.

### Actual M9128 SPI read lengths (2026-10-06)

The passive `SerialROM.read_ranges` log records completed READ03 transactions
without changing the producer or its data: `(address,length)` is
`(0,256), (0x7ffc,2), (0x7460,2), (0x7464,2), (0x7462,2), (0x7460,1024)`.
The genuine bootstrap consumes 1288 bytes over these six reads. Physical SPI
verification currently covers only the first eight bytes; all remaining ROM
payload is the actual board's legacy shadow, explicitly provisional for flash
equality. These bounded ranges, rather than a full ROM dump, are sufficient
for the next physical flash comparison. GPIO capability data is still unsupplied.

`capture-spi.c <M9128 sysfs directory> --pins` now captures exactly those six
ranges after the same genuine identity query. It accepts no arbitrary flash
address and uses only READ03, identity15 and its genuine AB fallback. The fixed
mode is bounded to 21416 GPIO writes and a two-second deadline, with saved304
control restoration before output on every path. `spi-pins-protocol.json` is
controlled zero-MISO actual MxMM448ef execution, **not hardware data**; the
checker compares every C helper write to this independent vendor execution and
checks restoration after failures inside the PInS transactions. Root must
review and run under the existing idle-target locks before admitting payload.

Root's sole-locked physical `--pins` capture subsequently completed all six
reads: 21416 writes, saved304=restored304=0, timeout0, rc0. Every one of the
1288 bytes matches the actual M9128 legacy shadow, including the full 1024-byte
PInS block at7460. `spi-pins-physical.json` SHA256
`08435025305a774318cef7542d8e5fa4c6b223480007b7b108a7877f1a1cae2d`
is now an admitted input. SerialROM rejects any SPI03 byte outside those actual
captured ranges, even within shadow bounds. This verifies consumed PInS data;
the remaining shadow is not asserted equal to flash. Bootstrap still stops
before the first unproven GPIO write; board capabilities remain uninitialized.

### Bounded GPIO capability source capture (2026-10-06)

Passive hooks on the real bootstrap prove the first `30f5f` request is kind2,
addressA0, bus9, subtype6, offset0, read1 byte. Actual context at432e5 uses
1c4c, clock input2/direction800, data input1/direction400, callbacks
42d0f/42d37/42d5f/42d5f, and frequency50000/period10000. Actual44bba integer
conversion yields half/quarter delays9/4us; clock-stretch retries use10us.

`capture-gpio.c` performs only the normal read grammar: START, A0/ACK,
one subaddress byte/ACK, repeated START, A1/ACK, one data byte/master NACK, STOP,
for fixed offsets0..127. Sending a subaddress selects a read; no EEPROM data
write or flash programming command is sent. Every one of its22784 successful
GPIO words equals actual MxMM routine execution with an explicit zero-data/ACK
fixture. Its first-slave-NACK path equals all51 genuine vendor writes, including
STOP. These controlled protocol fixtures are **not physical card data**.

The helper requires M9128rev1/22c0 and idle GPIO lines, acquires captured miscIO
ownership by clearing318bit28, checks3b0grant, and limits activity to three
seconds. It restores1c4c controls and318 before reporting every error; input
pins0/1 are excluded from the GPIO restoration comparison. Checker covers ACK,
NACK, early/middle/final deadline, missing grant and no-write refusal paths.
Only `completed` bytes are usable. If the actual card NACKs its first request,
its untouched zero output array is **not** a128-byte capability blob; preserve
that genuine failure and let the vendor producer take its real fallback.

The separate bounded `GPIOEEPROM` model has now round-tripped synthetic byte
values0..127 through the genuine vendor routines, recording exactly128
A0/offset/A1/STOP transactions. Its first-NACK model executes the actual51-word
error sequence and records no output bytes. Tests are CPU protocol proof only,
not actual board capabilities. The model rejects addresses other than A0/A1,
uncaptured subaddresses, EEPROM data writes and multi-byte/master-ACK reads;
actual physical replies must be separately admitted before bootstrap uses it.

### Genuine context restore and fragment-pointer save packets, 2026-10-06

Root executed actual dirty-state dispatcher `fce50 -> fc8f0`, including real
stream-vtable slot98=`24f20` and genuine relocation methods `25680/24870`.
The fixture controls owner/stream storage and direct address pairs; its sole
callback records resource-reference ABI arguments without admitting resources.
No command generator or relocation encoder is stubbed. Actual physical
allocation, API ownership, contents and GPU completion remain unproved.

The first pair at shadow7c/80 supplies the three pointer-save destinations;
the second at84/88 supplies the context-restore source. Both must be nonnull.
For controlled save addressS and restore addressR, the actual raw output is:

```text
45454524 R
453ce6e6 6dc00000 42000000 000000a1
44444444 30081898 30081898 30081898 30081898
44444444 30081898 30081898 30081898 30081898
45452f2e R+284 R+2c5
4545421a S+8  a1000009
4545421a S+c  a3000009
45e6421a S+10 a4000009 40008000
```

Offsets above are hexadecimal. The second2f payload includes the genuine
kind2f encoder's low control bit; it is not a plainR+2c4 address. Relocation
kinds are2b/2c/2f/30/30/30, all flags1. Actual resource-reference callbacks
are `(0,R,1)`, `(284,R,1)`, `(0,S,0)`. These eight packets/29GPUwords include
context restoration before the previously static pointer-save block. They
are distinct from `fb920`'s index24/E6x3/BCBD binding and `fab30`'s reset.
They do not establish a separate FAA resolve operation.

`context-fragment-state.json` preserves all five actual executions. Strict
command decoding and `check-context-fragment-state.py` verify complete words,
actual encoder writes, absent-first/second-pair omissions, clean dirty-state
omission and execution with identical nonnull pairs. Allcases clear dirtybit8;
samepair does not suppress the command block. The context shadow's resource
producers must still join the genuine API lifecycle before native use. Existing
Linux grammar lacks several of these forms; adding guessed addresses or a
broad E6 whitelist would not implement the required resource/lifetime contract.

### Actual EEPROM consumed; genuine capability owner retained

`M9128Inputs` now admits the physically completed128-byte GPIO EEPROM payload
while retaining helperrc1 and its20 GPIO readback delta as separate evidence.
Actual normal GPIO routines perform129 reads: offset0 probe followed by128
profile bytes. Genuine `31f90 -> 3265f` accepts header825d3 and fills the
real type14 record: +8=464b0034 (chipWORD34), +1ac=1b, +1b0=1, +204=2.
SerialKFP35758 matches the installed board. This establishes the populated
record, not normalFAA admission, complete adapter initialization or rendering.

`m9128_bootstrap.initialize()` retains the real emulator, miniport/profile/
configuration-instance/shared-owner pointers for downstream genuine CPU
constructors. It does not replace failed startup with a fabricated ready
adapter. The actual FindAdapter next stops before GPIO7 control3007b, delta80,
from transmitter-reset routine390e0/391ec. This next output operation is
separate from the already-completed capability EEPROM transaction.

Real `38414` interprets actual captured PInS function17 as data-mask20 and
output-enable8000. Physical saved300fb has8000 clear, so20 is an input pad
readback, rather than a mismatched enabled output latch. Function21 maps
data80/OE20000. Raw helperrc1 remains retained; no broad restore-mask change
and no physical transmitter reset is authorized by this CPU decode itself.
The conditional software continuation, if used, must retain its own scope
and cannot answer uncaptured DDC or transmitter slave responses.

### Conditional display-output continuation after actual capability decode

`output-controls.py` retains the same real miniport graph but separately models
only already-enabled output pads described by actual PInS function20/21. Real
390e0/391ec emits GPIO7 low/high words3007b/300fb, with genuine10ms delays.
These are explicitly **software output-control echoes**, not physical reset
captures or external transmitter replies. The ordinary bootstrap defaults to
the earlier GPIO7 boundary; actual EEPROM/capability evidence stays separate.

The next normal request is kind23/address10/bus13/subtype0, read offset0 byte1;
it first requests DDC ownership318=50000003 (bit29, versus EEPROM bit28).
Execution stops there because the current actual miscIO capture proves only
bit28. No DDC grant, transmitter identity, or success reply is invented. The
native CPU runtime/configuration publishers can use the retained actual owner
without requiring a physical display-reset detour.

### Normal runtime initializer completes under bounded Windows host contracts

`NTHost.MDLHost` now models owned zero-filled OS system-RAM pages, the x86 MDL
header/PFN list, kernel mapping, unmapping and separate pages/descriptor release.
Physical page numbers are deliberately synthetic OS allocation addresses,
distinct from guest virtual addresses; they are never captured GPU/VRAM backing
or native replay addresses. Unsupported allocation/mapping domains and ownership
violations remain explicit boundaries. The API behavior follows Microsoft's
[MDL layout](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_mdl),
[allocation contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatepagesformdl)
and [kernel mapping contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmmaplockedpagesspecifycache).

The actual first MmAllocatePagesForMdl request from247f4/24814 is Low0,\
Highffffffff, Skip0, TotalBytes1000. Genuine568e7 then requests the optional OS
service `\\Device\\MatroxExtioManager` at74f73. An explicitly empty controlled
Windows namespace returns OBJECT_NAME_NOT_FOUND for that name only, with no
fictitious device/file object and no installed-hardware claim. Normal568e7 now
returns1 after8250 actual-catalog PCI reads and1190 actual registry/default
queries. The record is `normal-runtime-host.json`; it does not claim adapter
admission, full display initialization, FAA resource backing or a draw. Native
owner construction can now consume this real CPU runtime graph directly.

### Actual normal shared CPU platform publication (2026-10-06)

**EMULATED, ACTUAL INPUTS:** `m9_shared_platform.initialize()` now executes MD
`84890` through MM `71d8e` / `61e82`, with the retained genuine M9128 miniport
owner and completed normal CPU runtime. The actual 20-byte `233038` response
publishes the exact MM-created shared hardware context, platform and shared
owner. MD `84914`, `84928`, `8493f`, `84942`, `8494b` and `8496e` publish
`device+c`, `device+10`, `device+8`, `hardware+8`, `hardware+4` and the
per-head owner. No host-provided success response or capability flags intervene.

The hardware context is a zeroed `0x145b0` CPU allocation. Its real allocation
chain `61f20 -> 11df2 -> [55cac]` selects actual MM `590e7`, then `2471b` /
`ExAllocatePoolWithTag` in this initialized function table. The generic
VideoPort pool ABI also has bounded CPU allocation/free support, independently
checked; it is not the allocator selected by this captured path.

The real platform already contains `+98=0xd0000000`, `+a0=0x10000000`, from
actual captured PCI BAR0. These describe a 256 MiB aperture, **not** the
installed 1 GiB VRAM. `+78=0x500` retains the explicitly controlled Windows
5.0 bootstrap host input. `hardware+10` is still zero: its normal mapping
publisher remains required before shared GPU allocator initialization.

The helper imports exact MM heap bytes at their original guest addresses
**before** MD publication and refuses a second import. Recopying the whole
heap after MD initialization would overwrite genuine MD-owned writes.
`check-shared-platform.py` passes actual publication, aperture provenance,
CPU allocation ownership and the destructive-replay refusal. The artifact is
`captures/m9128-device-inputs-20261006/shared-platform.json`. This proves CPU
ownership only; it does not admit FAA surfaces or submit GPU work.

### Normal aperture OS mapping and the remaining refresh boundary

**EMULATED, ACTUAL INPUTS:** normal MD `85d50` (initializer table `1f2520`)
issues `230458`, four input bytes and sixteen output bytes. MM `7014b ->
702cf` executes its real mapping policy and selects `0x08000000` bytes on
the controlled Windows `0x500` host. `69c84` calls real GetDeviceBase with
physical `0xd0000000` and flags `0xc`, constructs an aperture MDL, builds
PFNs `0xd0000..0xd7fff`, frees the temporary view and maps a reserved
write-combined alias. The bounded OS host chooses guest VA `0x60000000`;
it is not a captured vendor VA. Every initial aperture byte is unknown, and
guest reads fail until CPU execution has written those bytes. The full PFN
array is allocated; the x86 MDL.Size retains its 16-bit field.

The output buffer contains the mapped address and 128 MiB length **before**
the final adapter refresh `10fe6 -> 25978 -> 36f90`. The latter presently
stops at `344ab` clearing HMC `318` bit30. Consequently the IOCTL has not
returned, MD `hardware+10` remains zero and `platform+a0` remains the
pre-mapping BAR aperture size. `check-aperture-owner.py` proves this boundary
and checks actual PFNs, unknown-content refusal and CPU-written readback.
A populated response buffer is not successful normal publication.

**STATIC:** `344ab` requests/releases DDC ownership via `341bb/342cb`.
`341bb:34219` clears `318` bit29; `34251` checks `(3b0 & 2) == 2`;
`342cb:34307` sets bit29 on release. `capture-hmc.c --ddc` performs only
this bounded request/grant/readback/restore, with no GPIO/data/transmitter
writes, no force-grant or polling. The original default bit28 path remains.
`check-capture-hmc.py` validates both masks/grant predicates and restoration
on failures against CPU files. Actual DDC capture is pending parent owner.

### Actual idle ownership and conditional aperture refresh (2026-10-06)

**PHYSICAL:** the same M9128 boot's bounded bit29 request saved/restored
`318=70000003`, requested `50000003`, and observed `3b0=ffff` before,
during and after acquisition. The canonical `ddc-hmc-physical.json` has
SHA256 `f06fe16cd0d29b5a3b88e665238681d90acc361bbf876ea24d7b9505ab7f7059`.
The read-only adapter, PCI-refresh and clock-control snapshots accompany it;
their exact hashes are enforced by `m9_aperture_owner.initialize()`.

**EMULATED, CONDITIONAL:** optional refresh inputs explicitly combine the
retained partial-cold CPU owners with cleanroom postinit hardware observations.
The helper records this hybrid scope, retains the previous HMC state, and
admits only the proven bit29 grant plus actual344ab bit30 control writes.
It does not claim uninterrupted vendor FindAdapter completion. Actual
`1c10=2` and `1fdc=04010000` take the genuine reset-skip branch. Actual2e7a2
then saves/restores the captured PCI DWORDs, e4c/e50 and358. The CPU PCI host
accepts only bytes identical to captured PCI configuration; no physical write
is performed.

Actual3471b and2ea11 then emit finite clock-control transforms. Saved registers
are seeded from actual idle captures, while write-only constants initially
remain unreadable until genuine guest code writes them. Actual2eb3c emits
8fc=0,8f8=032f7f1f,8dc=0,3a8=00020a05. These are software control echoes,
never fabricated PLL lock, engine idle or draw completion. The refreshed
checker reaches actual2ebd1's unknown read1800 and refuses it. It verifies the
real BAR0 PFNs, unknown-content refusal and CPU-written readback throughout.
The artifact is `aperture-refresh-boundary.json`; normal hardware+10 is still
unpublished.

**STATIC:** the unconditional final702cf call10fe6 reaches more than mapping
bookkeeping: after saved display controls, actual2ebd1 invokes41892,4191e and
41c61, including DRAM programming/readback. A populated16-byte response
buffer is consequently still insufficient to claim normal mapper completion.
Unknown post-programming hardware replies remain explicit boundaries.

### Conditional shared allocator now returns with real CPU resource owners

**EMULATED, CONDITIONAL:** `m9_allocator_fixture.initialize()` supplies the
real702cf pre-refresh OS-owned aperture VA/length as explicit CPU caller inputs,
while preserving the incomplete normal85d50 result. It executes the genuine
primary-device link prefix2dfa9..2dfd2 (controlled EBX=device,EDI=0), then
ordinary865c0/cfb40 shared-lock construction. Full2dea0 display-mode construction
is not claimed. Genuine19420→19560→25a70→44b90 now **returns1** and itself
publishes device6984/6988/698c. No resource outputs or completion replies are
inserted by the host.

The missing platform counter has a genuine OS producer. Optional
`bootstrap(common_buffers=True)` provides a bounded Windows2000
[VideoPortGetCommonBuffer](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/video/nf-video-videoportgetcommonbuffer)
endpoint through OS config+60. ActualFindAdapter560b1/560b6 imports that callback;
real58c39 lookup and5a97c select5aa1d. MM5d2e1→6d628 requests159 system-RAM
bytes, real45350 clears128 bytes, then1cc7a initializes four CPU synchronization
counters. The counter OS physical address02007000 (after seven earlier common-buffer
allocations in this fixture) is deliberately **synthetic system
RAM**, never a captured native GPU address. Unknown common-buffer bytes and
access after release remain guarded. An exact unchanged captured PCI command
DWORD is echoed in the CPU host; no physical PCI write occurs.

`m9_shared_allocator.initialize()` uses only actual M9128 f04/3a8/f0c baseline
reads (f0c00200040 captured separately). Genuine initializer writes are retained
with PCs and full order; FIFO writes never supply read/completion replies.
The first1000000-instruction cutoff at5bc29 was a **CPU zeroing-loop budget**
boundary, not a GPU poll: real5bc20 writes0, advances the pointer, and compares
its count. A larger bounded execution budget completes those genuine CPU
writes. Sparse page data/validity stores their provenance without a Python
dictionary entry for every VRAM byte.

`check-shared-allocator.py` verifies returned19420, the real1f30e0 allocator
vtable/publications, actual CPU-zeroed aperture bytes, genuine counter bytes
and refusal of still-unknown VRAM. Captured init includes ordered132c/1330/1334
and1320/1324/1328 resource bindings. The artifact is
`shared-allocator-conditional.json`. This is usable conditional CPU ownership
for tracing AA allocation/map/contents; it remains **no normal adapter admission,
no vendor GPU completion and no native FAA rendering result**.

### Raw layout size, aligned descriptor extent, and first real HZ clear

`tools/m9emu/check-faa-allocation-extents.py` executes actual `ac7e0` with a
controlled 320x240 CPU caller surface on the genuinely returned conditional
`19420` resource allocators. A guest memory-write hook records the descriptor
extent before any backing allocation. The four genuine `45eb0` calls yield:

| Resource | Raw EAX return | Descriptor extent | Descriptor alignment |
| --- | --- | --- | --- |
| Main FAA store | `136020` | `136080` | `80` |
| Auxiliary store | `13020` | `13080` | `80` |
| Kind9 companion | `96020` | `97000` | `1000` |
| Initial HZ | `2d00` | `3000` | `1000` |

The raw EAX return is **not** the stored descriptor extent. Real `45eb0` takes
alignment from descriptor `+174` at `4648b`; `464ca..464d3` align raw EAX upward,
and `464d9` writes that aligned value to descriptor `+180`. The memory hook
observes every write at that instruction before `455c0` backing allocation;
the descriptor allocated bit2 is still clear at the `51bd7` return site.
Thus these extent differences are not evidence of `455c0` resizing the raw
layout. Genuine backing attachment `4dfe0` publishes backing pointer `+1e4`,
sets descriptor bit2 at `4dff7`, and copies its backing address at `4dffa`.
The earlier raw protection term remains valid; a native allocation must cover
its aligned extent as well. Native BO page rounding already covers the listed
linear cases.

With `--advance-clear`, the same real `ac7e0` frame continues through `b4fe0`
and `b5030`, including actual descriptor-kind8 computation, and reaches
`b5237`. That instruction would write the clear value to command shadow
`[context+b8]+3a8`, where context is actual `device+69a4` owner plus `b08`.
Its command-shadow owner is still null: `1cb90` constructs and clears the
context through `1de50`, but the later renderer binding is absent. This is
an explicit CPU ownership boundary before job submission, not a GPU status
poll or hardware rejection. No replacement shadow, successful clear,
completion value, or FAA rendering result is supplied.

Capture: `captures/m9128-device-inputs-20261006/faa-allocation-extents-clear-boundary.json`.
The replay uses `--runtime-tools tools/faaemu` in the canonical repository;
the isolated worktree used the native-agent helper directory explicitly.
Normal display admission and full `85d50` mapper completion remain unproved.

### Genuine queue/resource context publishers and HZ clear shadow

`m9_queue_context.initialize(e,state)` now executes real ordinary module
`1a260` followed by resource enable module `1ff90`; both return1. The former
executes genuine `22e30` and publishes queue owner at hardware `+3600`.
The latter calls `1e490`; real `1e670` binds clear context `+b4` to owning
resource `+1160`, and `1e67c` binds clear context `+b8` to resource `+1220`.
These are genuine ownership writes, replacing the previously absent module
lifecycle, without inserting shadow pointers into guest state.

`check-faa-allocation-extents.py --initialize-context --advance-clear` now
continues the original `ac7e0` frame through `b4fe0`/`b5030` to `b54ee`.
The actual HZ-clear CPU shadow, resource reference, clear value and rectangle
are retained in `faa-clear-owned-context.json`. The hook stops immediately
before genuine queue job dispatch. Queue `+30` dispatcher remains null;
no activation return, GPU clear result or completion is provided. This is
actual command-state preparation, not an emitted GPU packet or rendered HZ.

Independent exploration of genuine queue activation `230f0` returned0:
`23cb0` successfully allocates its first real command buffer through `5c160`,
then the second allocation returns0. Its actual candidate request is
pool1 / 2MiB, then pool0 / raw `3a6300` bytes. The activation failure is
retained as a CPU allocator/lifecycle boundary; it must not be overwritten
with a successful reply or described as a GPU completion issue. Its exact
heap/configuration dependency and the later dispatcher/currentjob publisher
remain open. No full normal display admission is implied by the independently
executed ordinary module CPU paths.

### Queue activation is a mapped-class policy boundary

`check-queue-activation.py` retains the actual `230f0` CPU activation result,
all `5c160` command-buffer candidate returns and the genuine `39450` class
checks. The first pool1 candidate has a successfully allocated backing node,
but `455c0` calls mapping predicate `45df0` under real configured policy
`101f8`, then `39450` rejects class1. At `45718`, the resource backing is
released and `455c0` returns0. `39450` accepts only classes0,5,6. The first
fallback candidate uses class5 and succeeds; a second fallback lacks space
in the bounded 4MiB mapped heap. No GPU completion is involved.

The real shipping `System.BmBuffers` DWORD input `01000100` is also admitted
through `61f70`/the actual registry descriptor query. Real typed wrapper
`61ba0` decodes bufferKiB as low12bits (zero becomes8), totalMiB as high8bits
(zero becomes1), and count as `(totalMiB<<10)/bufferKiB`. This input therefore
produces four 256KiB buffers; it still hits the same class1 policy rejection.
Reducing buffer size is not a solution to that mapping predicate.

Native agent identified the actual raw `+120` property backing derived
configuration `+338` as shipping registry option `System.MmFlags`. Alternate
policy must pass through this real option parser and retain genuine format
admission results; do not clear a guest capability/policy field by hand.
Capture: `queue-activation-policy.json`. No failed activation is overridden
or resumed into a GPU job in this checker.

`m9_allocator_fixture.initialize(...,registry_settings=None)` now optionally
passes explicit CPU registry inputs to the native agent's normal configuration
helper. Default callers are unchanged. Nondefault calls require helper API
commit `b5c839ca` (or its canonical integration), which exposes
`m9_md_runtime.initialize(...,registry_settings=None)`.

### 2026-10-06: ordinary miniport owner closes queue activation

The previous queue failure was an incomplete CPU owner/OS contract, not evidence
that the vendor's ordinary queue cannot activate. Genuine module `58bb0` (normal
initialization table `1f2508`) publishes `hardware+fe84[head]` as the address of
`hardware+fe94[head]`, whose DWORD is the PDEV's opaque OS adapter handle. The
normal caller prefix `2dedc..2deeb` copies its handle argument into `device+20`.
The new fixture executes that bounded prefix with a controlled OS handle and
runs the actual module; it does not claim full `2dea0`/display admission.

The actual PCI/DMA query `734a5 -> 6b184` publishes platform`+184=350207` before
MD allocator construction. Inputs are the captured PCI catalog and read-only
resource snapshot. The host bridge's BAR0 sizing reply of zero is explicitly a
**model inference** from its zero captured BAR0 and zero resource descriptor;
no physical BAR probe was run. Two host BAR0 writes and the GPU's PCI`+88` bit10
write happen only in the controlled CPU PCI shadow. There are no GPU replies.

With the real, FAA-admitting `System.MmFlags=101b8` option, queue candidates are
pools1/2/0. Pool1 is disabled by that policy. Pool2 now follows
`a14f0 -> a1c40 -> a1450 -> a11e0 -> 19b20 -> 58c90`, which finds the newly
published I/O owner. Real MD `232284` requests (24-byte input/output) execute
MM `7165f -> 734e6` and actual vendor allocation code. The Windows
`MmAllocateContiguousMemory` host supplies owned synthetic system RAM, with
separate lifetime and physical translation; these addresses are not native GPU
addresses or physical-capture claims. Thirty-two genuine allocation replies
succeed. Native helper commit `4efc3dd9` adds the exact CPU debug-allocation ABI
at allocator vtable`+10` (size/file/line), needed by `f829e`.

**Actual `230f0 -> f8120` returns1**, with a live TaskMgr at queue`+30`.
`check-queue-system-dma.py --mm-flags 0x101b8` independently verifies every
reply against its actual OS allocation owner. The retained capture is
`captures/m9128-device-inputs-20261006/queue-system-dma.json`.

Correction to the earlier terminal class1-rejection interpretation: although
`39450` returns0 for class1, `455c0` has an explicit alternative at `4588b`.
With the real miniport I/O owner, default `101f8` reaches
`458a7 -> 19d60 -> IOCTL23306c` (8-byte input/12-byte output). That OS mapping
contract is still a separate boundary. A zero I/O-owner slot had prevented it
before. Do not describe the class1 predicate alone as a permanent rejection,
force its return, or replace the vendor's mapping replies.

This closes genuine CPU queue activation on the conditional allocator fixture.
It does not establish normal hardware/display initialization, executed DMA,
FAA fragments, resolve completion or retirement. The next required proof is
one complete FAA frame through this returned queue, stopping at the first
unknown GPU reply.

### 2026-10-06: returned FAA initialization and correct live shader ownership

Genuine queue activation alone was insufficient for the clear fixture: the
fixture had called resource-enable module `1ff90 -> 1e490` before activating
`230f0`. At `1e526..1e52f`, resource-enable reads queue`+30` (TaskMgr) and passes
it to actual `f5d60`. That initializer writes shader`+f0=TaskMgr+0xc` at
`f5dda`, and shader`+ec=0xaa` at `f5df9`. With a null TaskMgr, `f5730` later
copies its instruction stream from address `0xc` and fails at `f5969`. This
was a fixture initialization-order defect, not a shader/GPU capability result.
The genuine sequence `1a260 -> 230f0(return1) -> 1ff90` creates the live source
without changing a shader field or overriding a return value.

The next CPU parent was the normal PDEV timeline alias `device+58`.
`b5509..b550f` reads this parent after actual job compilation. Its real normal
PDEV publisher is `2e02c..2e035`: load `device+c`, add `30`, store to
`device+58`. The fixture executes that bounded instruction prefix against the
already genuine MM-created CPU counter page. It does not execute or claim the
remaining normal display/PDEV initialization. The following `f19f0` is not
needed by this alias and is not executed as part of the prefix.

`m9_queue_context.initialize(e,state,activate=True)` now follows real queue
creation, successful real activation, the genuine timeline alias publisher,
and resource-enable. The default `activate=False` remains a preactivation CPU
publisher probe; its shader source is unsuitable for actual compilation until
resource-enable has run with the live TaskMgr. Neither mode supplies a GPU
completion.

With those genuine CPU owners, **full `ac7e0` returns1**. The returned owned
graph includes the main and auxiliary fragment stores, companion with linked
HZ resource, and the real sentinel wrapper. All three initialization clear
paths execute, including `b54ee -> f85c0 -> fadf0`, with `fc0e0` before `fb920`.
The retained run has 900 actual CPU writes to the genuine OS-owned system-DMA
buffers. The final sentinel initialization is the genuine full `ac7e0` tail,
not an independent prefix: words `a4a3a1a1,28041968,0,0,0`.

Reusable capture: `m9_faa_initialized.capture(e,state,bootstrap,...)`; checker
`check-faa-initialized.py --runtime-tools tools/faaemu --pci-inventory
captures/m9128-device-inputs-20261006/pci-inventory/inventory.json`.
Capture: `captures/m9128-device-inputs-20261006/faa-initialized.json`.
The surface dimensions are explicit controlled CPU caller inputs unless the
caller supplies an already genuine surface. The command spans export only
bytes actually written by the vendor CPU, excluding unspecified OS RAM bytes.
The existing MM-created buffer import establishes the MD CPU allocation view;
it does not assert physical GPU coherence or subsequent MM readback of MD
writes. No executed GPU work, rendered FAA fragments, retirement, or complete
normal hardware/display admission is established by an `ac7e0` CPU return.
The complete minimal draw/output/retirement trace remains the next gate.

The serial-bounded FindAdapter elapsed allowance is now 30 seconds instead of
10, retaining its 10-million-instruction bound. Concurrent emulator replays
could otherwise expire before reaching the genuine EEPROM branch. This changes
host scheduling tolerance only; actual device absence/failure boundaries and
captured physical input guards remain intact.

The returned initialization capture additionally joins actual CPU DMA bytes to
genuine submission writes. `24d60` calls `24530`; `24601` computes the current
buffer's physical end from its real translation and CPU cursor, and `2461e`
writes it to BAR2`+404`. Direct clear tails `b55af` and `b62c0` also publish
that end. The initialization run posts `01002174`, `010021b4`, and
`01002238` in its synthetic OS physical namespace. All are inside its genuine
first contiguous allocation (`physical=01001000`, extent `41000`); the CPU-written
stream starts at offset `428` and ends at `1238`. The retained bytes are actual
indexed DMA words, not the earlier software count envelopes: in this live queue
mode the producers take their branch that omits those envelope DWORDs.

The allocator's strict MMIO read hook remains installed after `19420`. It
records genuine writes, permits only its three captured configuration-register
reads, and refuses all unknown reads. All retained MMIO events during these
three initialization clears are writes. A submitted end pointer does not
create a completion reply. The CPU return proves command construction/posting
against the MMIO model, not hardware execution. No subsequent MM IOCTL reads
of MD-written command RAM occurred in this run; the request count remains the
32 genuine allocation calls.

### 2026-10-06: joined frame reaches the genuine first retirement read

The native agent's reusable `m9_faa_frame.initialize(...)` joins the real public
D3D context, full `ac7e0` allocation/initialization, actual `aa750` binding,
public `beeb0` DP2 triangle, and returned `abc20` output. CPU callback success
alone does not prove GPU rendering. The new retirement helper consumes the same
returned emulator/owned graph; it does not rebuild an unrelated shader shadow.

Real final queue callee `23b80` has normal direct callers `10919` and `1a5e8`.
With the genuinely returned queue's final reference `+94=1`, it calls the real
queued-completion producer `25370` and flush `24d60 -> 24530`. In the retained
320x240 frame the final completion packet is exactly the three DMA words
`4545e3e6,00000001,00000131`; the desired completion count is `130` after low
four-bit masking. Actual `2461e` posts final physical end `01002780` to BAR2
`+404`. The genuine first system-DMA owner is physical `01001000`, extent
`41000`, so the posted end is offset `1780`. Its final CPU-written packet
covers offset `1774..177f`.

The helper stops **before** instruction `23bc3` (`8b09`, `mov ecx,[ecx]`).
The preceding real instruction loads the status pointer from platform
counter-owner`+4`. This pointer is `20464000`, owned by the genuine live
159-byte common buffer, at synthetic physical address `02007000`. The required
next input is an actual GPU-written completion counter. No status value,
retirement return, resource release, or successful GPU response is supplied.
The whole normal PDEV/display caller remains outside this conditional CPU
fixture; the real final queue callee and its actual operands are executed.

`m9_faa_retirement.capture(frame)` captures the final CPU-written DMA spans,
actual MMIO posts, their unique owned-buffer translations and first status-load
boundary. `check-faa-retirement.py` verifies the joined context/init/bind/draw/
resolve stages, live final queue reference, posted buffer bounds, and refusal
to execute the GPU status load. Capture:
`captures/m9128-device-inputs-20261006/faa-frame-retirement.json`. Its runtime
helper dependencies include the native agent's actual tagged CPU-allocation
ABI/normal D3D policy (`19c5699d`) and joined frame/public frontend API.

This provides a complete CPU command-production/submission sequence and an
honest hardware-response boundary. Executable native translation must preserve
all referenced resource/program backing, address domains and lifetime through
observed retirement. It must not copy synthetic OS addresses or unspecified RAM contents into native
packets, and must not invent the blocked completion result. Counted secondary
images require their genuine typed pointer/export contract, described below.

The refreshed retirement capture uses native joined-frame API `b954cf25`, whose
passive hooks start before queue initialization. All six stages are required:
queue/context/FAA initialization, FAA bind, triangle and resolve. The first
buffer's prefix `0..427` now comes from actual queue-initialization CPU writes,
so it is not synthesized as zero-filled RAM. `m9_faa_operands.capture` supplies
read-only ownership candidates for the 86 genuine profile entries (`4ee1f`)
and all actual `25680` operands. There are 39 allocated resource views in the
retained graph; aliases and namespace overlaps remain explicit. Nonzero
operands all match a real resource/system-RAM/common-buffer owner; seven match
both a numeric VRAM-offset range and the synthetic OS physical range. No domain
is guessed from that numeric overlap. The genuine low two address bits, CPU
reference tags, register-specific payloads and retained backing objects remain
available for typed native translation.

This ownership manifest does not make the counted secondary draw image a raw
indexed command stream, nor establish that ordinary DP2 state correctly retains
FAA companion/depth/MRT participation. Those are separate execution gates.


### Counted secondary image is exported unchanged (2026-10-06)

Passive observers execute the genuine nested queue pop `MxMD!24b10` and its
pointer exporter `24c73..24d45` during the joined CPU frame. The triangle image
has queue depth1/kind1, 760bytes (190DWORDs). Forty-three count records consume
187DWORDs using stride `count+2`: the count word precedes `count+1` following
words, which can contain more than one indexed packet. The final three words
are the actual primitive `[23000001,0,4]`.

The primary stream is restored to depth0/kind0. Its genuine exporter writes
`[45452d2c,01042002,010422fa]`: actual buffer translation plus CPU start/end,
start address selector2, and end mode2 from the saved secondary kind. Hashes
before and after pop/export are identical:
`318d65e10d7c9f667646bc1e550b2d3676f69c353a2248507c31a6d67f5d4ede`.
There is no CPU strip, conversion or copy between these two observations; the
primary pointer references the same counted image. Thus blanket removal of
count words or insertion into the raw indexed ABI would discard the actual
vendor submission contract. A private replay must preserve the bounded typed
secondary image and its pointer/mode semantics. Hardware interpretation and
correct FAA draw target participation still require proof; neither is claimed
from this CPU trace.

Reproduce with `tools/m9emu/check-counted-submit.py --runtime-tools tools/faaemu`
and actual PCI inventory/resources. The passive helper is
`tools/m9emu/m9_counted_submit.py`; its retained output is
`captures/m9128-device-inputs-20261006/counted-secondary-submit.json`.
Runtime dependencies are the same genuine full-frame helper `b954cf25` and
ordinary option/allocator ABI `19c5699d`; no target access or GPU reply is added.


### Sentinel CPU alias and actual page-copy fallback (2026-10-06)

The sentinel's raw resource layout `+15c` is GPU address `1b49000`, published
from backing `+20` by `4dfe0`. Backing `+10` is a different allocator-relative
address (`cc8000`), and the actual public CPU map returns `60eb0000`. The
existing BAR0 view is `60000000`, making its CPU aperture offset `eb0000`.
Looking up initialized bytes at GPU offset `1b49000` therefore gives a false
negative: the CPU writes are retained at their actual aperture alias.

The genuine mapping chain is wrapper `+38` virtual slot14 (`a66c0`) through
wrapper `+48` slot1c (`a8d60`) to `55850`, `2cb40` and `27170`. The sentinel has
no staging-resource branch (`174/2a4/2b8/3d0/3d4/3e4` are zero). Its actual map
owner invokes `26830 -> 26a7a -> 1cab0 -> 597a0 -> 59410`. Source index is
`1b49`, destination alias index `eb0`, count1. The CPU-written source PTE
`1b49008` at `60407d24` is copied to `60904ac0`; the destination previously held
`eb0008`. Crucially, actual `1c120` returns0 and the real CPU fallback loop
`594a0` publishes `1b49008`. This is CPU page-table publication, not a supplied
GPU copy result or completion. The physical hardware's aperture interpretation
is not emulated; the exact real PTE, returned map and owning sentinel relation
are retained together.

Twenty bytes at that returned CPU alias are genuinely initialized as
`[a4a3a1a1,28041968,0,0,0]`. The remaining108bytes of its128byte extent remain
unknown. Pool6 initialization is a separate contract; this result gives no
permission to fill its unspecified bytes with presumed vendor state.
`tools/m9emu/m9_page_alias.py` observes the real producer and verifies the
source PTE and initializer against sparse CPU-write validity. It neither
remaps a resource after posted work nor supplies a device status reply.
`check-page-alias.py` reproduces the proof; output is
`captures/m9128-device-inputs-20261006/sentinel-page-alias.json`.


### Normal FAA pool preparation, ownership and retirement boundary (2026-10-06)

The current proof uses the normal surface-owned frame, not the earlier direct
FAA binding fixture. Genuine `a77c0` activation, actual resource association,
normal per-head target owner `20810` and public DP2 target state produce the
scratch quad, coherent FAA triangle and `abc20` output/resolve. The target owner
uses the retained M9128 DC snapshot. No GPU completion or successful retirement
reply is provided. The passive observer is `tools/m9emu/m9_pool_lifecycle.py`;
`check-pool-lifecycle.py` checks allocation, exact producer order, packet words,
owned completion post and known sentinel bytes.

The actual context constructor calls allocator `76540(bytes=500,pool=6)` at
`11c0af..11c0c5`, and its return observed at `11c0c0` is the real cached resource.
Requested shape is 320 DWORDs by one row, profile class5, hardware format78;
the actual allocated extent is580 bytes. The owning wrapper has refcount2
after creation and still2 at the final retirement-read boundary. `109310`
publishes this interface into the CPU shadow and marks dirty8; neither that
publication nor the observed factory performs a CPU pool memset. Unspecified
pool contents remain unknown, and lifetime cannot end at a CPU callback return.

The FAA triangle's `fb920` changed-pointer block is a counted secondary image:
`[1,45454524,pool|1,3,45e6e6e6,6dc00000,1,1,2,4545bdbc,main|1,aux|1]`.
The distinct pool preparation emitter is genuine stream virtual slot98,
`24f20`, reached during resolve by `fc8f0 -> fc99b`. In this normal trace the
queue is depth0/kind0, so its actual image is RAW, 19 DWORDs /76 bytes:

```
45454524 pool|1
453ce6e6 6dc00000 42000000 000000a1
44444444 30081898 30081898 30081898 30081898
44444444 30081898 30081898 30081898 30081898
45452f2e (pool+284)|1 (pool+2c4)|1
```

Both referenced offsets fit the actual580-byte resource. Resolve subsequently
emits the raw nine-DWORD index24/E6 `[6dc00000,1,1]`/BCBD rebind. Thus the
`453ce6e6` preparation packet and `45e6e6e6` rebind are different actual producer
contracts. This ordering proves CPU command production; it does not establish
what bytes the device writes to pool6 or justify inventing a blanket zero fill.
`shadow_14` in the observer is the raw value at the emitter's supplied shadow,
not an assumed FAA mode field in every shadow class.

The normal sentinel retains its genuine CPU-map owner. The read-only alias
helper now derives the map from raw resource `+1f8`, mapper `+1b4` descriptor
`+1c`, and mapper `+1e0`, rather than requiring the old fixture's saved map
field. Sparse CPU-write validity and the actual alias PTE prove exactly20 bytes
`[a4a3a1a1,28041968,0,0,0]`; the remaining108 bytes stay unknown. The second word
is a literal vendor immediate: `MxMD!acae1`, bytes `c7400468190428`, instruction
`mov dword ptr [eax+4],28041968`. It is neither a guest pointer nor a host input.

Genuine final queue callee `23b80` emits completion words
`[4545e3e6,1,151]`, then posts physical end `0100262c` through MMIO404. The
trace stops at actual `23bc3`, instruction `8b09`, BEFORE loading the GPU-written
counter. Desired counter is150, live common-buffer size159 bytes. Retirement
does not return, and subsequent resource releases do not execute. These values
are this captured frame's values; the checker derives address/counter relations
from their actual owners instead of treating them as permanent constants.

Retained compact proof:
`captures/m9128-device-inputs-20261006/normal-faa-pool-retirement.json`.
Validate it without replay using `check-pool-lifecycle.py --proof <capture>`.
Replay requires explicit `--runtime-tools`, `--target-tools`, actual
`--pci-inventory`, `--pci-resources` and `--dc-snapshot`; the helper always selects
`normal_surface=True`. Runtime dependencies are native `0050ee32` (normal FAA
frame and all its ancestors), target-owner `eae6be4d` (and its ancestors), and
the existing genuine allocator/queue/retirement helpers. No physical target
operation is performed by this proof.
