# Dedicated GPU TODO agent queue

User requested a background agent per remaining TODO topic. The available
concurrency is root plus three background agents. Each queued topic receives
its own focused agent turn as a slot becomes free; queued rows are not running
agents. Preserve coherent signed milestones before rotating a slot. Root
reviews/integrates commits and coordinates target access. Full glmark2 and Q3
GL2 rendering take priority; keep the GL1>40/full GL2>15 goals intact.

| Topic | State / owner | Required next evidence |
|---|---|---|
| Q3 GL2 runtime stage graph / scratch links | Running: native_geometry | Executable original-NIR stages and linked consumer, final depth/blend/discard/order |
| Normal maps / dependent texture resources | Running: texture_hardware | Cache ownership/failure proof, real GPU repack bindings, filtering precision |
| glmark2 FragCoord scenes | Running: shader_gaps | Real conditionals/function/loop compiler paths and pixel oracle |
| Q3 GL1 batching / submission performance | Queued: gl1_performance | Remaining drains/capacity gaps; one matching1024x768 run when authorized |
| Fullscreen presentation / double buffering | Queued: presentation | Pending/ready render-fence flips, pixels/display identity, async lifetime |
| Fragment BRDF / nonlinear math | Queued: fragment_brdf | Executable normalization/division/pow/exp2 stage kernels, retained exact effects |
| Vendor-driver ground truth | Queued: vendor_re | Established Windows emulator captures for unresolved coordinate/filter/TA semantics |
| Depth comparisons / shadow / Piglit | Queued: depth_shadow | Actual sampled depth comparison and FragCoord depth paths; focused current failures |
| Geometry acceleration | Queued: geometry_features | Polygon offset, crossing user planes, point-size/topologies, GPU transform preservation |
| Texture coverage | Queued: texture_coverage | Remaining S3TC shapes/filter footprints, perspective anisotropy, GigaColor/system fetch proof |
| Original-family LOD bias | Queued: lod_bias | Current -4 clamp root cause and actual failing GL test |
| P690 sampled alpha | Queued: p690_alpha | Spatial companion failure evidence and correct accelerated path |
| P690 larger packets | Queued: p690_packets | Established safe two-primitive boundary, host/vendor bounds before any held-risk GPU test |
| Kernel geometry BO bounds | Queued: kernel_bounds | Current destination extent/relocation validation evidence; no inherited exploit claim |
| Recovery / multiple heads / cursor | Queued: recovery_display | Actual reset mechanism and placement/display ownership contracts |
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
