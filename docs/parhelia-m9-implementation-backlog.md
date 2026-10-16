# What the M9 driver still has to implement

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

## 2026-10-08 latest: seven ordinary scenes pass; X survives; Quake3 gaps persist

DRM5f07050/Mesa32bc9ec and matched X cursor fix63569210 were tested once on
the M9128 at1024x768 with FAA disabled. Glxgears and six glmark cases pass
basic scanout review with no skips;108.734/113–115 appFPS, mode60Hz. X now
survives both Quake3 exits and the full nine-case batch. GPU final1a8420,
generation0, empty queue, wedged0. Full module teardown/reload allowed this
ordinary transport on the same boot after FAA; direct context handoff is open.

Quake3 GL1 still has stretched weapon geometry/HUD corruption; the C2
pending-job ownership fix is retained but did not close that pixel defect.
GL2 mix and bounded split/SFU improvements expose an ENOSPC two-pass
submission failure, followed by a lost context and loading-only pixels.
Investigate the actual command/resource capacity; do not revert the compiler
improvements or raise the measured six-interpolant/five-native-output limits.
Piglit and complete GL2 remain unqualified. [Current proof and image review](../tests/m9-nonfaa-regression-20261008/hardware-combined/README.txt).

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

This separates three things that the readiness ledger deliberately mixes:
what the **vendor hardware** does, what we have **reverse engineered**, and
what **this driver actually implements**.  Only the third column decides
whether a feature works today.

Checked against the driver source on 2026-09-18.

**Current queue:** [M9 open topics, 2026-10-06](parhelia-m9-current-topics.md)
reconciles this historical ledger against source 6780573 and the October 2
piglit results. In particular, multiple varyings, four MRT targets,
occlusion queries and sRGB support have landed. Vertex log2 remains open;
fragment log2 is implemented. Preserve the dated evidence below rather than
reopening completed work from its older status rows.

## Reverse engineered and verified, not implemented

These need no further capture.  The evidence is in the tree and in most cases
was confirmed against a live vendor GL context.

| feature | evidence | driver today |
|---|---|---|
| ~~**Occlusion query**~~ | - | **implemented and advertised** (`m9_query.c`, `caps->occlusion_query`).  Three things the RE note did not have were needed beyond the validator changes: the counter is off until `a7 = 0x00ffff7f` / `c6 = 0x00ffff09` name it; the counter is **free running and shared by the context**, so a result is a difference and a query may span jobs; and the closing record must be ordered behind the draws or it publishes a partial count (5919, 5578, 5297 on three runs of the same 7000-sample draw).  MEASURED: `occvis` 7000, `occpart` 4130, `occhid` 0 beside a visible companion at 7000, `occspan` 10000 across three jobs.  **UNTESTED**: a range passing more than 2^32 samples (the counter is 32 bits while core Mesa reports 64), multisample, and a query around a split shader, which counts the first raster pass only |
| **Rectangle textures** | **implemented and MEASURED.**  The premise this row carried was wrong in its mechanism: the engine has no unnormalized addressing mode at all.  CAPTURED, the vendor's 13x7 rectangle emits the descriptor words of a 13x7 2-D texture byte for byte and differs only in the three wrap fields of `7c`, while its draw alone carries `1/13` and `1/7` as a program constant, so the reciprocal has to be folded upstream of the sampler.  This driver folds it into `unit_scale`, the per-vertex term that already scales a render target of unaligned width, so no shader constant is needed.  Six listed fixtures, exact at the content edge; `texrectborder` is dispatched but unlisted, because clamp-to-border returns the border with red and blue exchanged and correcting it contradicts a captured vendor word.  See `parhelia-m9-rectangle-textures.md` | closed except clamp-to-border |
| **Separate stencil** | exact face controls and live separate-op packet.  Note the vendor's own API rejects valid LESS/GREATER enums - implement correct core behaviour, do not copy the bug | partial |
| **Fog coordinates, blend equations** | modes and destination-sensitive results captured; static mapping covers operations 1..5 | partial |
| **sRGB write** | RT0 `SRGBWRITEENABLE` emits an exact 25-slot shader suffix; clear has the exact CPU encode curve | the backend implements it behind `m9_fs_blend.srgb`, but `m9_format.c` advertises no sRGB colour format, so the state tracker never binds one and the path is unreachable.  Advertising a format is what remains |
| **Second and further vertex varyings** | output pair `(0x11+4j, 0x13+4j)` feeds the target at `0xb0+2j`, CAPTURED 310/310 passes; count declared in `ef` bits 19/21/23; stride and per-target placement are independent descriptor fields (bits 7:0 and 15:8, in dwords), so several preprocessing draws share one packed record and a route-fed final draw de-interleaves it - see `parhelia-m9-varying-transport-re.md` | one varying (`M9_VS_MAX_VARYINGS 1`); the two-draw chain failed on hardware once, retested after the `ef` chain-role fix |

## Reverse engineered, implementation in progress

| feature | state |
|---|---|
| Framebuffer multipass fragment | **implemented and on by default.**  The missing state was index `ef` bits 10:9, a pass's place in the sequence (0 only, 1 first, 2 middle, 3 last).  The record is four 32-bit floats a fragment, indexed by the engine's fragment counter.  Depth is evaluated in the **first** raster pass only.  Scene results in `parhelia-m9-glmark2-status.md`; what still stalls is a split draw returning to an earlier render-target shape |
| MRT, four render targets | **done and MEASURED 2026-09-19.**  The surface side was already pinned offline; the fragment backend now compiles up to four colour outputs, the context binds what the framebuffer holds, and `caps->max_render_targets` is 4.  The destination that names target p is MEASURED as the pair `0x2c + 4p` / `0x2d + 4p` -- the DERIVED `0x2d + p` was wrong, `0x2e` reaches no surface -- and a program with more than one colour output may not hold a live value in the scratch pair `0x11`/`0x13`, which is the colour output's own staging.  `mrt:4` renders 0 of 76800 pixels differing on all four targets.  Still refused: a multiple-target shader that discards, blends, or does not fit one raster pass.  `docs/parhelia-m9-mrt-routing.md` |

## Established as unreachable on this hardware

Recorded so they are not attempted again.

