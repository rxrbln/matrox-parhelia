# M9 GL2 runtime closure: constants, shader coverage, and FBO chaining

This note preserves controlled OpenGL 2.0 behavior from the saved Matrox Linux
ICD while the original M9 system is still operational.  The executable probes
are `tools/m9x-glsl-uniform-ab.c`, `tools/m9x-glsl-feature-matrix.c`, and
`tools/m9x-fbo-texture-chain.c`.  Compact command/resource fixtures are in
`captures/m9x-linux-gl2-matrix-20260914`; run
`python3 tools/m9-glsl-runtime-evidence.py --self-test` to verify them.

All runs used a direct GLX context and reported `Matrox ICD for M-Series` and
OpenGL `2.0`.  They did not use Mesa software rendering.

## Live constant transport

The uniform probe holds one linked program active and performs A/B/A updates
to one `vec4`, followed by one draw and `glFlush` for each value.  Fragment and
vertex variants use the same sentinel values.  Execute sequences 13, 17, and
21 are the three draws.

The allocation result identifies the 0x21000-byte system resource at GPU base
`0x40c3a000`.  The driver appends per-draw records within this resource:

| update | record offset | fragment sentinel offset | vertex sentinel offset |
|---|---:|---:|---:|
| A | `0x000` | `0x000` | `0x018` |
| B | `0x580` | `0x580` | `0x598` |
| A repeat | `0x900` | `0x900` | `0x918` |

The fragment shader consumes all four components, and all four submitted
floats occur contiguously.  The vertex shader consumes only `u_test.x`; its
record retains the first two submitted floats at `+0x18` and omits z/w.  This
is direct evidence of stage-specific live-component packing.  The preserved y
value must not be interpreted as a semantically read component.

The raster pass writes these exact descriptor tuples:

```text
fragment-uniform: 8c=00500400  8d=40c3a001+record  8e=40c3a021+record
vertex-uniform:   8c=00500200  8d=40c3a001+record  8e=40c3a011+record
```

The corresponding preprocessing pass starts at the following part of the
same record and writes `8c=00501500` for the fragment-uniform program or
`8c=00501700` for the vertex-uniform program.  Its 8d/8e values advance with
the same record offset.  This assigns 8c/8d/8e to constant-range resource
selection and proves that the two hardware passes consume distinct ranges.
The low fields within 8c and the low address tags remain unnamed; the 0x20 and
0x10 differences between 8d and 8e are recorded numeric facts, not byte-count
claims.

Every pass writes `8f=0` twice and none writes `90..9f`.  Program state remains
`69=11e42000`, `6a=25010000`, `6b=00001030`, `6c=40c3a381` because this is one
linked program.  The live ordinary GL route therefore binds external constant
ranges from the system resource.  It does not select the inline 80-slot
`8f/90..9f` form.

The static mode-2 wrappers documented in
`parhelia-m9-program-constants-re.md` are still not selected.  The BM buffers
in these fixtures are command-ring storage and are not evidence for the
compiler's mode-2 constant-bank option.  No supported public GL operation or
saved configuration switch that invokes those unreferenced wrappers has been
found.  Mode 2 is **untriggered/unknown**, rather than unsupported by the
vendor.

## GLSL 1.10 feature matrix

One bounded run compiled, linked, used, and drew all 24 cases without a GL
error:

```text
MOV ADD SUB MUL MAD DP3 DP4 MIN MAX RCP RSQ EXP2 LOG2 FLOOR
absolute/negate, BGRA swizzle, partial write mask, scalar predicate,
discard, fixed-function alpha test around a shader draw,
texture2D, texture2D with bias and mipmapped minification,
texture2DProj, textureCube
```

This establishes vendor acceptance and successful dispatch for these GLSL
forms.  It does not by itself assign a native instruction to each expression:
the compiler may expand, fold, or combine operations.  Exact canonical native
sequences for MOV through FLOOR, CMP/SETP, and one zero-metadata TEXLD are in
`parhelia-m9-native-lower-runtime.md`. The follow-up 91-case isolated-lowerer
matrix in `captures/m9-native-lower-matrix-20260914` supplies exact native
words for all destination masks, a source-swizzle bit basis, all source-
modifier values, destination routing classes, temp/varying/constant sources,
predicate polarity and conditional combinations, and both MOV compiler modes.
The final linked GL program resource still does not expose those instruction
bytes in the captured CPU mapping, so the static frontend opcode join plus the
pinned lowerer remains the reproducible compiler bridge.

There were no vendor-rejected cases in this bounded matrix.  Derivatives,
dynamic loops, non-square matrices, shadow comparison, and vertex texture
fetch were not tested and remain unknown rather than unsupported.

