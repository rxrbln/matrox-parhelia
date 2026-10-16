<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

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

<!-- SPDX-License-Identifier: GPL-2.0 -->

# M9 open topics and evidence, 2026-10-06

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

## October 7 resumed: FAA application work and first live COLOR hardware

The user resumed work to finish FAA and explicitly requested parallel agents.
Three tracks cover the Mesa application bridge, native repeated submission,
and focused vendor command derivation; root serializes hardware tests.
The first live COLOR four-stage upload completes Lock/readback/Unlock and
cleanup without a wedge, but returns an entirely black image. This is a
failed pixel result. Canonical DRM330b6b2 corrects two packed-record readers;
1fd825f prepares current-frame Lock authentication. The corrected build also retires healthy but renders black; the remaining
stage joins are under investigation. Full glxgears/ioquake3 FAA is unfinished.
[Exact live COLOR evidence](../tests/m9-faa-live-color-20261007/README.md).

## October 7 earlier wind-down: historical checkpoint

At this earlier checkpoint all tracks stopped at the user’s request.
The later explicit resume above supersedes that pause. Completed Mesa code, vendor source plans and the failed GL2
hardware capsule are integrated. Canonical DRM7106c67 preserves the variable
COLOR launcher, with experimental four-stage mode default-off and stride32
gated. Its source tree matches the agent's passing module build. The exact
final native draft state and
remaining work are recorded in the [wind-down handoff](parhelia-m9-wind-down-20261007.md).

FAA: two retained triangle draws pass hardware; the new shader-bound COLOR
upload/client and fragment frontend pass host checks. Full glxgears/ioquake3
FAA remains unverified. The one-color four-stage path is experimental and
does not manufacture an unused second varying.

GL2: the unchanged1024×768 native GPU multipass qualification **failed** before
readback: submitted520, completed460/raw461, queue1/wedged1. No passing pixels
or FPS result follows. The target was recovered to boot
`e98c7106-f10b-4680-a5ea-a71af43c0b4e`, module absent, and all locks released.
Code and the original oracle are retained; runtime admission remains opt-in.
The integrated Mesa evidence is in
`tests/native-gpu-padding-runtime-proof/hardware-20261007` (commitsf3d416c,
5237521). Earlier chronological checkpoints below retain their original scope.

## October 7: two retained FAA draws and complete cleanup pass

The latest M9128 batch passes the first live FAA image through Unlock1b0,
then a changed second draw with complete Lock280/readback/Unlock2a0, token
release and file close, helper exit0. All owners and shaders are reused.
Second image has12,495 colored pixels/506 partial edges, with zero checked
interior, exterior-union or channel errors. Raw2a1/generation0/queue0/wedged0
remain healthy after the second Unlock and release. Native2adfafe implements
the final-fence ownership and live timeline/address derivation. This supersedes
single-draw-only status below. Application FAA and continuous frames remain
unproven; the vendor one-color/color-reading-FS route selects a distinct
**two-vector** whole profile, now being integrated for glxgears.
[Two-draw hardware evidence and images](../tests/m9-faa-public-vs-next-20261007/README.md).

## October 7: live FAA triangle and complete single-draw cycle

Current M9128 hardware proof passes INIT b0 → public VS HEAD c0 → complete
DRAW 160 → normal Lock 190 → live cyan readback → Unlock 1b0 → token
release → file close, helper exit 0. Independent live-input pixel checks
find 9,801 colored pixels, 440 partial edge pixels and zero interior,
exterior or channel errors. The queue remains healthy after Unlock and
release. Native source d495803 includes authenticated public-VS Unlock;
the existing encoder remains unchanged. This supersedes earlier statements
below that no FAA fragments have rendered. It proves one live shader
triangle, not glxgears, continuous frames or general application FAA.
[Complete cycle and original hardware capsule](../tests/m9-faa-public-vs-unlock-20261007/README.md).

## October 7: complete FAA DRAW retirement and current parallel source

The latest M9128 graph path physically retires INITb0, preprocessing110,
and the complete returned DRAW plus explicit finish140/raw141. Kernel
`ea659e6` preserves all five source posts and live relocations. Actual
boot364aa8be has an empty healthy queue immediately and+2s. This closes
the DRAW submission/retirement boundary, not application FAA pixels.
The receipt-gated vendor normal Lock now reaches an unread170 wait after
three genuine posts; its complete3556B source plan includes eight live
address fields, five timeline values and two extents. Native Lock translation
and resolved output remain next. See the
[current DRAW/Lock evidence](parhelia-m9-faa-graph-post-init-draw.md) and
[hardware capsule](../tests/m9-faa-returned-draw-finish-20261007/hardware-20261007/).