| feature | why |
|---|---|
| **R8, RG8** | MEASURED: the M9 narrow family is luminance/alpha only (`0x30` L8, `0x31` A4L4, `0x33` A8, `0x50` L16, `0x51` A8L8); the firmware's 52-record format table holds no R8 or R8G8, and there is no sampler swizzle field - the format code *is* the swizzle.  Added experimentally with storage expanded into the RGBA8 family, an imported R8 plane samples nothing: "texture is not stored in a captured sampler layout".  `GL_RED` additionally needs R8 as a render target, for which no code exists.  Advertising it would make EGL accept an NV12 or R8 import and return black |
| **Sampling a linear 2-D surface** | MEASURED directly, not inferred: a probe held the image in the texture layout and switched **only** the storage and the kind byte.  Tiled written through the raw map (`01`) and tiled written through the kernel window (`21`) both sample exactly; linear sampled as `00`, `08`, `10`, `18`, `20` all fail at 76800/76800, as do linear bytes carrying a tiled kind.  The load-bearing control is the tiled 16x16 case, which writes through the *same* raw mapping the linear cases use, so the write route is engine-faithful and the layout is the only variable left.  This matches the captures: every sampled linear surface there is 1-D or 1x1, while the linear 2-D surfaces the engine *writes* - hierarchical Z, vertex-output targets - are never sampled.  It is what puts NV12 plane sampling and foreign dma-buf sampling out of reach, independently of format codes |

## Landed since the ledger was written

| feature | state |
|---|---|
| **Engine-side copy and blit** | `resource_copy_region` and `blit` run on the raster back end through `util_blitter`, guarded to destinations bindable as colour target 0 and sampleable sources, with the CPU transfer as fallback.  Two safety properties: a draw the backend cannot express is *dropped* rather than failed, so the skip count is checked and the CPU path redoes the copy; and the engine path stands down inside a live occlusion query, where its draws would be counted.  This is a **correctness** fix as well as a mechanism one - the CPU path ignored the filter and sampled nearest, so `blitlin` and `blitlin:32x128` fail on the pre-change binary at 76776 and 76720 of 76800 pixels |
| **A filtered blit was resampled NEAREST** | `m9_blit_convert` ignored the filter, so every reduction through the CPU fallback was a 1-of-4 decimation rather than a reduction - every CPU-built mipmap level, and any scaled window copy or `glBlitFramebuffer` that falls back.  Silent: aliased output at a good frame rate, which is why it survived.  Found while implementing `generate_mipmap` |
| **TGSI helper shaders reached the native backend** | `util_blitter` builds its shaders with `ureg`, which delivers TGSI, and `m9_create_fs_state` only reached the native backend for NIR - so every blitter draw was dropped with a **null reason**: the path looked available and silently did nothing.  Any Gallium helper shader hit this, not only the blitter |

## Hardware limits established 2026-09-18

| limit | evidence |
|---|---|
| **A multi-pass fragment job whose geometry leaves part of its render target uncovered stops the engine** | MEASURED.  Two passes suffice, the record is not involved (`M9X_SPLIT_STAGE=1` leaves it unbound and it stalls identically), and a large target is not needed (it stalls at 320x240).  A single-pass shader over the same partial rectangle renders 64 draws over, and the same geometry as two triangles rather than a strip stalls identically - so it is the multi-pass schedule over part of a target, not coverage or topology alone.  The boundary is not sharp: 1, 2, 8, 10, 12, 16 and 20 partial draws render, 24 and 32 stall, so no count-based rule can ship.  It is a **back-end** hang: the engine's fetch pointer consumed the whole submission (dword 171 of 171) and the writeback fence never reached its target.  `glmark2`'s `RenderWindowBlur` blurs a window back out over the screen at the window's position, which is why `desktop` hits it inherently.  The driver refuses such a draw by name; the fix is to render into a scratch surface the geometry covers and blit it into place.  **A second answer now works as well**: pad the draw out to the target's extent with a one-pixel frame and narrow the clip window to the pixels it covers (`M9X_SPLIT_PAD=1` with `M9X_SPLIT_BE=3`).  Its blocker was the same back-end clear race; with the swap it runs every split fixture pixel-exact and `desktop` end to end at 37 fps over **16** jobs a frame where the scratch path takes 28 (MEASURED 2026-09-19).  It is not the default, because it is not faster and the scratch path is what the thirty-three were measured on -- but it is the path a **mesh** needs, since a scratch surface only helps a draw that fills its box |
| ~~A surface of 64x64 or less whose height is above 16 and not a multiple of 8 samples the wrong texel~~ | **Superseded, and it was never a hardware limit.**  The rule is **odd height**, and the fault is a driver bug in one unit: the CPU window writes the **last row of an odd-height surface into the first row of the second page**.  Exactly two rows are wrong in every failing case; at width 64 row 4 holds row `height-1`, at width 32 row 8 does, and 4 and 8 are `256/row_span` - the rows in one 1 KiB page.  MEASURED producer-versus-consumer: `rtt:64x23` PASSES and `tex:64x23` FAILS on the identical shape with identical descriptor words, so the raster engine and the sampler are both correct.  Not small-family-specific (65x23, 100x61, 16x127 fail); width never enters it.  `13x7` and `16x15` are exact because the 32x16 packed-tail minimum lays them out with 16 rows, and `17x9` is exact because its misplaced last row lands on itself - both predictions of the model, not exceptions.  Fixed by `m9_surface_even_height()` in `cd61a2c` |

## Rules established 2026-09-18

| rule | consequence |
|---|---|
| **A surface's declared width only normalises the texture coordinate; addressing follows the surface geometry** | So a producer and a consumer may state different widths without harm, and **widths need no rounding**.  MEASURED: `rtt:40x32` and `texchain:40x32` both pass with the producer describing the surface by its 32-aligned span (64) and the sampler binding the raw width (40).  This **retracts** an earlier entry here that called those two disagreements defects: they were found by reading the code, and the hardware normalises the difference away.  No patch was landed for them, which was the right call - a plausible patch for a defect that does not exist is worse than none |
| **The border colour registers `7e`/`7f` carry the four channels in the descriptor format's own order** | `c8`/`c9` are R, G, B, A; `cc` is B, G, R, A.  This dissolves an apparent conflict between a captured vendor word (red in `7e[15:8]`, a `c8` surface, and the capture's own rendered frame confirms it) and a driver measurement showing red and blue exchanged (a `cc` surface).  Both are correct.  A previous "surface channel order" hypothesis was **withdrawn on a void experiment rather than disproved**: st offers this driver exactly one colour-renderable order, so the control fixture could not vary what it appeared to |
| **`M9_REG_COMMAND_BASE` (0x400) reads back as the engine's fetch pointer** | It is written once per slot with that slot's page-aligned base, so subtracting the base turns a timeout report into a byte offset into the submitted command image.  This is what "0x400 and 0x404 mismatched after a stall" has always meant.  The driver now names the stalling dword |

