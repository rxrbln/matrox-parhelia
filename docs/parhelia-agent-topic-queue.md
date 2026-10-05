# Dedicated GPU TODO agent queue

User requested a background agent per remaining TODO topic. The available
concurrency is root plus three background agents. Each queued topic receives
its own focused agent turn as a slot becomes free; queued rows are not running
agents. Preserve coherent signed milestones before rotating a slot. Root
reviews/integrates commits and coordinates target access. Full glmark2 and Q3
GL2 rendering take priority; keep the GL1>40/full GL2>15 goals intact.

| Topic | State / owner | Required next evidence |
|---|---|---|
| Q3 GL2 runtime stage graph / scratch links | Finishing round: native_geometry actual chain construction | Executable original-NIR stages and linked consumer, final depth/blend/discard/order |
| Normal maps / dependent texture resources | Host cache milestone integrated 07929c0; native proof pending | Cache ownership/failure proof, real GPU repack bindings, filtering precision |
| glmark2 FragCoord scenes | Host milestone integrated9ece93a; native window test held | Real conditionals/function/loop compiler paths and pixel oracle |
| Q3 GL1 batching / submission performance | 37.0 FPS measured on7a52cb4;37vs36.7 within one-run noise | Remaining drains/capacity gaps; one matching1024x768 run when authorized |
| Fullscreen presentation / double buffering | Host milestone integrated7a52cb4; target proof pending | Pending/ready render-fence flips, pixels/display identity, async lifetime |
| Fragment BRDF / nonlinear math | Dependency graph integratede4f41fa; executable BRDF kernels pending | Executable normalization/division/pow/exp2 stage kernels, retained exact effects |
| Vendor-driver ground truth | Seven captures integratedd6a9ce6; further RE queued | Established Windows emulator captures for unresolved coordinate/filter/TA semantics |
| Depth comparisons / shadow / Piglit | Uniform GPU depth dcd7af5/00c9070 host proof; sampled-depth gap open | Actual sampled depth comparison and FragCoord depth paths; focused current failures |
| Geometry acceleration | Safe GPU prefixes54b3b05 host proof; native crossings open | Polygon offset, crossing user planes, point-size/topologies, GPU transform preservation |
| Texture coverage | Perspective BC1 oracle42f2c30 host proof; native proof open | Remaining S3TC shapes/filter footprints, perspective anisotropy, GigaColor/system fetch proof |
| Original-family LOD bias | Queued: lod_bias | Current -4 clamp root cause and actual failing GL test |
| P690 sampled alpha | Affine guard193ad43 host proof; full sampled alpha open | Spatial companion failure evidence and correct accelerated path |
| P690 larger packets | Vendor range proofb8923e9; larger native draws held | Established safe two-primitive boundary, host/vendor bounds before any held-risk GPU test |
| Kernel geometry BO bounds | Late target identityec20aff host proof; extent bounds open | Current destination extent/relocation validation evidence; no inherited exploit claim |
| Recovery / multiple heads / cursor | Cursor active-outputa51792f host proof; dual-head/reset open | Actual reset mechanism and placement/display ownership contracts |
| FAA extension / regression / throughput | Queued: faa_support | App control semantics, occasional correct1024 test; main rendering stays FAA off |

Every agent owns an isolated checkout and names edited files before overlapping
work. Commit major progress with DCO signoff; retain correct features and
optimizations even if one benchmark is flat. Prefer GPU transforms. Use one
short benchmark per coherent change, not repeated numerical controls.

Every target GPU/module workflow must hold /tmp/parhelia-hw.lock with bounded
execution and preserve/restore known-good DRM/X. The explicit existing upload
and spatial-test approval holds remain in force; this delegation request does
not approve their transfer/execution. No two agents run target tests together.
Host evidence, native pixel proof and performance measurements are distinct.