Canonical Mesa now includes live-CB0 gate specialization, vertex truncation,
opt-in sRGB RT0 writes and opt-in cube faces through64. The archived Q3
case2 source drops from248instructions/78constant pairs to93/55 without
raising188/63 limits; target pixels remain unqualified. sRGB/cube caps
remain opt-in until hardware checks. Vertex log/power have implementations;
older rows calling them absent are superseded, while their numerical hardware
qualification remains open. Production complete-job aggregation is being
implemented with retained BO/arena lifetimes and per-job completion packets.
FAA, GL2, performance and other feature tracks continue in parallel; root
owns the target, with FAA first in the locked queue. No new FPS claim.

## Earlier checkpoint: FAA, GL2 operations and performance tracks

Latest hardware progress: the completed black clear plus the triangle's first
preprocessing launch retires at `0x120`, without wedging. Complete live GPU
buffers are archived; final FAA pixels remain unproven. Mesa f31f2ec constant
reuse passes 16 GPU draws/zero CPU fallback and zero of 1,572,864 wrong pixels.
One matching Q3 run is 40.7 FPS versus 40.8, effectively flat.
[FAA preprocessing proof](../tests/m9-faa-preclear-draw-hardware-20261007/README.md),
[constant reuse proof](../tests/m9-constant-broadcast-hardware-20261007/README.md).


Three background agents own FAA command/output flow, GL2 operations and
performance. Root integrates signed commits and runs serialized hardware
under the established locks. Preserve source-proven optimizations even when
one short benchmark is flat.

The earlier integrated Mesa8ce8651 adds ordinary native mask3b02e61 and zero-stride
current attributes8530cf9/8ce8651. Texgen's native screenshot loses the
horizontal geometry bands; dark stretched texture matches scene behavior.
The ordinary CPU fallback still bands and is a separate open case. Current
attributes pass16GPU draws/zero CPU and0/1572864 wrong pixels at1024x768.
One matching fullscreen basic Q3GL2 run now reports39.1FPS versus35.5, about
10.1% higher across integrated changes and different boots. Six draws/frame
move from CPU to GPU; jobs55→54, zero-stride refusal disappears. Other native
constant/instruction/attribute limits persist. Presentation remains GPUCopyArea.
[Current-attribute pixels](../tests/m9-current-attributes-hardware-20261007/README.md),
[Q3 run](../tests/m9-q3-current-attributes-20261007/README.md) and
[texgen controls](../captures/m9x-texgen-fused-mask-20261007/README.md).

The complete strict1024x768 six-field fixture passes all221184record
components and both786432pixel palettes, with six GPU user draws/zero user
CPU fallback and healthy retirement. Defaultvaryingcap2 remains unchanged.
[Strict record and pixel proof](../tests/m9-gl2-record-complete-hardware-20261006/README.md).

Actual glmark2 wireframe also retires correctly. A queued map-update run
with integrated CPU-only map optimization4f2012e produces a visible wave
mesh,20native draws/zero CPU fallback, queuepeak1 and no wedge. Its one-second
20FPS run includes screenshot readback and startup; it is not controlled A/B
to the earlier34FPS QUEUE0 run.
[Queued-map hardware evidence](../tests/m9-cpu-buffer-queue-hardware-20261006/README.md).
Observed-completion cache071517c removes redundant successful covered waits;
concurrent/error/unreached/wrap/lifetime controls pass. No FPS gain is claimed.
The previous matched clean-built Xorg/modesetting/glamor/GLX observer physically
passes fullscreen Q3 basicGL2:387frames10.9s,35.5FPS, FAAoff/vsync0. Every
observed presentation uses a GPU CopyArea; no PageFlip. This is a different
profile from the older38.9FPS windowed checkpoint, so no controlled gain or
regression is established. A read-only target probe confirms both primary
IN_FORMATS blobs advertise no formats for their custom tiled modifiers,
while LINEAR advertises all five. Async page-flip capabilities remain zero.
[Present observer](parhelia-m9-present-cost-observer.md).

