# Peer source and progress inventory — 2026-10-09

The current source trees and latest campaign log supersede the October 8
handover. This inventory records a filesystem inspection; a worktree or log
alone does not prove an agent is still running.

## Canonical trees

| Tree | HEAD | Inspection |
| --- | --- | --- |
| `src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/m9x` | `a6e0cb7` | Clean; includes shader select, uniform inlining and record texel-pair fixes |
| `src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm` | `ca1ee48` | Engine read/fill enabled; engine upload remains optional |
| `xf86-video-parhelia` | Includes October 9 topic evidence | Progress log is newer than the general handover/status headers |

## Peer work remaining to reconcile

Paths below are under `/home/ai/tmp/agents`. Preserve uncommitted work; it
has not been promoted merely because it exists.

| Worktree | HEAD / branch | Current state and next proof |
| --- | --- | --- |
| `m9piglit3-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `a6e0cb7`, `undef` | Two dirty files: constant folding and undef lowering, plus temporary dumps. Not integrated |
| `m9unify6-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `c0c1be2`, `m9fbo9` | Five dirty files completing sRGB constant blending, target masks and non-native draws; default-on change awaits its extended proof |
| `m9unify-mesa/mesa-26.2.2/src/gallium/drivers/m9x` | `fed13b9`, `m9logic` | Four dirty files in context/draw/native plumbing. Older branch; preserve separately |
| `m9glperf-drm-wt` | `df0e59a`, `cmdcached` | Clean committed command-BO coherent backing, based on canonical `ca1ee48`. Built module; short locked hardware A/B pending |
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

## Current next actions

Retest the exact Quake3 floor/weapon fixtures on main before using old probe
results. Investigate interpolation-pressure split ranking and record-producing
passes that take colour-specific instruction templates. Keep proven register
limits and GPU transforms. Test command-buffer caching separately at
1024x768 with the same Mesa DSO; its build is not a measured speedup.

All command construction must follow [the capture policy](gpu-capture-replay-policy.md):
derive addresses, descriptors, extents and timeline fields from live owners.
