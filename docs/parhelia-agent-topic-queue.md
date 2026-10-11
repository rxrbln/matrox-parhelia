# Dedicated GPU TODO agent queue

User requested a background agent per remaining TODO topic. The available
concurrency is root plus three background agents. Each queued topic receives
its own focused agent turn as a slot becomes free; queued rows are not running
agents. Preserve coherent signed milestones before rotating a slot. Root
reviews/integrates commits and coordinates target access. Full glmark2 and Q3
GL2 rendering take priority; keep the GL1>40/full GL2>15 goals intact.

| Topic | State / owner | Required next evidence |
|---|---|---|
| Q3 GL2 runtime stage graph / scratch links | Corrected actual GLSL link/uniform/unary stages5fd90f8 integrated; native_geometry implementing scoped P690 normal stage | Actual full GLSL frontend/compiler failures, executable stages/linked consumer, final depth/blend/discard/order |
| Normal maps / dependent texture resources | P690 nearest dependent AR→helperXY nativePASS35b6ed0; scoped RG→AR normal stage in progress | Native repack/cache generation/lifetime and original normalZ-stage result; continuous precision still open |
| glmark2 two-coordinate lookups | Sparse logical1/physical0+1 nativePASSa46eeea, four full frames/depthorders/generations; texture smoke139FPS726dab2 | Larger blur kernels and remaining full scene rendering |
| glmark2 FragCoord scenes | Host milestone integrated9ece93a; native window test held | Real conditionals/function/loop compiler paths and pixel oracle |
| Q3 GL1 batching / submission performance | 0528 37.0 FPS; P690 latest37.9 FPS with directPresentae6f666, prior38.1 ringfix,1024x768 FAAoff/vsyncoff | Remaining drains/capacity gaps; one matching1024x768 run when authorized |
| Fullscreen presentation / double buffering | Direct tiled-render→linear copyae6f666 retained; Q3single37.9FPS approximatelyflat | Pending/ready render-fence flips, pixels/display identity, async lifetime |
| Fragment BRDF / nonlinear math | Actual LUT/storage counterexamples6ec0447; signed/HDR scratch unavailable, unsupported numeric targets reject8fae6fc | Executable normalization/division/pow/exp2 stage kernels, retained exact effects |
| Vendor-driver ground truth | Genuine PSER firmware/D3D initialized; valid dependent738ecbc/native35b6ed0 and FAA bind/resolve6cc7050/partitionf72cac5 | Established Windows emulator captures for unresolved coordinate/filter/TA semantics |
| Depth comparisons / shadow / Piglit | Uniform GPU depth dcd7af5/00c9070 host proof; sampled-depth gap open | Actual sampled depth comparison and FragCoord depth paths; focused current failures |
| Geometry acceleration | Safe GPU prefixes54b3b05 host proof; native crossings open | Polygon offset, crossing user planes, point-size/topologies, GPU transform preservation |
| Texture coverage | Perspective BC1 oracle42f2c30 host proof; native proof open | Remaining S3TC shapes/filter footprints, perspective anisotropy, GigaColor/system fetch proof |
| Original-family LOD bias | Queued: lod_bias | Current -4 clamp root cause and actual failing GL test |
| P690 sampled alpha | Affine nearest nativeGLPASS eb94dd1; perspective3d02165 executes, strict32/786432 mismatches per phase3d494d2 | Perspective boundary precision, linear/mips/live scenes; defaultoff |
| Shared original/P690 larger packets | Corrected renderer86878af/kernel35980e2/prefixfix9d51ff4 retained; Q3stillWEDGEcd40781 | Genuine coherent-DMA class3 and multiple ranges before one final drain proved500101d/162fb85; single24vertex float9 farW PASS35b6ed0; five-draw additive/depthwrite/texture state-edge replay in progress; defaultoff |
| Kernel geometry BO bounds | Late target identityec20aff host proof; extent bounds open | Current destination extent/relocation validation evidence; no inherited exploit claim |
| Recovery / multiple heads / cursor | Cursor active-outputa51792f host proof; dual-head/reset open | Actual reset mechanism and placement/display ownership contracts |
| FAA extension / regression / throughput | Active depth_shadow: genuine captured-ROM SPIbootstrap8e813d2 opens D3DScene; actual FAA allocations/binds/resolve/32MiB partitions f72cac5 grounded; isolated kernel placement patch in progress | Native allocation separation/extent bounds and hardware FAA rendering; baseline FAA off |

Every agent owns an isolated checkout and names edited files before overlapping
work. Commit major progress with DCO signoff; retain correct features and
optimizations even if one benchmark is flat. Prefer GPU transforms. Use one
short benchmark per coherent change, not repeated numerical controls.

Every target GPU/module workflow must hold /tmp/parhelia-hw.lock with bounded
execution and preserve/restore known-good DRM/X. The explicit existing upload
and spatial-test approval holds remain in force; this delegation request does
not approve their transfer/execution. No two agents run target tests together.
Host evidence, native pixel proof and performance measurements are distinct.

P690 swap completed and work resumed by user instruction. Current healthy boot
0ebb2b54-089f-4809-bed2-b1583eade414; historical first-swap boot
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