Complex bump clip corruption is now fixed in the qualified native path.
Mesa05c7c29 applies the actual vendor finalizer's coupled-output pair rule:
bump FIRST maskfc00 replacesfe00 while shader instruction bodies stay
unchanged. Actual264vertices pass all1056clip components and1440index bounds;
the PNG shows a textured, lit object, with156GPUuser draws and0CPUuser.
One separate capture-free1024x768/windowed/one-head/FAAoff/vsync0 run of the
same DSO reports417FPS/2.400ms,1251GPUuser draws/0CPUuser and healthy84ac0.
The qualification's37FPS includes dumps/readback and is not a code-speedup
baseline; older34/43/85FPS runs had incorrect geometry. A full independent
application pixel comparison and broader shader coverage remain open.
[Complex bump qualification](parhelia-m9-complex-bump-mask-hardware.md) and
[archived clean measurement](../captures/m9x-glmark-bump-clean-bench-20261007/README.md).
A separate indexed264-vertex variableZ/W fixture physically passes both
6336record components and both65536pixel palettes with GPU transforms.
[Exact indexed variableZ/W proof](../captures/m9x-native-variable-zw-264-20261007/README.md). Indexed app qualification, radial
multipass and optional Q3 lighting remain open. Latest fullscreen basic Q3GL2 checkpoint is40.7FPS1024x768/FAAoff/vsync0;
38.9FPS is the prior windowed profile.
[Bump image failure](../tests/m9-gl2-bump-packed-hardware-20261006/README.md)
and [GL2 status](parhelia-m9-glmark2-status.md).

Component-aware dependenciesbd792ac remove23unused bump VS slots,179to156,
without changing default program bytes. Selected BE-owner waits21a446f
retain exact successful submit points and wait only before reusing the
selected owner. Host error/lifetime/negative checks and full builds pass.
Forced-clear1024x768 hardware regression passes221184record components,
both786432pixel palettes and5BEstate swaps, with healthy retirement. The
run does not independently qualify asynchronous overlap; previous queue
peak and async target are historical. No measured FPS gain is claimed.

Mesa1802132 adds vertex sign; all86host vertex references and five expected
refusals pass. Actual texture:texgen=true uses only GPU user transforms but
its first screenshot was visibly corrupted; the reported946FPS is not a valid
rendering baseline. Mesabb0c20a added bounded ordinary input capture. Two
actual frames exposed scratch registers10/12 beside varying outputs11/13,
with the submitted fixedbf00 mask. Actual vendor comparison establishedbc00;
Mesa3b02e61 removes the observed native bands as described above. Equal-pose
application pixel qualification remains distinct from that image improvement.
[Texgen image and input evidence](parhelia-m9-glmark2-status.md).

FAA public red constants change actual main-store contents, but final output
remains black. Kernel516990f preserves the vendor's omitted post-wait314kick;
one fresh hardware test still reads black after the kick, so the retained
ordering correction is insufficient. Deep review now traces the genuine
public surface-blit admission and operation direction, plus mode9 prerequisites.
The new public SURFACEBLT prototype returns success through the normal copy
dispatcher but has not reached the sampled helper; it is not a corrected
resolve candidate. The helper `aa750` publishes AA mode `1f`, whereas the
ordinary-output helper `abc20` publishes `1d`. Its sentinel binding alone
does not establish that it resolves sampled data to ordinary pixels.
The actual BM option audit proves both options are locked to one and select
resource-backed program transport; final mode9 explicitly selects four
inline instruction pairs. No missing resident firmware initialization or
native enable toggle follows from these option names.
[BM program ownership](parhelia-m9-faa-bm-program-options.md).
This is not working FAA. A genuine ordinary SYSTEM red control
now has a complete source plan and exclusive kernelprofilea6c0b5e:23live
owners,30addressfields,16timelines, noFAAactivation/resolve/companionclear.
Actual encoder placement/alignment and absent-clear controls pass. The
fresh-boot ordinary control now physically PASSES:9520red pixels, zero
interior/exterior/wrongchannel errors and reald0 retirement. The complete
SYSTEMoutput changes from allzero to9520nonzero words. This qualifies the
ordinary write/readback contract for this descriptor; sampled rendering and
resolve remain the FAA investigation. No output pixels are copied from a
capture. [Actual ordinary hardware proof](../tests/m9-ordinary-system-red-hardware-20261007/README.md). [Ordinary SYSTEM control](parhelia-m9-ordinary-system-constant-red.md),
[current FAA evidence](parhelia-m9-faa-native-current.md) and
[SYSTEM admission](parhelia-m9-faa-output-system-admission.md).

