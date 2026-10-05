# Dedicated GPU TODO agent queue

User requested a background agent per remaining TODO topic. The available
concurrency is root plus three background agents. Each queued topic receives
its own focused agent turn as a slot becomes free; queued rows are not running
agents. Preserve coherent signed milestones before rotating a slot. Root
reviews/integrates commits and coordinates target access. Full glmark2 and Q3
GL2 rendering take priority; keep the GL1>40/full GL2>15 goals intact.

| Topic | State / owner | Required next evidence |
|---|---|---|
| Q3 GL2 runtime stage graph / scratch links | Host chainf25811d complete; kernels/precision/native proof open | Executable original-NIR stages and linked consumer, final depth/blend/discard/order |
| Normal maps / dependent texture resources | Host cache milestone integrated 07929c0; native proof pending | Cache ownership/failure proof, real GPU repack bindings, filtering precision |
| glmark2 FragCoord scenes | Host milestone integrated9ece93a; native window test held | Real conditionals/function/loop compiler paths and pixel oracle |
| Q3 GL1 batching / submission performance | 0528 37.0 FPS; P690 38.1 FPS with ringfixa57fbfb,1024x768 FAAoff/vsyncoff | Remaining drains/capacity gaps; one matching1024x768 run when authorized |
| Fullscreen presentation / double buffering | Host milestone integrated7a52cb4; target proof pending | Pending/ready render-fence flips, pixels/display identity, async lifetime |
| Fragment BRDF / nonlinear math | Dependency graph integratede4f41fa; executable BRDF kernels pending | Executable normalization/division/pow/exp2 stage kernels, retained exact effects |
| Vendor-driver ground truth | Seven captures integratedd6a9ce6; further RE queued | Established Windows emulator captures for unresolved coordinate/filter/TA semantics |
| Depth comparisons / shadow / Piglit | Uniform GPU depth dcd7af5/00c9070 host proof; sampled-depth gap open | Actual sampled depth comparison and FragCoord depth paths; focused current failures |
| Geometry acceleration | Safe GPU prefixes54b3b05 host proof; native crossings open | Polygon offset, crossing user planes, point-size/topologies, GPU transform preservation |
| Texture coverage | Perspective BC1 oracle42f2c30 host proof; native proof open | Remaining S3TC shapes/filter footprints, perspective anisotropy, GigaColor/system fetch proof |
| Original-family LOD bias | Queued: lod_bias | Current -4 clamp root cause and actual failing GL test |
| P690 sampled alpha | Raw mask/depth-order PASS within1LSB; GL fallback exact; native admission tracing bfa089c | Actual route/primitive rejection trace, then native GL pixels and sustained Q3 cutouts |
| Shared original/P690 larger packets | Corrected renderer86878af/kernel35980e2 integrated; raw pixel PASS; sustained Q3 WEDGE561dc2c | Investigate actual failed aggregate/publication/lifetime; candidate remainsdefaultoff, no FPS claim |
| Kernel geometry BO bounds | Late target identityec20aff host proof; extent bounds open | Current destination extent/relocation validation evidence; no inherited exploit claim |
| Recovery / multiple heads / cursor | Cursor active-outputa51792f host proof; dual-head/reset open | Actual reset mechanism and placement/display ownership contracts |
| FAA extension / regression / throughput | Active depth_shadow: genuine SUN90 surface PASS; cached clock producers534d68f, D3Dgateclosed | Genuine memory-clock/FAA heap/bank evidence and native FAA proof; baseline FAA off |

Every agent owns an isolated checkout and names edited files before overlapping
work. Commit major progress with DCO signoff; retain correct features and
optimizations even if one benchmark is flat. Prefer GPU transforms. Use one
short benchmark per coherent change, not repeated numerical controls.

Every target GPU/module workflow must hold /tmp/parhelia-hw.lock with bounded
execution and preserve/restore known-good DRM/X. The explicit existing upload
and spatial-test approval holds remain in force; this delegation request does
not approve their transfer/execution. No two agents run target tests together.
Host evidence, native pixel proof and performance measurements are distinct.

P690 swap completed and work resumed by user instruction. Boot
49b5213d-00d4-43fe-a0d8-79dd41691aa9, PCI01:00.0 102b:2539rev01,
128MiB BAR0; known0f7d564 warmups/preflight and X start pass. One bounded
Quake3 GL1 run33.1FPS (387frames/11.7s),1024x768 fullscreen uncompressed/aniso1,
FAAoff/vsyncoff, deployed7a52cb4/core48cd2d9, conservative2primitives/packet.
Main integrates native target-class scratch allocation and ringfixa57fbfb;
combined overlay/full CPU suite pass. Raw and Gallium same-BO nearest/linear proofs pass; native_geometry is
preparing scoped admission/fullGL2 work. geometry_features traces genuine allocation
lifetime after vendor rollover evidence. depth_shadow retains alpha mask/RGB
failure evidence and investigates requested P690 FAA. Resolve018312e and trackingaff05c4
are vendor-grounded; bank placement/admission/native FAA remain open. Both kernel and Mesa
currently exclude2539 FAA; ordinary fallback is not native FAA validation.
Root owns all target tests. Specific pre-existing approval holds unchanged.
