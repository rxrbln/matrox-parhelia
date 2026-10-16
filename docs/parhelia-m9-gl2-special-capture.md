# M9 GL2 special-operation live capture

This capture closes the vendor-frontend and live-execution boundary for the
highest-value GLSL 1.10 special operations.  It also corrects the resource role
previously assigned to command index 69.  Evidence is in
`captures/m9x-linux-gl2-special-20260914`; the executable verifier is
`tools/m9-glsl-special-evidence.py`.

## Live results

The direct context reported `Matrox ICD for M-Series`, OpenGL 2.0, and
`1.10 Matrox GLSL compiler`.  Every accepted case linked, drew, finished,
read one pixel, and ended with `GL_NO_ERROR` [CAP]:

| case | result |
|---|---|
| baseline | `(141,114,114,255)` |
| conditional discard | `(141,114,114,255)` |
| unconditional discard | clear survives: `(16,32,48,255)` |
| `dFdx` / `dFdy` | `(3,3,3,3)` / `(0,0,0,0)` |
| `texture2D` / bias | `(127,98,129,255)` / `(127,98,129,255)` |
| projected `texture2D` | `(125,185,122,255)` |
| `shadow2D` | `(255,255,255,255)` |
| constant / uniform `if` | `(141,114,114,255)` / `(141,114,114,255)` |
| fixed / uniform loop | `(41,83,134,237)` / `(41,83,134,237)` |
| `mat4 * vec4` | `(173,149,153,255)` |
| vertex `texture2DLod` | `(0,0,0,255)` |
| ARBfp `SLT` / `SGE` | `(0,0,0,0)` / `(255,255,255,255)` |

Fragment `texture2DLod` and `texture2DGradARB` are clean compiler rejections.
The live extension string does not advertise `GL_ARB_shader_texture_lod`, so
these are unsupported by the vendor frontend rather than failed GPU draws
[CAP].  Vertex explicit LOD is accepted and executes under the advertised
four-vertex-sampler limit [CAP].

## Bind and resource ownership

One context owns a stable index-69 allocation and one 0x21000 system resource.
All 17 selected raster submissions end with [CAP]:

```text
69 = 11e42000
6a = 25010000
6b = 00001030
6c = 40c3a000 + case_linked_offset + 1
```

The selected 6c offsets progress from `0x200` through `0x4900`; each is
0x80-aligned before the retained low tag bit.  The case manifest records exact
offsets, packet counts, stage-resource descriptors, and hashes.  The first
emitted resource range is the preprocess range and the second is the raster
range.  The invariant fixed-vertex-shader preprocess payload is 13 64-bit
slots.  Raster payloads range from two slots for simple cases to six for the
uniform branch and ten for loops or matrix constants.  ARBfp uses a 22-slot
preprocess payload and a four-slot raster payload [CAP].  These are opaque stage
resources: baseline, `dfdx`, and ordinary `texture2D` have byte-identical
13-slot preprocess and two-slot raster payloads, so the ranges cannot be the
sole native programs and their size is not a native instruction count [DER].

The full system resource is byte-identical immediately before and after each
selected ExecuteDma call.  It is compiler-populated state selected by the draw,
not a result written by that submission [CAP].

## Index 69 capture correction

The allocation descriptor used by index 69 reports address `0x11e42000` and
extent `0x1b080`.  The original address-directed BAR snapshots appeared to
expose pixels from the preceding draw.  Representative observed prefixes were
[CAP]:

| submission before which bytes were read | repeated first dword |
|---|---:|
| `dfdx` | `ff102030` (the clear color) |
| `dfdy` | `03030303` (the `dFdx` result) |
| `tex2d` | `00000000` (the `dFdy` result) |
| `if-const` | `ffffffff` (the shadow result) |
| ARBfp `SLT` | `ff000000` (the vertex-texture result) |
| ARBfp `SGE` | `00000000` (the `SLT` result) |

That interpretation is superseded by the exact `RsFragPtrsBuffer` constructor
and binding call graph.  Index 69 binds a per-context immutable, flattened
fixed-function vertex/declaration-conversion fragment database.  The captured
`0x1b080` address-directed BAR bytes are an opaque alias, not a linear copy of
the database, drawable evidence, or proof of an MMU/root structure [BIN/CAP/DER].
The apparent preceding-draw values show that the snapshot did not resolve the
resource's logical mapping; translation remains unresolved.

## Native-looking bytes in the selected 6c resource

Three selected-record windows contain aligned 64-bit sequences whose bit
patterns match the established native-instruction shape [CAP]:

| case | offset from untagged 6c | candidate slots | first / last pair |
|---|---:|---:|---|
| `dfdy` | `+0x100` | 8 | `1e62001d/ae003c08` / `ffc02d1e/a29fff1f` |
| uniform loop | `+0x100` | 25 | `e084185c/9001405f` / `ffff801c/a01fc00f` |
| vertex texture LOD | `+0x108` | 5 | `1e622d1d/ae003c18` / `ffc0ad1e/a29fff1f` |

The verifier pins every pair, not just the endpoints.  The `dfdy` span includes
two identical `ffff9100/a000000f` pairs; `0x91` is numerically consistent with
the statically recovered derivative operation family, but the capture does not
yet prove that field assignment [OBS].

This is evidence that native-looking program material can reside within the
0x21000 system resource near the 6c record.  It is not yet a complete storage
rule: several decisive A/B windows, including `dfdx` and ordinary `tex2D`, do
not expose operation-specific words at the same relative position, and the
selected window may include adjacent allocator objects.  The universal native
start/count, vertex versus fragment boundary, and internal stage-entry fields
remain open.  A driver must keep arbitrary generated GLSL gated until static
producer/consumer analysis joins those fields to these exact live bytes.

## Reproduction controls

`tools/m9x-glsl-special-matrix.c` creates all source programs in one context.
The tracer's `M9XTRACE_EXEC_LIST` selects sparse absolute submissions and
`M9XTRACE_MAP_LIST` selects one tracked mapping, avoiding repeated full-aperture
dumps.  The exact sparse control values are recorded in the capture README.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