The public FAA preclear admission prefix now physically retiresc0/rawc1 on
kernel7a270ae, with both live page-clear ranges and no wedge. The early vendor observer now proves the clear handler already returned;
the triangle remains incomplete and there is no output owner. Exact +20/+40/+60 channels
remain10/0; continuation must use only their actual readings.
[Prefix hardware proof](../tests/m9-faa-preclear-hardware-20261007/README.md).
The preceding normal qualification used boot9054ec2d-cf67-467f-bdb0-6a7d8cc0333c,
kernele1703f9, user submissions enabled and privateFAAoff. Current-attribute
regression passes16GPUdraws/zeroCPU and0/1572864wrongpixels, healthy6a0.
The kernel combines eight temporary submit arrays into one without changing
GPU resource lifetime. Root267valid/2588refusal CPU cases and fullbuild pass;
the one short Q3 hardware check reports40.8FPS versus39.1 (~4.3%ballpark).
SameMesa8ce8651 and fullscreen1024x768/FAAoff/vsync0 settings, differentboots.
Laststeady ioctl6.40→5.25ms; jobs54 and native/CPU draw counts unchanged.
Actual231760/raw231761 retires with emptyqueue/peak4/wedged0, noGPUwarnings.
[Scratch hardware qualification](../tests/m9-submit-scratch-hardware-20261007/README.md).
Current Mesa f31f2ec also adds constant broadcast reuse and its hardware pixel
fixture; the preceding Mesa8ce8651 includes sign1802132, input capture, ordinary native mask
and current attributes; the complex bump
rendering qualification belongs to Mesa05c7c29. The current source retains
the earlier map, completion-cache, packed publication, dependency and wait
improvements. The earlier 34FPS run with156wrong Z/W pairs is superseded by
the correct bump qualification above; it remains historical failure evidence.
Private control tests require fresh boot and evidence archiving before
mandatory recovery. Root owns all target runs.

The older ledger below retains its dated measurements and source checkpoints;
use this checkpoint and linked topic notes for the current work queue.

This ledger reconciles the independent M9 (`102b:0540`, `m9x`) userspace
source at **05c25ed** and kernel **8b4e339** (native combined controls at
a9c5668/4b84cbc; last timedemo bd24336), the October 2 piglit results, and the feature-specific
RE notes. It is a work queue, not a new hardware result. The user has installed
an M9 in nano for deployment and FAA testing; results from that new installation
must record the actual adapter/subsystem identity, boot, kernel and Mesa hashes.
Older M9128 or M9148 measurements remain labelled with their original board.
P690, Parhelia and LX packet meanings do not establish an M9 command.

Current nano deployment is now qualified on M9128 102b:0540/22c0,
kernel7.2.5-t2, boot815d4dcc. See the
[deployment proof](../tests/m9-native-deployment-20261006/README.md) and
[integrated sampler/FAA-resource proof](../tests/m9-faa-sampler-integration-20261006/README.md).
Ordinary texture/depth, queries, four MRT targets and accelerated GLX pass.
One1024x768/vsync-off texture scene reports717FPS; this is not a whole-suite
performance result. One fullscreen Quake3 GL1 timedemo reports49.6FPS on
d1f2b12, a new-board baseline. The sampler base and border regressions pass.
Driver5ad6c17 additionally passes the actual advertised varying-limit test,
after compiler splitting and padding overlap fixes. See the
[interpolation proof](../tests/m9-interpolation-native-20261006/README.md).
FAA mode remains unproved and unadvertised. Historical5ad6c17 profile49.5FPS
had zero native vertex draws. Current32fa687 packs independent identity
texture pairs into one stream and preserves interpolated color in the second;
actual texture/affine/depth controls pass. One1024x768/vsync0 Quake3 run is
51.4FPS, about99.2% GPU vertex draws and zero skips; see
[latest native proof](../tests/m9-packed-coord-native-20261006/README.md).
Native constant batching now passes textured A/B/A and49-matrix pixel
references, including depth, with actual3/49 constant runs per job. One new
1024x768/vsync0 run reports56.6FPS,55jobs/frame and zero uniform splits;
see [batching proof](../tests/m9-uniform-runs-native-20261006/README.md).
ea97652 enables the tested path by default. Remaining CPU draw is triangle
polygon offset (factor=-1,units=-2), about0.8% of calls/0.03ms per frame.
Native D24 polygon offset is now implemented in0b101e9/c74a519, with typed
kernel admission in7da488d. Source408e866 probes that admission without GPU
execution and retains CPU fallback on older kernels. The DSO build and batch
suite pass. Kernel4b84cbc with Mesa a9c5668 now passes native texture,
depth-tested offset and depth-disabled offset controls, each0/76800 wrong;
see [combined control proof](../tests/m9-completion-native-20261006/README.md).
The99.2% and56.6FPS measurements above predate this feature.
FAA companion allocation now matches28 genuine vendor geometry/descriptor
cases (fb4e710). Main and auxiliary stores now use genuine linear rows and
independent32-byte guards, even at the32MiB budget cap (7443ff6/d8819db).
Actual-C helpers match six independently captured vendor cases; ownership
sanitizers, complete batch suite and DSO build pass. The corrected resources now pass native1920x1200→2048x1536→1920x1200
same-context growth with AA mode off, strict readbacks and texture controls;
see [guarded-resource proof](../tests/m9-faa-guards-native-20261006/README.md).
They have no FAA-on rendering proof.

