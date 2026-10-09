# Peer source and progress inventory — 2026-10-09

The current source trees and latest campaign log supersede the October 8
handover. This inventory records a filesystem inspection; a worktree or log
alone does not prove an agent is still running.

## Canonical trees

| Tree | HEAD | Inspection |
| --- | --- | --- |
| `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/m9x` | `e565872` | Also includes live BO admission, compact shader schedules and bounded search |
| `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm` | `b1b3cf2` | Coherent command backing and ownership comment; behavior matches tested candidate |
| `xf86-video-parhelia` | Includes October 9 topic evidence | Progress log is newer than the general handover/status headers |

## Peer work remaining to reconcile

Paths below are under `/home/ai/tmp/agents`. Preserve uncommitted work; it
has not been promoted merely because it exists.

| Worktree | HEAD / branch | Current state and next proof |
| --- | --- | --- |
| `m9piglit3-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `a6e0cb7`, `undef` | Two dirty files: constant folding and undef lowering, plus temporary dumps. Not integrated |
| `m9unify6-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `c0c1be2`, `m9fbo9` | Five dirty files completing sRGB constant blending, target masks and non-native draws; default-on change awaits its extended proof |
| `m9unify-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `fed13b9`, `m9logic` | Four dirty files in context/draw/native plumbing. Older branch; preserve separately |
| `m9glperf-drm-wt` | `df0e59a`, `cmdcached` | Clean committed command-BO coherent backing, based on canonical `ca1ee48`. Integrated as `9d854ea`; locked cache A/B flat (map 55→56, subdata 56→55 FPS); retained |
| `m9fbo5-drm` | `927f76a`, `m9fbo5` | Clean S3TC small-kind validator patch; not in main; previous notes explicitly leave validator admission pending |
| `m9fbo5-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `f3769f1` | Clean; record texel-pair fix already integrated as `97845af` |
| `m9q3c-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `e75ec3b` | Clean older snapshot; current main has later compiler fixes |
| `m9unify6-drm` | `0982baa`, `m9unify6-shadow` | Clean; FAA context restoration is integrated but application proof remains open |

The older `wip/m9-gl2-*` trees are clean October 7 snapshots. They are
useful history, not current integration candidates without comparison to main.

## Latest recorded evidence

[Campaign log](../../campaign/campaign-log.md), through October 9 at 22:47:

- Core Piglit: 284 PASS / 1 FAIL, with one timeout and one warning. The
  remaining core failure is the documented depth-code rounding difference.
- The latest execution subset gained 71 passes; the transient not(bvec)
  regressions were fixed. The preceding undef-index fix gained another 21.
- A 33-scene glmark2 A/B found no material regression; effect2d 3x3 improved
  from 88 to 95 FPS. Terrain's blue cast is fixed; a record-consumption
  speckle probe remains open.
- FBO subset reached 234 PASS / 4 FAIL. sRGB target support reached 76 PASS /
  6 FAIL with opt-in; default-on remains pending extended controls.
- Quake3 GL1 has correct weapon geometry at approximately 50 FPS. GL2's
  submission stall is fixed, but floor/weapon colour correctness remains open.
- Full Quake3 FAA remains unqualified. Its latest combined application and
  context-restoration group was not run; triangle/program probes do not
  establish complete gameplay support.

The current M9128 target inspection found no Xorg or ioquake3 process.
Boot `56289a98-ba5e-4df9-9b1a-ca50f2acfd9f`, kernel `7.2.5-t2`, transport
submitted=completed `0xdb8dff0`, generation 0, queue 0, async 0, wedged 0.
The promoted module stage identifies DRM `ca1ee48` and build ID
`c6697c086d2cfd8da0870fadb74a2526be0f9a37`. Target prefix names alone do
not prove which Mesa revision an application loads; verify the DSO hash.

## October 10 integration and hardware proof

Mesa `67cfae0` gates colour-specific output templates away from float records and resets retry route ownership. `c7196c7` distinguishes interpolation pressure, retries earlier cut choices and bounds the search. Both are committed with production compiler sanitizer/reference controls.

The combined DSO SHA `7ed5a8301348469ba03ea3558e74d738271438ab61969db0efad8fcf21acaee2` rendered ten exact GLSL fixtures on the unchanged boot and promoted DRM `ca1ee48`: floor187 now renders in 16 passes, weapon199 in 5; both have zero pixels differing from llvmpipe by more than 8 byte levels. All eight controls pass the same bound, with facing controls exact. No skipped draws, queue or wedge. [Complete proof](../tests/m9-gl2-fixed-20261010/README.md). Full gameplay remains a separate check.

Command-cache A/B used the same baseline Mesa `a6e0cb7` and candidate module for both modes, once each at 1024x768 / three seconds / vsync off. Map 55→56 FPS and subdata 56→55 FPS are flat within one-run precision. The change is retained; canonical module build is byte-identical to the tested module. All four captures were visually reviewed and target health stayed clean. [Proof and runner caveats](../tests/m9-command-cache-20261009/README.md).

Opt-in sRGB now passes all24 extended GPU phases (live colours, lines and mixed MRT masks) on Mesa `e7efe22`; [complete proof](../tests/m9-srgb-hardware-20261010/README.md). Default remains held for current sRGB/core regression selections.

The prior full-game BO-capacity failure is documented in [historical capacity evidence](../tests/m9-q3-capacity-20261010/README.md). Mesa `e565872` fixes admission from live owners; full configured gameplay now completes387frames at42.0FPS without refused/skipped draws or context loss. Matched software screenshots agree within8 byte levels for99.96%/99.60% of pixels. GPU stays healthy on the unchanged boot. Advanced effects remain disabled; full-feature GL2 is open. [Complete game proof and scope](../tests/m9-q3-game-fixed-20261010/README.md).

Compact floor scheduling now needs9passes/6slots, and all ten images are byte-identical to the preceding successful GPU captures. Search fixes preserve4PASS/20existingFAIL and eliminate all6 introduced timeouts in the selected24 Piglit cases. [Compact/search proof](../tests/m9-compact-search-hardware-20261010/README.md).

## Current next actions

Validate full Quake3 GL2 gameplay with deterministic frame captures at 1024x768. Continue separate sRGB extended-control, remaining Piglit undef/execution and CPU varying-store performance tracks. Preserve GPU transforms and tested hardware limits. Root alone owns the locked target queue; a stalled track switches to another open topic rather than repeating an unproductive probe.

All command construction must follow [the capture policy](gpu-capture-replay-policy.md):
derive addresses, descriptors, extents and timeline fields from live owners.
