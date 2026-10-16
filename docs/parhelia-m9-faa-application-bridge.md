# Application bridge after the verified graph triangle

## Current hardware result, 2026-10-08

DRM36073bf/Mesa1468df5 pass32 real glxgears FAA frames at1024×768,
with GPU transforms,32 Unlocks and32 GPU presentations; finalaed0,
queue0, generation0, wedged0. No reference interior pixels are missing,
although shading differences remain. About1.4FPS in the unpaused portion.
[Evidence and screenshots](../tests/m9-faa-gears1024-hardware-20261008/README.md).
Quake3 GL1 FAA remains the next priority; older source-only scope below is
historical, not the current1024 rendering status.


The same-graph M9128 INIT → public DRAW → normal Lock producer now has real
FAA pixels. The isolated source-returned F021 view passes the strict pixel
oracle in `tests/m9-faa-normal-lock-view-20261007/check.py`, at RE
`9f13e594`. This proves the admitted constant-red 320×240 triangle. It does
not route Gallium draws or publish the image into an application's framebuffer.

This audit pins Mesa `e0d9262` and kernel `4d1b0a3`. File hashes and the final
17-object canonical DSO build are in
`traces/m9-faa-application-bridge-20261007.json`. No target was accessed and no
production activation changed.

## Current integration after verified second draw and Lock

Canonical DRM791ad1b and30c18a0 implement the retained second draw's
preprocessing, returned-DRAW rendering finish, and second normal Lock. Real
M9128 receipts now reach1e0,210 and240 on the same owned graph, with queue0,
wedged0, generation0. The new native triangle receives live XYZRHW/diffuse
records; the fragment program remains the fixed red diagnostic. Source proof
9133cfa0 establishes exact reuse of the3556-byte normal-Lock encoder after
recomputing eight addresses, five timeline words and two extents. Hardware
capsules are under tests/m9-faa-next-frame-tail-20261007 and
m9-faa-next-frame-lock-20261007. Their saved raw files are FIRST image only.
A separate guarded second F021 view at DRM8552a94 is now hardware verified:
real second image has14065 colored/528 partial pixels,4704 changed pixels,
zero interior/exterior/channel errors. Evidence and first/second PNGs are under
tests/m9-faa-second-view-20261007. These are bounded320x240 diagnostics.

Canonical DRM29cb6d4 reuses genuine Unlock after the observed second F021
view. The fresh-boot hardware receipt in
`tests/m9-faa-second-unlock-20261007` reaches260 with20 posts/12348 primary
bytes, queue0/wedged0/generation0, and both images byte-identical to the
validated prior run. This proves the second restore to writable state, not
a third draw or arbitrary continuous reuse.

Canonical Mesa23f0999 supplies the GPU integer-viewport position encoder and
private-clone GL depth adapter, computing `(z+w)/2` on GPU. Mesa789d4f5 joins
live four-vector programs, dense constants, current viewport and raw POSITION/
UV FLOAT4 records into address-free upload spans. Actual source-byte equality,
12 atomic refusals, five relative-field refusals and256 changed-input controls
pass under ASAN/UBSAN. SourceREb2553362/ab5f4056 proves the distinct public-VS
first-stage structure, including owner24 output and same-owner relative248f2.
Its originalf0/final100 path has not yet been submitted on hardware.
Canonical DRMdeb22e4 now supplies the separate typed public-VS ioctl0x16.
It validates the address-free COPY profile before consuming INIT, retains
the original SYSTEM mappings, installs six write fences, follows the source
program access order and stages64/64/112 primary bytes. Actual C admission,
ownership, failure and module checks pass. The helper accepts live adapter
output with strict byte lengths and independently observes retirement; the
semantic compiler producer and real100 hardware receipt remain pending.

The current combined18-object Mesa build passes, including manual compilation
of m9_faa_program.c (f4a5f9e fixes the earlier17-object build omission). The
helpers remain unactivated in production; linker GC can leave the DSO hash
unchanged even though the helper object is rebuilt. These host/build proofs
supply no application FAA or FPS claim. The remaining edges below still apply.

## Four concrete missing edges