The [October 2 piglit status](parhelia-m9-piglit-status.md) supersedes the
September 18 backlog where they disagree. The glmark2 standing is still the
September 20 **M9128 at 1920x1200** result, not a current nano benchmark. Root
owns the target and its established hardware locks. Background investigators
work on local evidence and isolated branches and submit bounded tests to that
owner; historical scripts' different lock names do not authorize concurrent
access.

## Priority and focused agent scopes

FAA is first. With four concurrent agent slots including root, retain the
target owner and FAA investigators, then rotate the remaining topic agents.
Each row is a separate scope; an inventory is not evidence that every row has
an agent running simultaneously.

There are currently three background agents: `m9_faa_emulator` traces the
shared allocator and platform/OS contracts; `m9_faa_native` traces the FAA
resource graph, initialization, binding and lifetime; `m9_open_topics` works
on asynchronous completion and presentation. Root integrates their results
and owns physical tests. Remaining shader, texture and conformance traces
are queued. Some have partial vendor evidence, but they are not all closed.

| order | scope | actual remaining gap | next useful proof |
|---|---|---|---|
| 0 | deployment and baseline | Qualified on current M9128; broader scenes remain | Retain the pinned deployment and post-test health proof when testing each new candidate |
| 1 | FAA vendor emulation | Genuine normal target-owner publication independently passes with recorded DC inputs. Normal surface-owned frame now reaches resolve and its second launcher retains the FAA graph; strict descriptor/order audit is pending. Earlier manually bound frame remains incoherent. Exact counted-secondary export and CPU sentinel alias publication are proven | Finish strict normal-frame audit using actual selector13 companion/selector12 HZ descriptors and both ordered launchers; retain actual pool initialization, resource membership and retirement before executable translation |
| 1 | FAA implementation and native probe | Existing diagnostic state does not prove multisampling: previously zero of 318464 store dwords changed | Compare the vendor-owned resource/enable/reset/resolve graph, test one edge scene, read back store writes and resolved edge coverage; follow with depth and texture controls |
| 2 | max-samplers regression | Base and border cases closed: stale CPU record layout after fragment specialization | Both native cases pass32seen/zero skips after d8fa015, without capacity changes |
| 2 | kill/early-z state | Three quad lanes use an earlier quad's kill outcome with our instruction and an unchanged vendor program | Obtain genuine vendor engine MMIO while `fs-discard-cond` runs and compare initialization plus job-head controls |
| 3 | interpolation splitting | Advertised varying-limit case closed; broader scene schedules remain | a4995c2 classifies allocator pressure;5ad6c17 excludes overlapping padding. Actual glsl-max-varyings passes69seen/zero skips. Fourteen-input stress passes offline only; native VS still has a five-input limit |
| 3 | per-fragment TXB | Uniform/draw-constant bias works; saved two-pass route uses literal +1 without mip filtering; varying bias remains refused | Run the prepared LOD-sensitive vendor probes before implementing packed coordinates/bias and captured selector mode 3; see [capture boundary](parhelia-m9-txb-capture-boundary.md) |
| 3 | fragment control flow and values | General control flow, some value splits and shadow-reference clamping remain refused or unstarted | Map one existing vendor control-flow program into the native pass graph; prove branch-dependent color/depth/kill semantics before generalizing |
| 3 | native vertex ISA | Vertex logarithm/power remain refused; six odd temporary registers are excluded after native failures | Capture a genuine vertex log2 seed and complete chain; separately trace output/register lifetime to determine whether any excluded register can safely be recovered |
| 4 | complex split scenes | glmark2 terrain remains incorrect/slow in the recorded standing; partial split coverage is implemented, not a blanket missing feature | Capture one bounded terrain/split case against a reference with skip accounting and identify the first bad intermediate rather than reporting its FPS as conformant |
| 4 | texture layout and filtered copies | Cube faces larger than 16 are not placed; non-halving filtered engine copies have known failures | Trace the vendor cube address/layout calculator above the packed tail; use existing `blitlin:75x37`, `blitlin:74x37` and `blitlin:100x40` controls for the copy issue |
| 4 | MRT blending and combined operations | Four unblended targets work; blending has per-target replay, but single-pass destination reads for RT1..3 and combined stencil/discard/split cases remain open | Capture destination-read registers and program image for RT1..3 blend, and verify each target plus retained BO membership |
| 5 | performance and GPU geometry | Current 05c25ed / kernel8b4e339 at1024x768/vsync0:56.3FPS, all steady gameplay transforms native;56jobs/frame,zero uniform splits. Earlier56.6FPS used older sources, so performance is flat at single-run precision | Reduce remaining submission costs; retain packing, batching, GPU polygon offset and queued ownership improvements |
| 5 | presentation and async submission | Four-job opt-in queue passes native matrix/depth and shared-FS vertex-program A/B/A controls. Q3 reaches four actual jobs in flight; post-game texture and WAIT pass on the same boot. Copy presentation passes; opt-in flip switches DC_START but output enable reports -67 | Retain [native queue proof](../tests/m9-queue-native-20261006/README.md); qualify physical flip output separately. No repeat timedemo needed |
| 6 | 10-bit masks and conversion | Unblended single-target partial masks use a constant-color blend fallback; already-blended and multiple-target partial masks remain refused, and converting readback needs current retest | Prove the existing unblended mask fallback, implement captured mask lowering for combined cases, then test converting readback and linear filtering |
| 6 | 10-bit display verification | M9128 KMS/GBM/EGL 30-bit path passed; panel precision and transmitter depth remain unverified | Repeat on the installed board and inspect the physical output; keep raw storage precision distinct from panel output precision |
| 7 | query and kernel lifetime/recovery | Query counter wrap, split-pass sample accounting and fault/recovery boundaries require targeted qualification | Audit the current kernel first, then submit a bounded wrap/lifetime test to the target owner; no unsupported arbitrary requester state |
| 8 | pixel conformance | Eight-bit GL_DITHER ignored; polygon stipple capture decoded but not implemented; minor rounding/state tests remain | Schedule after FAA and major rendering/performance gaps; use the captured state plus explicit pixel references |