**The vertex ISA's gaps are decoded.**  There is **no sine, no cosine and no
integer ALU** on this hardware in either stage: the vendor lowers `sincos` to a
`fract` range reduction plus a degree-four minimax polynomial, whose
coefficients sit in `MxMD.dll`'s compiler constant pool at VA `0x20af18`
immediately after the exp2 and log2 polynomials.  The driver lowers it the same
way, from instruction forms the backend already emits - `M9X_VS_SINCOS` is
**on by default** since 2026-09-20, MEASURED exact over 1200 arguments spanning
`[-4pi, 4pi]` and the reduction's edges, worst error **7.085e-05** on the engine
against 6.925e-05 offline.  `iand`/`ior`/`inot` were a missing *lowering*, not a
missing encoding, and are implemented.  `flrp` never reaches the backend.
**`flog2` stays OPEN** and is a genuine encoding question: log2 needs a hardware
seed for exponent extraction that no float lowering replaces, and exp2's and
log2's seeds differ only in `W1[13:12]` - the probe is in
`parhelia-m9-vertex-isa-gaps.md` section 4, and it will probably lock the
engine.  The register-register dot product `op2 = 0x0d` is DERIVED and
unimplemented; both backends expand a dot of two computed vectors into
multiplies and adds, where two instructions would do.

## Known defects, cause understood

