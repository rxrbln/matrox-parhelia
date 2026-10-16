# M9128 wind-down handoff, October 7

The user requested that all agents wind down, code and documentation be
synchronized, and work stop. No further hardware test is authorized by this
handoff. FAA remains incomplete; the goal is paused, not achieved.

## Verified rendering and open boundary

The retained live FAA triangle path passes on actual M9128, PCI102b:0540,
revision01, subsystem22c0. INITb0 → HEADc0 → DRAW160 → Lock190 → cyan
readback → Unlock1b0 → changed retained DRAW/Lock280 → changed readback →
Unlock2a0 → token release/file close returns0. Both images pass the independent
pixel checks; the second contains12,495colored pixels and506partial edges.
Transport remains raw2a1, generation0, queue0, wedged0 after cleanup.
[Hardware capsule and screenshots](../tests/m9-faa-public-vs-next-20261007/README.md).

This proves two retained draws with the admitted shader/layout. It does not
prove glxgears, ioquake3, arbitrary shader or render state, continuous frames,
application presentation or an application FAA performance number. Those
remain required work before calling M9 FAA finished.

## Integrated code and source evidence

Canonical Mesa driver commits9302d74, d422c5b, 651600d and9128470 implement:

- Variable136-byte COLOR_LOCK uploads backed by one owned allocation with
  checked capacity, program entries, dense spans, raw records and semantic VS
  declarations. The client authenticates every returned input field and the
  exact64-bit final point, then independently waits. Two vectors use INIT+a0;
  four use INIT+e0. Failed attempts remain consumed until release/reset.
- COLOR Unlock through the existing40-byte ioctl, with an authenticated
  Lock+20 point and independent wait. Failed release retains all local state.
- A source-derived COLOR0 FS64 frontend and a lowered-NIR classifier supporting
  complete vector/scalar outputs and smooth pixel interpolation. Unsupported
  arithmetic, extra inputs, side effects and interpolation forms refuse.
- A live one-real-COLOR four-stage snapshot using the existing GPU carry,
  gather and position compiler. MVP/lighting and GL half-Z conversion stay on
  the GPU. No nonexistent second varying is manufactured.

The new one-stream four-stage launcher is experimental. The vendor's
four-stage control has two real VS outputs; the genuine one-color vendor
profile instead fuses a two-vector FS/VS. `native_admission` remains false.
The client/snapshot does not itself establish a usable application route.

Host evidence: actual client corruption/error/wait/refusal tests pass under
ASan/UBSan for both profiles. The fragment frontend matches original source
FS64/utility16 and passes12refusal controls. The live NIR binding passes vector
and scalar controls plus12refusals without mutating the NIR. The older
328-byte profile remains source-exact. All19driver objects and DSO link pass;
DSO SHA256 `dcec3f738ac0031a14244478dc99bc9e6a28c183098728c661c008cae803feab`.

The generated live shader fixture is exported as a136-byte zero-token/pointer
header plus828owned bytes in
`/tmp/m9-faa-live-color-export-20261007/color-live-upload.bin`, SHA256
`ee8f21b0709f4938188497ab342ef24db06eb09e0d97119c919a1a01c8eb27dc`.
Program entries0/64/184/216 select FS64/carry120/gather32/position200;
constants168, raw geometry224, stride32/count3, declarations5. It is a
representative generated GL shader fixture, not a captured glxgears draw.
It has not run on hardware. The runner must bind actual tokens and pointers.
The tiny upload/caller input and hash manifest are also retained in canonical
Mesa `tests/faa-live-public-upload-20261007/color-live-*` so temporary-file
cleanup does not remove the handoff's validation input.

Canonical RE commits5a741038,278365e0 and1e098f60 contain the original
one-color full DRAW/Lock source plan, COPY→ADD compiler controls and complete
two-real-output four-stage DRAW/Lock plan. The latest source plan reconstructs
the vendor bytes and two relocated owner/epoch placements;59address fields,
20timelines, four dimensions, three relative fields and stage metadata are
typed. Seven refusals pass. Final vendor0x150/0x190waits remain unread in
these CPU captures; they are not new GPU receipts.

COPY→ADD changes21of29fused VS slots. The ordinary tail matches only7of26
source slots. This does not provide a fixed generic fused MVP tail. Source
opcodes and independently derived instruction forms may be reused; captured
parameters/addresses may not. [Standing capture policy](gpu-capture-replay-policy.md).