## What is already implemented

These are controls for deployment, not new open feature implementations:

* Native vertex and fragment programs, textures, derivatives, variable
  indexing, shadow/depth sampling, and framebuffer pass splitting exist.
  Native VS defaults on, with CPU draw fallback for refused shaders. Native
  program length is bounded at 188 slots and its constant span at 63 pairs.
* Native VS compiles up to five varyings (`m9_vs_compile.h`); the default
  fused hardware path uses two output streams. Three or more require an
  additional preprocessing draw that remains unqualified on this board. Native FS has six interpolation
  registers, four fetch units and up to sixteen raster passes. API sampler
  declarations advertise eight so unused variants can link.
* Interpolation splitting is implemented in `m9_fs_compile.c`. Register
  pressure now enters `FS_CUT_REGS`; the advertised-limit piglit case passes
  after padding overlap repair. Broader complex scenes still need qualification;
  the six-register hardware limit is unchanged.
* Four MRT destinations were measured pixel-exact, at pairs `0x2c+4p` and
  `0x2d+4p`. The fragment compiler refuses single-program MRT
  blend/discard/pass splitting. `m9_mrt_blend_split()` replays blended draws
  one target at a time, with equal-depth/no-write and query pause on later
  targets; it refuses a live stencil test. Single-pass destination-read
  decoding remains open, without reopening basic four-target routing.
* Occlusion queries are implemented and advertised. Logic operations were
  implemented at 6780573. Stencil, fog, sRGB texture/write support and GL 2.1
  exposure supersede the earlier partial or unreachable claims.
* Fragment `nir_op_flog2` is implemented. Native **vertex** logarithm and
  power remain refused; the two stages must not be conflated.
* The six bad native VS temporary registers (`11`, `15`, `17`, `19`, `1b`,
  `1f`) were removed from allocation. Their mechanism is open RE with
  possible performance benefit, not an unmitigated allocator defect.
* Partial mesh split draws have the padded path by default
  (`M9X_SPLIT_PAD=true`). Earlier blanket refusals are historical. Correct
  scheduling and coverage still need proof for any newly admitted shape.
* Cube sampling in the captured tail works up to 16-pixel faces; volumes are
  placed up to the advertised 256 extent. This is not a blanket missing cube
  or 3-D texture implementation.
* Ten-bit render, nearest sample and clear, plus the
  `SRC_ALPHA/ONE_MINUS_SRC_ALPHA` blend pair, were measured on M9128. The
  actual KMS/GBM/EGL 30-bit chain and packed codes were also measured.
  `m9_draw_vbo()` implements an unblended single-target partial mask as
  `src*m + dst*(1-m)` with an exact 0/1 constant mask. Combined blend/MRT
  masks remain refused, while filtering precision, other blend factors,
  converting readback and panel/transmitter verification are separate
  questions. The older GigaColor note predates that fallback.

## Latest FAA command and resource evidence

