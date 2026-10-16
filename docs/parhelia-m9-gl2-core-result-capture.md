# M9 core OpenGL 2 result capture

The live vendor driver closes several core OpenGL 2 behavior questions that
static binary analysis could not answer.  The normalized evidence is in
`captures/m9x-linux-gl2-core-results-20260914`; its executable verifier is
`tools/m9-gl2-core-result-evidence.py`, and the regenerating client is
`tools/m9x-gl2-core-result-matrix.c`.

## FBO completeness and producer-to-consumer behavior

The implementation returns the expected EXT status values for two negative
cases [CAP]:

| configuration | status | error |
|---|---:|---:|
| no attachment | `0x8cd7` (`INCOMPLETE_MISSING_ATTACHMENT`) | zero |
| 32x32 color plus 16x16 depth | `0x8cd9` (`INCOMPLETE_DIMENSIONS`) | zero |

A 32x32 depth-only FBO with both draw and read buffers set to `GL_NONE` is
complete (`0x8cd5`).  A draw with `GL_ALWAYS` writes depth 0.5 and reads it
back without error [CAP].  A stencil-only FBO is also complete; clearing to
`0x35` reads back 53 without error [CAP].

Attaching distinct color, depth, and stencil storage at once yields
`0x8cd6` (`INCOMPLETE_ATTACHMENT`) on this vendor route.  Clear/draw/read then
returns `GL_INVALID_FRAMEBUFFER_OPERATION` (`0x506`) and zero data [CAP].
This is a demonstrated vendor limitation, not evidence that the hardware
lacks packed depth-stencil.

After detaching stencil, the color-plus-depth FBO is complete.  The producer
draw reads `(77,102,147,255)` from its color texture.  Binding the same texture
to a default-framebuffer consumer draw produces the identical center pixel
with `GL_NO_ERROR` [CAP].  The producer's selected submission ends with
index 69 `0x11e42000`, 6a `0x25010000`, 6b `0x1030`, and 6c
`0x40c3a301`.  The consumer keeps those values, changes 6c to `0x40c3a901`,
and binds texture index 78 to `0x11f41000` [CAP].  This provides a concrete
render-to-texture state/cache transition for the new driver.

## Occlusion queries

All four query cases report availability zero before the explicit finish and
return a blocking result afterward with no GL error [CAP]:

| case | samples passed |
|---|---:|
| full 128x128 quad | 16384 |
| depth-hidden quad | 0 |
| half depth-hidden quad | 8192 |
| reused query object | 16384 |

Every focused query draw has four ExecuteDma ranges containing
`164, 38, 8, 3` decoded packets.  The hidden case happens to use bus-master
mapping zero while the other three use mapping one; the fixture retains the
active mapping rather than assuming a fixed slot [CAP].  Query counter wrap
was not attempted because there is no bounded, safe way to force it on this
aging stack.

## GLSL built-ins and uniform types

`gl_FragDepth` compiles, links, and draws.  The center readback is
`(64,127,191,255)` with depth 0.25 [CAP].  `gl_FrontFacing` also compiles and
executes: oppositely wound triangles produce red for the front-facing side and
green for the back-facing side.  Its final index-6a value is `0x24cc0000`,
distinct from the common `0x25010000` in the other focused draws [CAP/OBS].
The bit meaning remains open.

One linked program uses vertex and fragment arrays, integers, booleans, and
mat4 uniforms together.  Its draw returns `(93,97,84,207)` with no error
[CAP].  This proves those types on an ordinary active-uniform route; it does
not establish every packing rule for arrays of matrices or non-square matrix
types introduced after GLSL 1.10.

The combined point case uses `gl_PointSize` in the vertex source and
`gl_PointCoord` in the fragment source.  Fragment compilation fails with an
empty vendor log even though `GL_ARB_point_sprite` is advertised [CAP].
Because linking never occurs, this case establishes the `gl_PointCoord`
frontend rejection but does not independently decide shader-written
`gl_PointSize`.  Fixed-function point-size and point-sprite evidence is
documented separately.

## Uniform-limit behavior

The ICD reports 512 vertex and 512 fragment uniform components.  Active
`vec4` arrays of 128 and 129 elements both link in each stage.  The active
uniform query returns size 128 or 129 respectively, location zero is zero,
and the last locations are 127 and 128 [CAP].  Thus the vendor linker does not
enforce its advertised 512-component limit for the tested limit-plus-one
declarations.  A new driver should enforce the implementation limit it
advertises rather than reproduce this inconsistency.

## MRT wedge boundary

The four-output shader links and its four RGBA8 attachments form a complete
FBO.  Draw-buffer mask zero and mask one both finish and permit attachment
readback.  Mask zero leaves all four attachments at zero; mask one writes red
to attachment zero and leaves the others zero [LIVE-OBS].

Changing the same live context to mask two reaches the flushed
`MARK mrt-02 error=0x0` line, then wedges before the per-attachment result
line.  A separate all-four-output run without readback reaches the same
zero-error marker and then spins in kernel device-fd teardown despite `_exit`
[LIVE-OBS].  Both require target reset.  The precise boundary is therefore:
shader link, FBO completeness, draw submission, and `glFinish` succeed; later
readback or resource teardown is unsafe.  No claim is made for masks 3 through
14.  For mask 15 only draw completion is established; attachment correctness
must not be inferred from the successful shader link.

## Evidence boundaries

The focused packet fixture covers 12 submissions across 11 cases.  Each
retains the active bus-master image and the full system resource selected by
allocation zero.  That resource is byte-stable across every selected
ExecuteDma call, consistent with compiler/client-populated state consumed by
the submission [CAP].  FBO status, pixel/depth/stencil results, query values,
and shader link results come from exact client output.  MRT facts are labelled
operator observations because the temporary packet capture could not survive
the resets required to recover the host.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