| defect | consequence |
|---|---|
| Projective fetch whose projector varies across a primitive divides per **vertex** | MEASURED wrong in 75373 of 76800 pixels.  Currently masked by an accidental refusal, so loosening that check without fixing the divide turns a refusal into a silently wrong picture |
| One constant span per job | a per-object transform ends the job; cost the native vertex path 10% on `ideas` and 5% on `pulsar`.  **Half of it is closed**: a texture coordinate's scale and bias, which used to live in that span and made every render target of a different addressed span end the job, now ride a source-record slot instead (`parhelia-m9-native-vertex-backend.md` 4a).  MEASURED on glmark2 `desktop:shadow`, the scene that hit it hardest: **16.00 jobs a frame on 8.25 uniform splits** became **6.67 jobs and 0.00 splits**, which is the draw module's own 5.67, and 124 fps became 131.  What is left is the shader's own uniforms, which no record slot can carry because there are only four and a matrix is eight pairs |
| ~~**One job per draw is the driver's submission model**~~ | **Resolved 2026-09-19, and mostly it already was.**  A job ends on a state change, a uniform change, a full record ceiling or a full command image; `M9X_JOB_DRAWS` defaults to 0, so nothing else bounds the draws an unsplit job carries.  MEASURED at 1920x1200 with `M9X_PROFILE=100`: `ideas` puts **351 draws into 20 jobs** a frame, `shading=phong` 7 into 3.  Where jobs and draws are 1:1 the reason is one of the two real ones: a mesh draw fills the ceiling on its own (`build`, 2 jobs 2 draws; `bump` high-poly, 3 jobs 3 draws), or the job is split, and a split job carries exactly one raster draw because two stop the engine.  So `refract`'s 70 jobs could not have been batched: every one of them is split |
| **The engine does not act on the `ef` cull bits** | MEASURED 2026-09-19 on the M9128: `M9X_NO_CULL=1`, which clears them on a native draw, moves neither the frame rate nor the ioctl phase on three native scenes at 1920x1200 -- `bump:normals` 191 fps / 5.125 ms against 191 / 5.130, `texture` 288 / 3.417 against 289 / 3.404, `build` 322 / 2.856 against 323 / 2.840.  If the engine were removing back-facing triangles, taking the bits away would have made it rasterise them.  It is masked on the draw-module path, which removes them on the CPU first: `texture`'s cube reaches the engine as 22 source records a frame through the draw module and as a fixed 40 natively.  So **every native draw of a closed mesh rasterises its hidden half**, and closing this pays on every native scene rather than only the textured ones.  `m9_cull_ef()`'s encoding is CAPTURED and the batch test checks it, so what is missing is a second condition the engine wants, not the field |
| A filtered **engine** blit whose reduction is not an exact halving loses pixels | MEASURED with plain level-0 textures, so it is not the mip plumbing: `blitlin:75x37` differs in 2452 pixels and `blitcpu:75x37` in 75138, while the exact halvings `74x37`, `76x38` and `150x75` are all exact - and `blitlin:100x40`, ratio 2.5, is **also exact**, so the rule is not simply "non-exact ratio".  Not pinned down; the two failing mip cases are unlisted with the evidence rather than left green.  Every level `refract` samples is an exact halving, which is why its chain is exact |
| ~~`refract` renders a black frame although it skips no draws~~ | **resolved 2026-09-19** (`98be636`), and it was never the split path.  A split shader preprocesses one copy of the geometry per raster pass, so `m9_job_reserve` refuses a run of more than `ceiling/passes` records - but every producer sized its runs by the whole ceiling and, on the refusal, returned without a word.  refract's main pass is a 208998-corner mesh (bunny.obj, 69666 triangles) under a four-pass shader: the draw module handed it over in runs of 11520, **every one of them was refused**, and the frame carried the 156-record tail alone.  MEASURED with `M9X_SPLIT_TRACE=2`, which names every job: one split job a frame of 156 records against nineteen depth-pass jobs of 11520.  The black frame therefore **predates the padded path** - the 2026-09-18 build is black too, by refusing the same draw for coverage - so this was a scene that had never rendered, not a regression.  `m9_job_max_vertices()` is that ceiling in one place now and every producer uses it; a run that still does not fit is tallied as a skipped draw.  CAPTURED off scanout: 223965 distinct colours at 5 s and 194363 at 8 s against llvmpipe's 181662 on the same frame, and the known-good fixture renders after it |
| ~~**Correct `refract` costs 103 split jobs a frame, and that is now its whole cost**~~ | **Superseded 2026-09-19: the jobs were never the cost, the clear each of them took was.**  MEASURED at 1920x1200, 210.8 ms a frame and 5 fps at 130 jobs, and the new `be` profile phase says **82 of those 210 ms were the megabyte of VRAM the back-end object was cleared with, once a split job**.  With the clear sized by the extent the object actually uses and the record ceiling at 23040, `refract` is **128.0 ms and 8 fps at 70 jobs**, ioctl 74.7, be 16.1, varying gather 29574 KiB where it was 118983 -- no draw skipped, no stream timeout, the picture agreeing with llvmpipe to 1.5 of 255 over 8x8 tiles.  What is left is **not** submission: with `M9X_DRYRUN=1`, which validates the same stream in the kernel without executing it, the ioctl phase is **4.6 ms** where the live one is 75, so the 75 ms is the engine rendering and it does not move with the job count (81 at 130 jobs, 76 at 70, 83 at 55).  **refract cannot reach 60 fps by submission work: the GPU half alone is 13 fps**, and the next lever is the four raster passes themselves |
| `refract` spends 95% of its frame in a CPU transfer | MEASURED, `M9X_PROFILE=4` at 1920x1200: `1065.62 ms/frame: draw 8.05, submit 8.08, ioctl 7.25, transfer 1014.03`.  The scene calls `glGenerateMipmap` on its distance target once a frame; the driver implements no `generate_mipmap` and refused a target whose level is not zero, so the state tracker built every level on the CPU through the de-tiling path.  **Fixed** (`386e0ce`): 1 fps to **39 fps**, transfer 1014.68 to 0.52 ms, attributed on one binary with `M9X_LEVEL_TARGET=0` reproducing the old path.  A base level above zero turned out to be a **LOD clamp** rather than a second allocation, so pinning the view's low bound samples that level alone.  The scene is still **not conformant**: one draw a frame is refused by the partial-coverage gate.  Cost is proportional to that texture's area and nothing else (a 2.13x smaller target gives a 2.14x smaller transfer).  The refusal turned out to be **missing plumbing, not a storage constraint** - a level already is a render surface; what was absent was a byte offset on the colour relocation and a level in the framebuffer state.  The packed tail levels of 16x16 and below genuinely are not surfaces |
| `m9_fs_raster_88()` treats index `88` bits 31:24 as eight per-interpolant flags | They are not.  MEASURED: setting bit 30 for a seventh input made **input 4** read garbage as well, and setting all eight corrupted every input, while forcing the six-input mask `0x3f10dff0` on a seven-input pass left the first six exact.  The `BITFIELD_BIT(24 + k)` loop is correct only because varyings are capped at six, which is now explicit.  Raising that cap without decoding these bits would corrupt inputs the shader never touched |
| ~~`VS_ADD_RR`'s subtract form had its two operand fields swapped~~ | **resolved 2026-09-20.**  The form computes `a - c`; the vertex backend emitted `c - a`, so every register comparison, every `inot` and **both halves of a two-register `min`/`max`** were inverted - and `ior` is a maximum.  It had been shipping: `glmark2 conditionals:vertex-steps=5` ran 1157 native draws a sweep with inverted comparisons and reported a frame rate throughout.  No offline evidence could see it, because the gate's simulator held the same belief as the compiler.  The fragment backend already encoded it correctly (`N_SUB`).  MEASURED: `vsprim:minmax` wrong in 1172 and 1173 of 1200 cells before, 0 of 76800 pixels after |
| ~~**ioquake3's eight sine programs are refused one reason later, not run**~~ | **Answered 2026-09-20, and the length was the wrong suspect.**  The eight compile to **138 to 206 slots** and **68 to 82 constant pairs**.  MEASURED: the engine runs a **5161-slot** vertex program and renders every pixel of it exactly (`vschain:1024`), so there is no length ceiling within reach; what it will not take is the **constant span** -- 63 pairs is exact and 65 stops the card, reproduced on two programs and again with `M9X_VS_CONST_PAD` on a program that names only 27, so the resource is the span handed to indices `8c`/`8d`/`8e` and not the constants a program names.  `M9_VS_MAX_CONST_PAIRS` is 63 and refuses those programs now, which is what makes a raised length bound safe: at `M9X_VS_MAX_SLOTS=4096` the old driver handed the engine five spans of 68 to 82 pairs and got `two-pass job failed (Input/output error); context lost`.  `M9X_VS_MAX_SLOTS` is **188**, measured: the vertex stage costs about twice as much per vertex past a step between 186 and 191 slots, so seven of the eight are now inside the length bound and refused for their span alone.  The timedemo is **64.3 fps in both arms**, unchanged, because nothing moved.  `docs/parhelia-m9-vertex-isa-gaps.md` sections 1.7 to 1.9 |
| **ioquake3's `deformVertexes` programs need a constant span of 68 to 82 pairs and the engine takes 63** | **OPEN**, and it is now the only thing between ioquake3 and hardware T&L for those surfaces.  MEASURED: **44 to 54 of the 55 to 69 user pairs a program names are uniforms**, not literals.  Three routes, none measured: the appended tail owns pairs 9..21 unconditionally, so a user program has 51 of the 64 (a seventh of the file spent on a viewport transform); a pair holds two scalars and the operand swizzle is `0` for a broadcast and `1` for the two halves, so two broadcast constants could share a pair **if** a swizzle selects the second half alone -- that encoding is **not captured**; and a uniform the backend can inline through `M9X_VS_SPECIALISE` stops being a pair at all |
| **`fcsel_gt` is refused, so `x > 0.0 ? a : b` falls back** | The select instruction is there - `op2 = 0x1a`, class 4 CAPTURED on the vertex unit, class 3 is ">0" and class 1 ">=0".  Two cases in `build_alu` plus a hardware run.  Left open deliberately rather than added on offline evidence |
| `emit_rcp` with a literal operand returns 4x too large | worked around by folding literals at compile time; root cause open |
| ~~D16 depth attachments draw with no depth~~ | **resolved**: Z16 is now claimed for sampling only, so the state tracker falls back to `x8z24_unorm` and a depth attachment always carries the hierarchical surface |
| ~~A split draw that returns to an earlier render-target shape stops the engine~~ | **resolved**: the engine keeps per-target state in the object at index `be`, and a revisited shape read what another shape left there.  One `be` object per render-target shape.  Sharing by shape is what the engine wants - one per colour *resource* still stalls |
| ~~`desktop` and `terrain` stop the engine at scene scale~~ | **resolved 2026-09-19.**  It was none of the three suspects.  The driver cleared the engine's back-end state object with a CPU `memset` while a job that bound it was still executing: a job posted asynchronously retires only at the **next** submission (CAPTURED from the kernel), and the scratch path's seed copy is a draw into the very surface whose object is cleared next.  A race, which is why the count a context survived was never sharp.  A target shape now keeps two back-end objects and a clear swaps to the spare.  MEASURED, one binary, `M9X_SPLIT_BE=3`, `M9X_BE_SWAP` the only variable: `conv3rep` fails 76800 of 76800 pixels and stops the engine without it, passes three runs over with it.  `desktop` renders at 38 fps with nothing refused; `terrain` does not stop the engine either, and what refused it was the fragment backend, which no longer does (item 1 below) |
| ~~No picture when the machine boots without the BIOS VBE POSTing the card~~ | **resolved** (`7c7e80c`).  Two corrections came with it.  The display-controller enable bit was never the mechanism - `m9_program_timing()` already sets it, and the controller was enabled while nothing looked at it.  And this is not an unusual boot: CAPTURED from `captures/m9x-vendor-fourhead-20260917/`, POST always leaves the card on the VGA CRTC (`1c10 = 1`, VGA MISC `0x63`) and the vendor driver leaves `1c10 = 2`, MISC `0x68`.  `vga=0x318` merely hid it, because the BIOS mode set performs the same handoff.  Two registers decide it: `0x1c10` bit 0 selects the VGA raster, and the legacy VGA miscellaneous output register's bits 3:2 are the CRTC clock select, which POST leaves on the fixed **25.175 MHz VGA oscillator** - so with correct geometry the head rastered at **20.00 Hz** and writing `0x0e48` did not move it by a hertz.  MISC `0x68` gives the programmed clock.  MEASURED cold: 1280x720 at 60.00 Hz, line rate 45.0 kHz.  The monitor reporting "out of range" was the decisive datum - the DisplayPort MSA carries geometry only and read correct throughout, so no log would have shown it |
| `m9x_ddc_init()` compacts the PInS DDC map, so a head can read EDID from one socket and light another | this board reports `DDC map 09 0a 00 00 0d 0e 00 00` - populated pairs at positions 0 and **2**.  Compaction packs them into slots 0 and 1 and `head->ddc_index` becomes the head index, while `head->link` still comes from the bank table.  Whenever the map has holes, "DP-2" reads the third selector pair and drives pad 1.  A strong candidate for why outputs 2-4 have always behaved differently from output 1 |
| A module swap leaves the device returning `-EBUSY` for everything, and only a reboot clears it | distinct from a stream timeout: it follows an ordinary reload with no stall, and made one control run read as a failure of the change under test |
| `m9_reloc_uses_vram()` lists `PARHELIA_M9_RELOC_TEXTURE`, and the selftest at `m9_submit.c:1183` asserts it | contradicted by capture: the vendor binds `78 = 40c3a001`, a class-1 system GPUVA with the `|1` tag, 84 times.  The correct rule is the one stream slots `00..09` already use - **the address class is in the tag, not in the slot** - so index `78` should accept a VRAM base untagged or a class-1 GPUVA resolved as `bo->system.start + target_offset \| 1`.  Left unchanged deliberately: relaxing it widens what a submission may reference, and after the linear-2-D result the only use the evidence supports is a linear 1-D texture this driver cannot yet produce.  The **assertion** is the part that will outlive the evidence |
| ~~The X server holds three full-screen buffers of every GL client that has exited~~ | **resolved 2026-09-19.**  Not the X server: the m9x driver's shared-handle table (`m9_prime_import_put()`) freed its refcount entry without ever issuing `DRM_IOCTL_GEM_CLOSE`, so every dmabuf glamor imported for a DRI3 client stayed resident in the server's DRM file until the server exited.  The entry now owns the handle it created and closes it with the last reference.  MEASURED off the VRAM manager, one X session and five `build` clients: 24413, 31253, 38093, 44933, 51773 pages used before; 19853 after every one of the five now, and `build` reports 294-295 fps either way.  `tools/m9x-glmark-default-suite.sh` runs the whole default list in one session; `tools/m9x-glmark-default-suite-fresh-x.sh` is kept as the one-server-per-benchmark control |
| ~~glmark2 exits with rc=139 moving from the 15-tap `effect2d` scene to `shadow` in one process~~ | **resolved 2026-09-19.**  Ours, and a fixed-size array: `m9_vbuf_render::extra_offset` was declared for the CPU views of **one** fragment pass (`M9_FS_MAX_VARYINGS + M9_FS_MAX_TEXTURES`) with `struct util_dynarray indices` behind it, and a split variant has a view per interpolated input and per coordinate stream of *every* pass, so the emit loop wrote up to eight entries into that dynarray.  Nothing faults while the job runs; the corrupted array is freed when glmark2 destroys the scene's context, so the crash landed in `ralloc_free()` under `m9_render_destroy()` -- CAPTURED under gdb, the whole stack inside the driver.  The minimal pair is the 15-tap `effect2d` then **`pulsar`**, the next benchmark of the default list, not `shadow`.  `d14b711` sized the array `M9_FS_MAX_EXTRA` as part of the terrain work and closed it; DERIVED by bisecting on that pair, `22c0352` crashes and `d14b711` does not.  MEASURED: `glmark2 --size 1920x1200` now completes all thirty-three in ONE invocation, rc=0, no draw skipped, 0 stream timeouts, and `tools/m9x-glmark-default-suite.sh` runs it that way |
| ~~A framebuffer with **no colour attachment** has no raster parameters, so every draw into it is skipped~~ | **resolved 2026-09-19.**  Every raster parameter is derived from a colour surface (`m9_draw_params_init` builds the depth and HZ descriptors from the same shape), so a depth-only framebuffer now takes a surface of its own shape out of the scratch pool `m9_deferred_run` already allocates from, and the job masks colour writes off - which is what GL does with them, so nothing reads it.  MEASURED: glmark2 `shadow` goes from 1447 draws a frame skipped at 145 fps to **0 skipped**, and the frame rate to what drawing the shadow map costs |
| ~~A blended shader that names the whole register file has no temporaries for the colour lanes~~ | **resolved 2026-09-19.**  The blend path took its two saturated colour temporaries only from registers no slot of the program named at all, and freed the general ones after choosing them; a register whose last mention is at or before the slot writing its colour lane is dead from there on and carries the lane instead.  OFFLINE under `SRC_ALPHA/INV_SRC_ALPHA`: 133 of 182 corpus shaders supported to 146, all thirteen "out of temporaries for blending" refusals closed.  MEASURED: glmark2 `jellyfish` 1245 draws a frame skipped at 125 fps to **0 skipped** at 77 |
| ~~A fragment shader reads an integer uniform as the float Mesa stored~~ | **resolved 2026-09-19.**  The screen declares no integer support, so Mesa keeps an integer uniform in the constant buffer as a float (`uniform_int_float`); gallivm lowers the vertex path's NIR to match because the shader declares `no_integers`, and the fragment path did neither.  MEASURED with `M9X_FS_INLINE_TRACE=1`: `loop:fragment-uniform=true` receives `dw0=40a00000`, the float 5.0, where its `ige` wanted the integer 5, so the trip count was 1084227584 and the loop was refused as control flow.  `m9_fs_lower_integers()` runs `nir_lower_int_to_float`, and a UBO element index is put back with `f2i32` - without that the `matuni` fixture resolves constants past the bound buffer.  glmark2 `loop:fragment-uniform=true` 3446 draws a frame skipped at 345 fps to **0 skipped** at 159 |
| Two hardware lock files on the target do not exclude each other | The test box carries both `/tmp/m9-hw.lock` and `/root/tmp/m9-hw.lock`, and different tools in this tree take different ones (`tools/m9-mesa-glsl.sh` and `m9x-texenv-run.sh` take `/tmp`, `m9-mesa-glsl-suite.sh`, `m9-x-run.sh` and `m9x-reload.sh` take `/root/tmp`).  Three agents were on one card at once on 2026-09-19, each believing it held it: one agent's `glmark2` was found rendering on another's X server, halving its frame rates, and several stalls and "the card renders nothing" results turned out to be the overlap.  Until the two are unified, every hardware run has to take both, outermost first -- `tools/m9x-hw-lock.sh` does it -- and re-prove the engine with a known-good fixture **after** the measurement as well as before |
| A fragment pass with more constants than the engine holds stops the card | **bounded 2026-09-19.**  MEASURED with the `fsconst:N` fixture, which grows the constant count at a fixed slot count: `fsconst:24` (209 slots, **56** constants) renders every pixel exactly, `fsconst:30` (257 slots, **68** constants) fails all 76800 and logs a stream timeout.  A captured vendor pass carries 39 (`m9_fs_corpus.h` entry 189 pass 2), so the ceiling is between 56 and 68 and is not decoded.  `m9_raster_fs_init_textured` now refuses above `M9_RASTER_MAX_CONSTS` = 56, the highest measured-good count, so a shader in that band is skipped with a reason instead of stopping the engine -- MEASURED: `fsconst:30` now reports "a job could not be built" and leaves the card alive, where it used to stall it.  Splitting on the constant count, rather than refusing, is the remaining work |
| CPU-uploaded depth textures never reach the sampler | tiled surface, linear transfer |
| `glReadPixels(GL_DEPTH_COMPONENT)` returns the first map's value forever | pre-existing, not understood |
| First modeset above the firmware's mode logs `scanout publish failed (-19)` | the frame after is correct; the ordering is not |