1. **Prepared graph to application job.** `m9_faa_graph.c:117` allocates a
   separate sampled primary; lines 135–136 retain the application destination
   and depth references. Lines 187–192 pass the six prepared owners to the
   kernel. `m9_faa_surface_init_kernel` publishes `initialized` only after an
   actual INIT receipt. In contrast, `m9_context.c`'s job initializer still
   assigns `job.color` from `ctx->color`. Its FAA block populates the independent
   screen probe stores through `m9_faa_store_bind_job_locked`, never the six
   owners in `ctx->faa_surface`. There is no production assignment to
   `job.faa.graph`. `m9_draw.c:668`, 680 and 860 already distinguish graph
   descriptors and HZ from probes, but the application path never selects it.

2. **Application job to retained requester.** The context still sends every
   job through `m9_batch_submit` (or its ordinary aggregate path).
   `m9_requester.c:274` refuses `m9x_transport_arm_locked` once
   `faa_virtual_primary` owns the requester; the normal submission path calls
   this arm helper. INIT intentionally retains that owner. Changing a graph
   boolean cannot make normal M9_SUBMIT a valid continuation. A graph append
   must authenticate the exact file token, generation, retained six BOs,
   private pool/sentinel context, and actual preceding point, then use the
   retained virtual-primary endpoint and relative timeline. It must retain
   source CPU uploads, mappings and staged END ordering rather than importing
   a complete ordinary job's footer or restoring a physical requester slot.

3. **Live shader/vertices to the FAA preprocessing contract.** The current
   `GRAPH_DRAW_RED` ABI and `m9x_faa_graph_draw_submit` admit a bounded native
   program and geometry, not arbitrary Gallium input. Its continuation state
   rejects a previously claimed draw. The new `m9_faa_program_link` and
   `m9_faa_preprocess_moves` helpers have focused compiler tests but no
   production caller. The new bounded position encoder and GL depth adapter are available, while
   graph record bindings, shader admission and nondefault policies remain open. An application
   producer must join live declarations, linked C2 entries, constant spans,
   varying destinations and raster state to the source's preprocessing/raster
   split. Substituting a Gallium program handle into the fixed diagnostic
   template would not prove these interfaces. Keep unsupported texture,
   depth, blending, multiple-target and shader shapes refused until their
   producers and consumers are joined.

4. **Sampled primary to app destination and next draw.**
   `m9_faa_surface_prepare` retains `destination_offset` and `destination`,
   but there is no graph publication/resolve consumer of them in Mesa. The
   normal app target remains a different BO. Kernel F021 read admission is
   currently restricted to the exact retained primary, same generation,
   the first Lock's eight posts/INIT+0xc0 or the separately guarded second
   Lock's17 posts/INIT+0x190, and an idle queue. A clean Lock view
   is therefore a finite readback result, not scanout or Present. The source's
   normal Unlock changes the layout using a real GPU copy; the earlier
   same-backing F021→F022 and reverse-copy proofs are distinct lifecycle
   evidence. Both normal Unlocks now retire on this same graph. The diagnostic second draw and Lock now retire. The app path must still
   qualify its destination, overlap policy,
   retirement and source copy/presentation ordering, and transition back into
   a writable graph before another draw. Reading raw F022 as linear pixels
   or extending the old Lock admission point is not that transition.

## Smallest executable application slice

Extend the retained graph producer with a bounded, typed live draw request;
do not activate the raw FAA environment probes. Begin with one owned primary,
the source-qualified diffuse preprocessing interface, a live compiled constant
fragment output, GPU-transformed triangle vertices, single attachment and
explicit supported depth/raster policy. Supply BO identities, byte extents,
entry offsets, declarations and constants; derive all addresses and timeline
fields in the kernel. Preserve the established INIT binding and private pool
across the staged preprocessing, returned-DRAW finish and Lock/resolve phases.

First qualify that request's pixels through the existing source-returned F021
contract. Then qualify a genuine GPU publication to the retained application
destination plus the next-draw lifecycle. These are separate endpoints; an
INIT-only receipt cannot authorize them. The working fixed red producer is a
reference for source ordering and owner joins, not a generic fallback that
silently replaces the application shader or vertices.

The Mesa aggregate integration remains default-off and excludes active FAA
and initialized graphs. The new trace commit changes only window receipt
logging. Its combined full DSO compile passed with SHA
`ce224f540bb2cb93a07b57c9941dd9d5a69eb87caa74454d1ce598fe69abbb32`;
this is a build result, not an aggregate hardware performance measurement.