Canonical DRM7106c67 integrates the sealed native draft39219d9. Its tree is
identical to the agent's compiled tree. The variable launcher derives owner
addresses, program entries, dense ranges, declaration packets and geometry
placement (ALIGN(actual constant bytes,128)), fences all six owners before
publication, and supports the typed F021/Unlock lifecycle. Four-stage mode
requires explicit `faa_color_four_experimental=1` and is gated to stride32:
twelve additional source stride fields need consumer proof before generalizing.
Graphs remain320×240. No program SHA is used as production shader admission.

Source two-vector and two-real-stream four-vector encoder checks pass at four
placements/epochs with nine atomic refusals. The generated one-stream upload
passes host grammar/mask checks only. Actual two-vector submit extraction
checks six fences, CPU mapper/readback, four posts, eight prewrite/eight stage
failures and readback refusal. Four-stage equivalent native failure coverage
and all new COLOR hardware pixels remain missing. These distinctions are in
canonical DRM `m9x/tests/faa-color-lock.md`.

The graph-profile module build passes; retained candidate
`/tmp/m9-faa-color-lock-winddown.ko` has SHA256
`305a3e0ff8b07e343de0db9213ee0a4014841fee3d33811cdd1d07c204f5f0f2`.
It was not deployed. Both required firmware templates are tracked in the RE
captures and must be installed by any future matched runner:
`m9x/faa-graph-color-lock-template.bin` (5004bytes,SHA256
`560f8bebb7c96b2227d5e16308e5fe889710887c273e38437fbe5966b20231c4`),
and `m9x/faa-graph-color-four-template.bin` (6408bytes,SHA256
`a7c8b9a2b69f0c519a3b76d4b6434db8eec2600f4b8936506306c85827805743`).

## GL2 hardware failure retained

Canonical Mesa evidence commitsf3d416c and5237521 archive one unchanged
1024×768 implicit/multipass fixture run. It used Mesa sourcea295717,
DSO SHA256 `6a7fc10ae1b7c962188bafdc3362204b1fe0a42ce4ab68690d3ab54fbb6a01ae`,
ordinary module SHA256
`0ceef5d0fb02c958835d66802a42659ab9cf14d43299587784714dd3cae81881`
(source59bdbe0+40f8644), and boot
`ec815b9d-5e53-4675-a2b2-3ad00a64a042`.

The GPU reached completed0x460/raw0x461, short of submitted0x520;
queue1/wedged1 persisted immediately and two seconds later. Readback reported
a GL error; fixture exit2, runner exit1. No pixel or FPS qualification follows.
The first pair retires0x3a0; second JOB6retires0x460, then JOB7fails before its
first0x470dependency publication. Successful/failing command bodies, C2,
declarations, indices and run partitions match; live resource state differs.
The causal mechanism remains unresolved. No speculative production fix was
made. All useful implementation code and the strict original oracle remain;
the runtime gate is default-off.

The archive checker passes hashes, exact eight-job timeline joins, complete
constant/input partitions and the recovery receipt. This verifies the failed
evidence, not rendering success. See canonical Mesa
`tests/native-gpu-padding-runtime-proof/hardware-20261007/README.md`.

## Target and remaining work

Recovery used NanoKVM power-off/power only, without LED status. Final observed
boot `e98c7106-f10b-4680-a5ea-a71af43c0b4e`, kernel7.2.5-t2, same M9128;
`m9x_drm` is absent. Recovery execution ended0 and all host/remote locks were
released. No further target mutation occurred during wind-down.

Before resuming FAA application work:

1. Review the sealed native COLOR draft and its exact gates/build results;
   revalidate the target and acquire every established lock before deployment.
2. Qualify the generated live COLOR shader's complete draw, pixels, F021 read,
   Unlock and cleanup. The one-stream routing must be independently proven.
3. Wire actual application draw state, raw input/topology, shader/CB0 changes,
   repeated draws/frames, depth/textures and presentation. A single representative
   shader fixture cannot substitute for full glxgears/ioquake3 rendering.
4. Keep GL2 diagnosis separate; resolve the archived second-draw failure before
   promoting the multipass runtime gate. No new GL2 throughput is established.

Unrelated preexisting edits in RE WIPs were left untouched, including
`wip/m9-faa-position-re/tools/m9emu/m9_faa_graph_public_vs_continue.py`.
All three agents stopped. Their owned worktrees and the canonical code trees
are clean; no test process or target lock is retained.