## Display side

| feature | state |
|---|---|
| Extended desktop across heads | **implemented**: `m9_plane_window()` scans out a per-head window of a shared framebuffer, and `mode_config.max_width/height` are raised.  It is still built on `drm_simple_display_pipe` - that turned out not to be the blocker the note assumed; our own three checks were.  **Open:** whether a horizontal start offset addresses a *tiled* surface correctly.  Only 256-pixel and 64-row origins are pure translations, because tiled surfaces are stored in bands of 32 rows and the horizontal term is `x * 4 * 32` |
| Framebuffer console | **implemented**: stock `drm_client_setup_with_fourcc` + `DRM_FBDEV_TTM_DRIVER_OPS`, gated on `m9_kms_scanout`.  Default mode is the raster the card was POSTed with, or the EDID preferred mode on a head the firmware left dark; `m9_kms_mode="DP-1:1280x720"` pins it, below `video=` in precedence.  The buffer is a linear dumb buffer, so every publish re-tiles through the aperture: 395 ms a whole frame at 720p, 15.7 ms a console line.  `m9_fb_dirty()` honours its clip rectangles, without which the blinking cursor alone held a CPU at 100% system time |
| Dumb-buffer page flip cost | MEASURED ~440k pixels/s, a whole-frame de-tiling copy, so 0.19 fps at 1920x1200.  Direct scanout needs a tiled BO, which only the GL path allocates |
| DP++ dual-mode detection | early exit implemented; reading the adapter identifier over DDC would skip training outright |