## FBO render-to-texture chain

`GL_EXT_framebuffer_object` is advertised.  A 64x64 RGBA8 texture attached to
an FBO reports `GL_FRAMEBUFFER_COMPLETE_EXT`; rendering to it and sampling it
in a later GLSL draw both return `GL_NO_ERROR`.

The direct no-readback capture keeps the producer and consumer asynchronous:

| sequence | role | final raster state |
|---:|---|---|
| 14 | render into texture | `b0=11f21000`, `b1=c8010040` |
| 15 | sample texture into window | `78=11f21000`, `79=c8010040` |

The exact equality of producer `b0/b1` and consumer `78/79` proves the same
surface backing and descriptor move from render-target state to texture unit
zero.  Producer post-state writes `43=00000350`; before the consumer raster
draw, state writes `43=000003e0`, followed by `e3=000003f0`.  The consumer
dependency is newer than the completed producer task.  A draw references
multiple resources, so `0x3e0` can be their maximum dependency; this capture
does not claim it is the texture's fence value alone.  Together with the
statically proven class-1 producer/resource query in
`parhelia-m9-texture-coherency-re.md`, it closes the live render-to-sample
ordering path.

A separate correctness run enables one-pixel readback after both stages.  The
producer center is RGBA `(3,130,130,255)` and the sampled consumer center is
`(2,128,128,255)`, total absolute difference 5 from interpolation/filtering.
This validates visibility through the chain.  The readback run is deliberately
separate because its synchronization would obscure the normal asynchronous
submission capture.

No standalone texture-cache invalidation packet is visible.  The supported
implementation rule is to register render targets as class-1 writes, register
later texture backing as a sampled read, emit the maximum returned dependency
through index 43 before the consumer, and preserve the observed state order.
Whether index 43 has an implicit cache side effect inside hardware remains
unknown; no additional vendor packet is required in this tested chain.

## Two-client context and resource isolation

`captures/m9x-linux-gl2-context-20260914` preserves two independent direct-GLX
processes with simultaneously live contexts and different linked fragment
programs.  The parent alternated their draws A/B/A/B.  Both clients returned
the same pixel on their first and repeated draw with `GL_NO_ERROR`, while the
two clients returned different pixels:

| client | readback on draw 1 and 2 | system resource | index-69 resource |
|---:|---|---:|---:|
| 0 | `(16,48,64,255)` | `40c3a000` | `11e42000` |
| 1 | `(97,16,31,255)` | `41105000` | `11f5a000` |

For both sequence 13 and sequence 21, each client has the same three command
ranges with packet counts `[149,37,3]`.  The repeated draw restores that
client's exact `69/6a/6b/6c` tuple.  Client 0 binds
`69=11e42000,6a=25010000,6b=00001030,6c=40c3a281`; client 1 binds
`69=11f5a000,6a=25010000,6b=00001030,6c=41105281`.

The stage ranges are equally address-space local.  The final raster range is
`8c=00500400`, `8d=resource+1`, `8e=resource+0x21`; the preceding preprocess
range is `8c=00500d00`, `8d=resource+0x21`, `8e=resource+0x89`.  Complete
0x21000 resource snapshots are byte-identical between each client's first and
repeated draw, and the linked record at byte offset 0x280 differs across the
two shader programs.  This closes process/context ownership, bind restoration,
and resource-address isolation for the tested GL2 path.  The later static
`RsFragPtrsBuffer` constructor and binding recovery proves that
index 69 is each context's external fixed-function
vertex/declaration-conversion fragment-database binding.  The address-directed
BAR capture in `parhelia-m9-gl2-special-capture.md` is an opaque alias; it did
not establish drawable ownership or resolve translation to the logical
database.

The fixture also includes the authoritative full `glxinfo -l` inventory from
the same live stack.  It identifies direct rendering by `Matrox ICD for
M-Series`, OpenGL 2.0, GLSL 1.10, four fixed-function texture units, 16
fragment texture image units, four vertex texture image units, 20 combined
units, eight texture-coordinate sets, and four draw buffers.  PCI, kernel, X
process/executable, and libGL/tracer/test binary hashes are saved alongside it.

Run `python3 tools/m9-glsl-two-client-evidence.py --self-test` to verify the
command/resource joins, isolation, readback, and inventory.

## Remaining live differentials

The strongest remaining shader-specific gaps are exact sampler wrap/filter/
anisotropy/LOD fields and comparison-control bit values beyond the one static
CMP/SETP fixture. FBO copy/blit-to-sample and
depth/stencil attachment transitions are separate producer families and are
not covered by this color-render-to-sample fixture.

Those are explicit implementation-validation targets.  The features actually
exercised above are supported by the saved vendor stack.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
