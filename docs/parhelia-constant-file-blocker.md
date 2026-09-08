# The constant file blocks transform, clipping and lighting

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Status 2026-09-08: **open**.  The depth, alpha and blend work of
`docs/parhelia-3d-state-complete.md` is all fixed-function state written through
ordinary register lists.  Transform, clipping and lighting are different: all
three read the WARP **constant file**, and every attempt to write it from our
own stream stops the engine.  This file records what has been excluded, so the
next attempt does not repeat it.

## What the constant file needs

The `0x41` block load is not a DMA write to a register block - it is **executed
by a WARP program**.  In the vendor's 3D draw span
(`captures/gl-matrix/depth-secs.txt`, section `195204ea`) every block load is
preceded by a control bind:

```
REG 1374=00000000 10f0=20000028 10f0=20000028 1100=08557383   <- bind kind 0
BLK n=8 first=0x0        <- c[0..7]
BLK n=5 first=0x134  BLK n=5 first=0x149  BLK n=5 first=0x155
BLK n=2 first=0x160  BLK n=4 first=0x172
BLK n=1 first=0x178  BLK n=2 first=0x17d
REG 1130=0700200c 1374=2121dd87 10f0=20000028 10f0=20000028 1100=08557583
BLK n=224 first=0x160    <- the kind-1 base-register load
REG 1374=00000000 10f0=20000028 10f0=20000028 1130=0200200c
```

`0x08557383` is `mtx1079`, the kind-0 **constant-file loader**: a 14-instruction
loop that copies each block into `c[]` through destination file 3.  MODE
`0x20000028` binds this control class; the per-vertex program is bound later
with MODE `0x20000002`.

The block load's "sub-header" is not a header at all - it is the first vec4 of
data, consumed as the bound program's `v0`.  For the loader `v0 = (count,
first, 1, 0)`; for the kind-1 program `v0 = (b0, b1, b2, b3)`.  The packet
header's vec4 count excludes it.

## Excluded by experiment

Each row is a hardware run with a positive control in the same run.  "stalls"
means the draw produced nothing **and** the engine stopped retiring
(`idle 0`) - a hang, not a dropped draw.

| variant | result |
|---|---|
| no constants, passthrough program, screen-space vertices | draws 86577 px, idle |
| **same stream plus the constant block loads** | **stalls** |
| c0..c3 alone / c8,c9 alone / c[0x160] alone | each stalls on its own |
| our `mtx1079` copy bound (MODE `0x20000028`), **no** block loads | draws 86585 px, idle |
| loader bound, block loads emitted | stalls |
| all three loads in ONE ring call, as the vendor emits them | stalls |
| plus the kind-1 `mtx251` base-register program and its verbatim packet | stalls |
| our own program given a real non-zero entry (`--warpidx 32`) | draws 86577 px, idle |

So it is **not**: the packet encoding (our headers are bit-identical to the
vendor's `0x41080013` / `0x4104000b` and the sub-header is `[n][first][1][0]`),
the destination slot, the ring-call grouping, the loader bind itself, or a
missing program entry.

## Leading hypothesis

The loader is an `f2b = 0` control-class program built around `getidx`, `mova`,
`subi` and `br`, and its loop termination depends on `a0`.  We upload it to the
**on-chip instruction store** (`--warpstore`, `0x920` bit 0 = 0) while the
vendor runs it from the sysmem heap.  If that store cannot host a control-class
program, or if `a0` is not what the loop expects, the loop does not terminate -
which is exactly the observed symptom, an engine that stops retiring rather
than a draw that renders nothing.

Binding `mtx251` did not fix it, but that packet's `v3.x` is a kind-6 clipper
entry we do not have and we passed 0 there, so `a0` may still not be latched.

## Next steps

1. Read the constant file back (`docs/parhelia-block-file-constants.md` has the
   read path) after a load, with the engine still alive, to separate "the load
   never happened" from "the load happened and something later hangs".
2. Run the loader from the heap path (`--warpprog` without `--warpstore`)
   rather than the on-chip store.
3. Failing both: transform on the CPU.  `p3d_warp_passthrough` consumes window
   -space vertices and is fully working, so a driver can ship the fixed-function
   path with a CPU transform and no constant file at all.  Lighting and hardware
   clipping would stay unavailable.