## Needs capture before it can be implemented at all

Listed so they are not mistaken for ready work: anisotropic filtering; PBO;
and sRGB **sampling** - the write path is exact but the sampler decode was
never joined.

**Multisampling moved out of this list, and no longer needs a Windows
capture.**  The state is decoded and partly measured: the two dwords at
command indices `ad` and `af` are built by `m9_faa.c`, `af[8:0]` is the
sample coverage value in 1.8 fixed point with its invert, coverage-enable and
alpha-to-coverage flags at bits 9, 10 and 11, and the enable is a three-bit
mode field in index `ef` bits 31:29.  MEASURED on the M9128: modes 6 and 7
make the engine reserve one lane of every 4x2 pixel block and write nothing
into it, while alpha to coverage and a depth attachment each stop the engine.
What remains is not a capture but an allocation - the vendor's separate
multisample buffer group of about 16.55 bytes a pixel - and the command index
that binds it, for which `0xa1/0xa3/0xa4` are the named candidates and a
kernel whitelist change is the prerequisite.  Evidence, probes and the nine
answered gates are in `parhelia-m9-faa-re.md`.

**S3TC moved out of this list.**  The storage rules are decoded and are
executable assertions in `tools/m9-texture-resource-evidence.py`
(`verify_static_compressed_storage`).  DXT1 is hardware code `0x10` at 4 bits
per texel in storage class 7, DXT2/DXT3 are code `0x15` and DXT4/DXT5 code
`0x17`, both 8 bits per texel in storage class 6; DXT2 and DXT3 are the same
hardware format, and so are DXT4 and DXT5.  Descriptor dimensions are texels
rounded up to whole 4x4 blocks, allocation is
`padded_w * padded_h * depth * bits_per_texel / 8`, the block is the
addressing unit of the class-3 DXT calculators (which take *texel*
coordinates), and blocks upload verbatim - the hardware never decompresses.
`docs/parhelia-m9-s3tc-compressed-textures.md` is the implementation note.
**Implemented, MEASURED and advertised** for `PIPE_BIND_SAMPLER_VIEW` on
`PIPE_TEXTURE_2D`.  The decisive find is that the descriptor-translated
surface window **de-tiles a compressed level itself**, presenting it as a
linear block image, so no compressed address calculator was needed at all -
the driver reuses the path large uncompressed levels take.  Both remaining
questions closed: `79[15:0]` carries the **logical** width (`dxt1:132x64`
samples exactly with 132 in the descriptor against a span of 160), and the
sampler follows standard BC1 punch-through.  Two rules had to be measured
rather than derived: the storage kind is the one the level was *written*
through, and the window descriptor's height must reach a whole microtile - 16
texel rows for DXT1, 8 for DXT2..5.  And §4.3 of the note was wrong below
128x128: "exactly the block byte count, no decompression reserve" holds only
at 128x128 and above, while below it the bank swizzle spreads blocks over up
to eight times their content.  Not measured, so not advertised: render
targets, mip chains, cube faces, volume slices, sRGB variants, `79[24]`.