The implementation gate is one complete vendor FAA frame: allocation,
initialization, normal state compilation, draw, resolve and retirement.
Canonical7806a76d/613dcc84 now provide the joined CPU frame through final
DMA submission and the first genuine GPU retirement-read boundary. Root
independently replayed both:1768 live-owned DMA writes, nine known byte spans,
actual public context/triangle success, FAA mode1f draw and mode1d resolve.
Every exported DMA byte matches an actual vendor CPU write within its owner.
The final real completion packet requests130 hex and posts01002780; execution
stops before status load23bc3 from the actual common-buffer physical02007000.
No GPU completion or subsequent release is supplied. Kernel-owned indirect
state initialization and complete operand/backing provenance remain executable
translation requirements. This is CPU evidence, not a rendered FAA image.
Subsequent review found the public DP2 triangle replaces the FAA companion/HZ
after manual aa750 binding. Root confirmed B8 changes01a51001→012a8801 and
BA01ae9001→0, with ordinary descriptors. Mode1f alone is insufficient.
The joined sequence is therefore not yet a semantically coherent FAA frame;
23000001 alone does not prove the63000001 raster launch. Native agent owns
the actual FAA context/target-state join; emulator owns the count-prefixed
secondary consumer contract. The strict checker must additionally assert FAA
attachment identities and raster execution before porting.
Canonical5661c4f4 now activates the real vendor queue with32 owned system-DMA
allocations through genuine MM handlers and bounded synthetic OS RAM, using
the genuine FAA-admitting MmFlags101b8 option. Root independently replayed
this successfully. Default101f8 has a separate mapping contract still open;
its class1 predicate alone was incorrectly treated as terminal rejection.
Canonical58ac2169 fixes the capture's initialization order: queue activation
precedes resource/shader enable, so the shader uses the live TaskMgr+0xc.
The genuine PDEV CPU alias publisher supplies the existing timeline owner.
These close the earlier f5969 and missing device+58 CPU boundaries.
Root independently replayed actual ac7e0 returning1, all three initialization
clears,900 system-DMA CPU writes and sentinel initialization; no GPU execution
or completion was supplied. Main/auxiliary/companion/HZ extents at320x240 are
136080/13080/97000/3000 hex. Captured packets must not be concatenated to
infer normal ordering. No FAA fragments or resolved target image have been
demonstrated.

Kernel1f1ff0d admits the exact nine-word FAA binding grammar for validation,
with typed pool ownership, tagged VRAM addresses and extent checks. Actual
pool execution is refused until kernel-owned initialization and lifetime are
implemented. The actual-parser check passes the captured record and rejects
202 malformed or unsafe cases; this is CPU validation evidence only.

Actual128-byte M9128 capability EEPROM now executes genuine31f90/3265f and
populates type14. Both SPI payloads and EEPROM responses are from this board;
full adapter initialization is still incomplete. Retained live MM owners and
actual25-device PCI/assigned-bus inventory advance the normal runtime profile
constructor. Actual configured companion477f0(format3d) admits sample2; this
is a constructor prerequisite, not a native FAA rendering result.

The genuine conditional shared allocator now returns1 and publishes its
resource owners using the actual OS-owned aperture mapping. Its128-byte
synchronization counter comes from a real vendor CPU producer backed by
bounded synthetic OS system RAM, not a GPU response. Integratedb0b416c8
passes independent shared-allocator and common-buffer ownership checks.
Normal85d50 mapper publication remains incomplete. See the
[allocator evidence boundary](parhelia-m9-faa-re.md).

[Companion recipe proof](parhelia-m9-faa-companion-resource-20261006.md) corrects
the old linked-format9 BA/BB interpretation: AA uses64-bit format3d/layout2
and its own optional HZ plane. The1024x768 preallocation extent is600020;
sourcefb4e710 now includes that guard and the actual sample-layout2 descriptor.
Ordinary context pool6 is a distinct1280-byte resource; its allocated graph
is captured, while its GPU initialization and executable ownership remain
open. The genuine allocation stores an aligned1408-byte extent.

Actual fce50/fc8f0/24f20 command execution now captures full contextrestore
thenA1/A3/A4save:29GPUwords,6genuine relocations, strict negative controls.
It is distinct from the existing24/E6/BCBD surface binding and pointer reset.
Native FAA still requires complete physical owners/content/lifetime, enabled
draws, store writes, resolved edge coverage, depth and texture controls.
Two FAA agents own normal allocation and platform/OS backing joins; the
performance agent continues independently. Root owns all target tests.

## FAA evidence boundary

The separate worktree `wip/m9-faa-emulator` contains M9-specific execution
evidence now integrated into this RE tree through **b0b416c8**.
It uses the genuine MSerX `MxMD.dll`, `MxMGLX.dll` and `MxMM.sys` for
`102b:0540`. Its actual `0xfb920` AD/AF emitter and `0xfab30`
`RESET_FRAG_PTR` emitter ran under the established PE/Unicorn emulator with
controlled ABI owners. These are real instructions, but the owners are not
the result of a fully initialized multisampled API surface.

