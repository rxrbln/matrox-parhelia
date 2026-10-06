# P690 implementation checkpoint — 2026-10-06

Current implementation and native evidence are maintained in the Mesa
[P690 status/TODO](../../wip/gpu-features/mesa-driver/docs/p690-current-status.md).
This checkpoint supersedes older open-driver status claims in the historical
static audits; their binary disassembly remains useful evidence.

The exact tested board is P690 `102b:2539`, revision1, 128MiB. The integrated
Mesa runtime build is `83b6b1d` (later edits correct comments/docs); kernel
runtime is integrated as `a7d3429`. Full build/CPU checks and the native controls
below pass. The target runs the isolated final renderer on fresh boot
`c98fefd3-9d66-42db-b28d-bb75045c2ba8`. See the linked status for artifact hashes,
retained proof directories and hardware-lock/recovery rules.

| Topic | Current result | Open boundary |
|---|---|---|
| FAA textures/depth | Earlier Quake3 matched gameplay images; this round's CPU/GPU mixed fixture passes90pixels/three draws/one resolve | Last13.63FPS is a short prior capture, not a new complete timedemo or long-run guarantee |
| FAA GPU clipping | Finite near/inside/side control passes102pixels with three actual24-DWORD GPU sources and no CPU tail | Default-off diagnostic, eight-DWORD profile only |
| Ordinary large batches | Default-off complete P690 graph passes single GPU FETCH4triangles and CPU FETCH8triangles with texture/depth checks | Nine-vertex trap still splits a three-triangle GPU range2+1; original/LX compatibility limits unchanged |
| Ordinary command termination | Initialized genuine vendor AA0 XYZ8/10 captures retain the same eleven program bodies/eight constant blocks as FAA profiles | Preserve ordinary raster state; Linux a60ec4 tagged307 is accepted only for fully qualified ordinary requests |
| Filter shaders | Released five/nine/desktop H/V controls pass GPU readback, alpha, depth and cleanup; each plan uses three distinct instruction images | Actual glmark2 scratch pitches still mismatch sampler pitches and replay is refused |
| Performance | Full default Quake3 timedemo35.1FPS; opt-in ordinary25.8FPS,1024x768/vsync0 | Keep the slower feature default off; reduce logical-graph publication cost without weakening ordering |

The current sampler pitch rule is `MAX(32, align(width,32))`, not a power-of-two
rule. Actual glmark2 validation targets have width800/RTstride832 and
width210/RTstride256; sampler expectations800/224 disagree. This blocks owned
scratch aliases. The four passing1024x768 controls do not establish complete
glmark2 application rendering. Exit0 and ValidationUnknown are not pixel proof.

Original Parhelia/LX and M9 findings stay separate. This P690 round does not
establish M9 FAA, complete Quake3 GL2, 10-bit textures, general multisample
framebuffer support, or unrestricted shader coverage.