**10-bit GigaColor render and sample moved out of this list.**  `A2B10G10R10`
is hardware code `0x68` and `A2R10G10B10` code `0x6a`, both 32 bits per texel
in storage class 2 - allocated and tiled exactly like `B8G8R8A8`, so no new
layout code is needed.  Both take render class 0 at index `89` bits `3*s`, the
same class an 8-bit target takes; the per-target 16-bit write mask in `ab`/`ac`
must be `0xffff`, because that mask is a byte-lane mask and the vendor routes
10-bit channel masking through the fragment output program instead.
`docs/parhelia-m9-gigacolor-10bit.md` is the implementation note.
**Implemented, MEASURED and advertised** for `SAMPLER_VIEW`, `RENDER_TARGET`,
`SCANOUT`, `DISPLAY_TARGET` and `SHARED`: the sampler keeps ten bits, the
render target keeps ten bits over all 1024 levels of every channel, and blend
reads its destination at ten bits.  The note was wrong where it met the
hardware.  A clear to 682/1023 reading back 681 looked like a truncating
conversion; it is a **spatial dither** - 681 on exactly half the surface and
682 on the other - and `ef[7:0]` is `0x55` off / `0x00` on.  With it off the
conversion **rounds to nearest**.  The driver now programs `0x55` for a 10-bit
target, as the vendor's Parhelia path does.  Neither the dither nor the
rounding rule was in the note.  Still open: a **partial colour mask** on a
10-bit target is refused rather than applied, because `ab`/`ac` gate byte
lanes and the vendor's fragment-output lowering is not implemented; and a
**converting blit out of a 10-bit surface silently returns nothing** -
`glReadPixels` asks for a blit into an `R10G10B10A2` staging image, the driver
declines it, and the read comes back untouched instead of falling back.

**10-bit scanout is in scope.**  The M9 display
controller does know the format: `MxMM.sys 0x37b31` lists `0x6a` and `0x68` as
scanout surface codes, `0x368fe` packs the code into bits `[31:25]` of the
display surface descriptor, and `0x3dfd0` builds a ten-bit-per-channel gamma
ramp for code `0x6a`.  But both producers of the mode-record format mask in
`MxMD.dll` (`0x30352` and `0x105f0`, tables `0x30424` and `0x10704`) map a
Windows bit depth to `{1,8,0x10,0x20,0x40}` and reject everything else, so the
10-bit mode arm at `0x302d6` is unreachable, and the `GigaColorRender` /
`GigaColorFullScreen` registry switches that selected it on Parhelia were
deleted from this driver.  What is dead is therefore the **vendor driver's
mode enumeration**, not the silicon: this driver writes that descriptor itself
and enumerates its own modes, so the path is reachable here.  The remaining
unknowns are whether the DisplayPort transmitter needs its bit depth stated in
the MSA and whether the attached panel accepts 30-bit - both answered by
looking at the panel, not by more disassembly.  An earlier version of this
paragraph said "do not add a depth-30 visual", which mistook the vendor's dead
enumeration for a hardware limit.
**Implemented and MEASURED up to the controller; UNTESTED on the panel.**  The
plane advertises `XR24 XR30 AR30 XB30 AB30`, every modeset completes, the link
retrains, and the display-controller pitch word read *while the head is
scanning out* is `0xcc210780` for XR24, `0xd4210780` for XR30/AR30 and
`0xd0210780` for XB30/AB30 - codes `0x66`, `0x6a`, `0x68`.  GBM takes all four
for `SCANOUT|RENDERING`, six of twelve EGL configs are 30-bit, and a
GL-rendered ramp maps back with 1024 distinct codes across 1920 pixels with no
non-monotonic step.  EGL offers only the AR30/XR30 orders because Mesa's GBM
config list has no XB30/AB30 entry - a Mesa property, not a hardware one.  A
depth-30 **X** visual additionally needs the server and DDX at depth 30 and
was not tried.  What no instrument on this bench can answer is what the panel
makes of the pixels: link-format register `0x1c38` is patched from the PInS
table on pixel clock alone and reads `0x111d011d` identically for 8-bit and
10-bit modesets, so the transmitter bit-depth question is unresolved and needs
a human looking at the monitor.

**Float textures moved out of this list (partly).**  The vendor binds format
code `0x34` (`A32B32G32R32F`) at texture index `78/79` in
`captures/m9x-vendor-glamor-shaders-20260915/gradient-{linear,radial}-18`,
so a 128-bit float texel is a real sampler format, not a table entry.  What is
captured is one linear 1-D lookup with NEAREST filtering from system memory;
tiled placement and filtering of float texels are still uncaptured.  See
`parhelia-m9-texture-upload-and-copy-re.md`.

## Suggested order

Items 1-3 of the previous list - the third vertex attribute, the occlusion
query, and cube and 3D textures - are all done.