Genuine SPI identity responses were recovered from retained vendor-X trace
echoes and checked against1123ROM bytes. The actual miniport now passes flash
identity and PInS parsing. A bounded ownership model grounded in nano's
request/grant/restore capture progresses to GPIO1c4c. The earlier FindAdapter87
was an emulator chip-select error. Full FindAdapter/HwInitialize, configured
FAA allocation, draw, resolve and physical store retirement remain unproved.

The genuine surface emitter additionally executes conditional index24,
E6=[6dc00000,1,1], thenBC/BD when the device-owned per-slot resource exists;
the null owner skips that whole block. These packets are missing from the
Linux FAA diagnostic, but the resource producer/physical type must be joined
before replay. Normal shipping Windows configuration and API routes exist:
OGL\\Multisample nonzero plus3D\\AAMode bit0 enables32-bit WGL pixel formats
advertising eight samples. That establishes static reachability, not a fully
initialized API draw. Dead debug strings are separate weaker evidence.
The dynamic configuration/query chain now executes both constructors and
real driver escape, with actual ICD enable matrix0/0/0/1. Pixel-format
capability initialization is incomplete: the harness rejects ordinary
hardware formats and its appended fallback formats actually advertise zero
samples. The flag alone must not be called dynamic AA advertisement; the
agent is tracing that capability producer and rejection condition.

Current diagnostic source separates `ad`/`af`, the `ef[31:29]` AA mode,
`f0` control bits, `bc`/`bd` store bindings, `b8` companion and reset state.
The old hardware probe's zero changed store words is a failure to establish
FAA enable, not proof that M9 has no FAA engine. MSAA remains unadvertised.
`af` bit 11 alone previously stopped the engine. Index `c7` emitted mid-job
also stopped it; do not transplant that isolated value from a vendor job
head into a live raster range.

An acceptable native FAA result needs all of: real M9 renderer, visible
baseline, changed fragment-store contents attributable to the draw,
resolved edge coverage different from single sample, correct textured and
depth-tested output, and full retirement without timeout. A completed process
or altered command word alone is insufficient.

## Existing bounded proof tools

The source tree supplies `m9-batch-test`, `m9-vs-compile-test`,
`m9-fs-compile-test` and the `--srgb` compiler test. Compiler tests prove
construction/reference behavior; several earlier native VS defects were
shared by the simulator, so they do not replace target readback.

The RE tree supplies `tools/m9-mesa-glsl.c` and `tools/m9-mesa-vs.c`, with
texture, depth, split, MRT and GigaColor controls. For larger source changes,
`tools/m9-mesa-glsl-suite.sh` asserts `GL_RENDERER Matrox M9` and stops after
a kernel stream timeout. Use the established root-held locks around it and
submit only the relevant cases first. The current target baseline should
be brief; do not rerun old full suites to manufacture a new FPS claim.

`tools/m9x-glmark-default-suite-fresh-x.sh` defines the thirty-three benchmark
list; `M9X_SKIP_REPORT` and `M9X_PROFILE` expose refused draws and costs.
Use one short 1024x768 run for a new candidate and record vsync explicitly.
Retain correct features and optimizations even when one sample is unchanged.

The fresh nano baseline, FAA result and final deployed hashes belong in a
separate dated test record linked here when the target owner finishes them.

## Detailed owners of the remaining evidence

* [Piglit failures and capture requests](parhelia-m9-piglit-status.md)
* [FAA static and native evidence](parhelia-m9-faa-re.md)
* [Fragment backend and kill artifact](parhelia-m9-mesa-fragment-backend.md)
* [Native vertex backend and corrected allocator](parhelia-m9-native-vertex-backend.md)
* [Raster interpolation limit](parhelia-m9-raster-stream-limit-re.md)
* [Native control-flow decode](parhelia-m9-native-control-flow-re.md)
* [Cube and volume placement](parhelia-m9-cube-volume-textures.md)
* [MRT routing, superseding older retirement blockers](parhelia-m9-mrt-routing.md)
* [glmark2 standings, costs and incomplete terrain](parhelia-m9-glmark2-status.md)
* [Ten-bit results and remaining questions, section 9](parhelia-m9-gigacolor-10bit.md)
* [Buffer sharing and linear-sampling boundary](parhelia-m9-buffer-sharing.md)

The September 18 implementation backlog and outstanding-features note are
historical records. Their one-varying, absent MRT/query and unreachable sRGB
rows must not be used as the current task queue.