1. ~~**The scene-scale split fault.**~~  **Closed 2026-09-19.**
   ~~**`terrain`'s fragment shader.**~~  **Closed 2026-09-19 (second).**  The
   splitter partitions a shader by the resources a raster pass holds -- texel
   slots, the register file, the six interpolation registers -- and `terrain`
   goes from 24 refused draws a frame to **0**, with no stream timeout
   (MEASURED, default knobs).  `docs/parhelia-m9-fragment-pass-splitting.md`
   section 8.

   ~~**A computed-coordinate schedule past three taps renders wrong
   pixels.**~~  **Closed 2026-09-19 (third).**  The reproduction was a new
   fixture, `coordconv:N` in `tools/m9-mesa-glsl.c`: N taps at whole-texel
   offsets around a base coordinate the fragment stage computes, checked
   pixel by pixel against a CPU reference.  `coordconv:4` failed on 54092 of
   76800 pixels, **blue alone and always short**, by exactly
   `w * (red - blue)` of the one tap its two-unit producing pass weights at
   0.5.  Index 88 carries two fields over one range of bits, and from two
   bound units up a record-writing pass's flags land on the register pairs
   the engine may reuse -- marking that pass's own scratch free.  The
   allocator keeps those pairs empty; `coordconv:4`, `:5` and `:6` are exact
   and listed, fixture suite 228 of 228.
   `docs/parhelia-m9-fragment-pass-splitting.md` section 8.5.

   What that leaves is the **new top of this list**: **`terrain`'s picture is
   moved but not right**, and it is **two** defects, not one.  The scene's
   own options separate them (CAPTURED 2026-09-19 at 1920x1200, one X server
   a variant, against llvmpipe frames of the same variants taken on the build
   host):

   * `terrain:bloom=false:tilt-shift=false` -- the terrain drawn straight to
     the window, no offscreen target and no post-processing -- had the
     silhouette and the relief exact and the **colour wrong**.
   * The **striped blocks appear only with the tilt-shift blur**, the nine-tap
     pass, and are still open.

   **The `pointSpecular` half is closed (2026-09-19, third).**  It was the
   fragment **register allocator carrying its placement across compilation
   attempts** (`compile_output()` makes two): `reset_output()` and
   `reset_graph()` clear the lops and the nodes but not `c->values`, `allocate()` reset only `v->last`, and its free
   scan then matched the previous attempt's values and pushed registers it
   still held live values in back into the pool.  `pointLightColor[0] *
   uSpecularColor`'s xy lane sat in `0x1f` from slot 34 to slot 171 with
   sixteen later values handed the same register; its zw lane sat in the
   scratch register `0x13` and survived, which is why red and green came back
   saturated and blue carried the product.  `allocate()` now starts from no
   placement and refuses a pass in which two live ranges share a register;
   an interpolated input's register joins the general pool at its last read,
   which is what keeps the honest allocation inside the file.
   `M9X_RA_RESET=0` and `M9X_CHECK_RA=0` are the negative controls.
   MEASURED: the probe goes from 24712 of 76800 pixels differing to 470, and
   the scene's frame mean from 68.6, 43.6, 113.8 to **92.5, 89.6, 79.8**
   against llvmpipe's 95.0, 88.8, 31.7 -- an 8x8 tile difference of 21.9 of
   255 where it was 58.6.  `docs/parhelia-m9-fragment-pass-splitting.md` 8.4b.

   **What is left of the colour is a blue excess, and it is localised.**
   Every term of terrain's colour now matches llvmpipe on the card --
   the texture mix 2.9, `totalDiffuse` 7.3, `pointSpecular` 9.8 and the whole
   lighting sum 9.7 of 255 -- and the stock shader is 21.9 because of its
   last line, the **product of the record the texture mix is carried in with
   a vector the consuming pass computes**.  A record times a literal (1.7), a
   literal times a computed vector (7.6) and a record times a uniform product
   (2.0) are all right; a record times `totalDiffuse` is 9.6 and blue is 38.5
   against 22.6.  Narrowed to one component, `.x` is 4.2, `.y` 4.1, `.w` 8.6
   and **`.z` is 17.8** -- and the card's `.z` frame sits 3.7 to 4.4 from the
   `.y` frame, the distance a correct probe sits from its own reference.  The
   third component of a narrowed record comes back as the second.  Keeping
   all four components in the record makes it right again.  The next step is
   a fixture that writes a record of four traceable values and reads each
   back; a vendor capture of a producing pass with four different dwords
   would settle whether the producer or the consumer is at fault, and
   `m9_fs_corpus.h` holds none.
   `docs/parhelia-m9-fragment-pass-splitting.md` 8.4c.

   **Program size is not the cause** -- that reading is measured and retired.
   `fschain:40` is 337 slots and exact, and glmark2's own simplex noise
   compiles to a 1701-slot single pass in the same scene and draws correctly.
   **Index 88 is not the cause of the `pointSpecular` fault either**: the
   probe that renders exactly and the probe that does not compile to the same
   one-pass shape and carry the same `0f30030f`.

   **It has a fixture now.**  `filefs` reproduces it in seconds once the
   fixture's vertex shader stops letting the linker pack terrain's five
   varyings into two slots: a vertex shader whose five varyings all vary in
   every component gives four slots and an emitted fragment program
   byte-identical to the one terrain's own vertex shader produces.  Matching
   the schedule is not sufficient on its own -- the clobbered register has to
   be carrying something the frame shows -- so the regression test is
   `tools/m9-terrain-fixture-clobber.vert`, which separates 24712 from 470
   where `tools/m9-terrain-fixture.vert` gives 2145 and 2839.

2. **MRT is done** (2026-09-19) and is no longer on this list.  What it left
   behind, in rough order: an engine-side clear of the attachments past the
   first, which are filled on the CPU today; `ARB_framebuffer_object`, without
   which a mixed-format attachment set cannot even be asked about; blending,
   the alpha test and a discard for a multiple-target shader; and a splitter
   that follows more than one output cone.
3. **The projective divide defect** - before anything relaxes that refusal,
   because loosening the check without fixing the divide turns a refusal into
   a silently wrong picture.
4. **sRGB write** - the backend is written; advertising a colour format is all
   that stands between it and being reachable.
5. **A second vertex varying** is **stalled pending a new vendor capture.**
   Four shapes have now been refused on hardware: the pair mask at index
   `0x88`, the barrier between the two draws, the chain role in `ef`, and the
   packed record with its route-fed gather.  The last was emitted in the
   captured grammar in every field the RE decoded - `ef=00000200` head with
   `b1=ee190004`, `ef=00000600` gather, `ef=00288e00` fused - and the card
   still renders nothing.  The second preprocessing draw needs something that
   is in no capture this tree holds, so the next step is a **new vendor
   capture of a multi-draw chain**, not a fifth hypothesis.  Two things
   survive the negative result: no kernel change is needed, because
   `m9_index_allowed()` already admits `0x74..0x79` in a preprocessing range;
   and the tail-free head was never marking W1 of slot `n-4`, which every
   captured native program does.  With that mark the four-varying gather is
   byte for byte the vendor's own entry 39 of `vs-varyings-4`.
   The superseded history: three hypotheses were refuted on hardware: index `0x88`'s pair mask, a
   barrier between the two draws, and `ef` bits 11:9 as the draw's place in
   the chain.  The minimal case fails - `vary3`'s extra draw binds a *single*
   stream target and still stalls the engine.  The one lead left is that every
   captured chain-opening draw writes a **packed intermediate record** that a
   later route-fed draw de-interleaves, where ours writes separate
   `0x68002000` stream views.
   **The one-word declaration is settled and gave no ceiling raise.**  MEASURED:
   the hardware accepts `0688` for a general interpolant exactly, taking six
   inputs from 15 declaration words to 9 - but the ceiling is **six either
   way**, because the interpolation registers `0x40 + 8*i` are handed out in
   ascending attribute order and there are exactly six; `0x70` reads garbage.
   The projected rise to thirteen was wrong: the declaration budget was never
   what bound.  The win is that six interpolants now fit beside all four
   texture units, where the pair form left no words.  A seventh input fails
   **quietly** - every pixel written, no error, wrong values.
6. **The depth pair**: CPU-uploaded depth textures, and
   `glReadPixels(GL_DEPTH_COMPONENT)`.
