# Hardware transform (TCL) on the P690 WARP vertex engine

Copyright 2026 René Rebe <rene@exactco.de>

Status of this document: implementation notes for the fixed-function
transform path added to the kernel WARP program set and to the Mesa/Gallium
driver.  Every hardware claim below is marked with the run it came from;
claims taken from the reverse-engineering documents carry their citation.

## 1. Why

A perf profile of the ioquake3 timedemo client spends about 36% of its CPU
samples inside Mesa's TGSI interpreter (`fetch_src_file_channel`,
`fetch_source`, `store_dest`, `vs_exec_run_linear`), because Gallium's draw
module transforms every vertex in software before the Parhelia backend packs
window-space triangles.  `/sys/kernel/debug/dri/0/parhelia_submit_stats`
showed the engine idle for about 30% of each frame waiting for that work.

The WARP vertex engine can do the transform itself.  The kernel already owns
a `warp_transform` program; what was missing was a textured variant, a
userspace path that feeds object-space vertices, and evidence that the result
is pixel-correct.

## 2. What the WARP transform program actually computes

`warp_transform` (kernel `parhelia_warp.c`, source
`drivers/gpu/drm/parhelia/warp/transform.wasm`) disassembles to:

```
 0: add  T0, v0, v0.0000              ; T0 = object-space position
 1: dp4  r2.w, T0, c3
 2: dp4  r2.z, T0, c2
 3: dp4  r2.y, T0, c1
 4: dp4  r2.x, T0, c0                 ; r2 = clip space
 5: add  o1, v5, r1.0000 | mov T1, c9 ; o1 = colour, T1 = viewport offset
 6: smul r5, r2, r2.xyz1 | mov o11, r2; r5 = (x/w, y/w, z/w, 1/w)
 7: mad  T1.xyz, c8.xyz1, r5, T1.xyz0 ; window = scale * ndc + offset
 8: add  o0, T1, r1.0000
 9: end
10: wra
```

Facts that shape the driver, with citations:

* `smul` broadcasts the reciprocal of source 0's `w` lane
  (`parhelia-warp-control-ops.md:977` for the `src0.w` broadcast,
  `parhelia-p690-warp-isa-re.md:262` for the reciprocal, tagged inferred but
  strong).  Swizzle codes 4 and 5 are the literals 0.0 and 1.0
  (`parhelia-warp-isa-complete.md:38-42`), which is why `r1.0000` and
  `r1.1111` work out of an uninitialised scratch register.
* **The program emits window space, not clip space.**  `o0` is the setup
  unit's position input and the program performs the divide and the viewport
  itself (`parhelia-p690-warp-isa-re.md` §3.2, PROVEN(hw)).  Writing clip
  space to `o0` has never been tried.
* `o0.w` ends up as `c9.w` — the viewport height — because instruction 7
  masks `.xyz` (`parhelia-warp-programs-decoded.md:191`, and the kernel's own
  reference constants `c9 = (512, 384, 0.5, 768)` for 1024x768).  It is *not*
  `1/w`; the `1/w` that `smul` produced stays in `r5.w` and reaches no
  output.  Perspective-correct texturing therefore does not come from this
  program, exactly as on the existing pass-through path.
* `o11` receives the clip-space position.  Every shipping program writes it,
  but no document traces a consumer, so hardware frustum clipping cannot be
  assumed from it.
* `o1` is colour 0, `o3`/`o4` are texture-coordinate sets 0 and 1
  (`parhelia-p690-warp-isa-re.md` §3.2, PROVEN(hw)).  `o4` **must** be written
  whenever set 1 is advertised or the hardware drops it from `0x1110` before
  the draw.

The constants the program reads:

* `c0..c3` are the matrix rows in the sense `r2.i = dot(v, c_i)`, i.e.
  `c_i[k] = M[i + 4k]` for a GL column-major matrix `M`.
* `c8 = (sx, sy, sz, 1)` viewport scale and `c9 = (tx, ty, tz, H)` viewport
  offset, with the GL depth range folded into the `z` components
  (`parhelia-block-state-map.md` §1.a, `parhelia-block-file-constants.md:178-197`).
  The same pair is written again at `c[0x160]/c[0x161]`, which is where the
  resident clipper program reads it through base register `b1`.

## 3. Kernel: two new programs

`parhelia_warp.c` gains `warp_transform_textured` and
`warp_transform_multitex`, assembled from the new sources
`warp/transform-textured.wasm` and `warp/transform-multitex.wasm` with
`tools/parhelia-kernel-warpasm.py`.  They are `warp_transform` plus the
`mov o3, v7` (and `mov o4, v8`) of the existing textured programs.  Like
`warp_textured` they deliberately do **not** write temporary `T2`: writing it
there locked up the first vertex-alpha draw of the Q3 demo.  `T2` is primed
once per context by `ph_prime_warp()`'s degenerate pass-through draw.

`warp_transform_multitex` additionally initializes `T2` with
`mul T2, v5, r1.0000`, exactly as `warp_textured_multitex` does.  A fresh
engine rejects or locks up on a draw that depends on an unwritten `T2`, and
that program is the one a two-set transformed draw binds first.  The
single-texture variant deliberately does not write it, because in
`warp_textured` a `T2` write locked up the first vertex-alpha draw of the Q3
demo.

`warp/transform.wasm` was added as the source of the already-resident
`warp_transform`; the assembler reproduces the existing 11 words bit-exactly,
which is what keeps the three programs in step.

A WARP entry may only be written to `0x1100`/`0x1104` if the kernel handed it
out, so `parhelia_warp_entry_allowed()` had to learn the two new entries as
well; without that the first textured transform submit came back `-EACCES`
(measured, session of 2026-09-11).

Instruction-store placement (`parhelia_drv.h`), appended after the probe
allocation and covered by the existing `WARP_FITS` assertions:

```
PARHELIA_WARP_TRANSFORM_TEXTURED_ENTRY  (2656 * 4)   128 instructions
PARHELIA_WARP_TRANSFORM_MULTITEX_ENTRY  (2784 * 4)    32 instructions
```

New parameters, appended, never renumbered:

```
PARHELIA_PARAM_WARP_TRANSFORM_TEXTURED  29
PARHELIA_PARAM_WARP_TRANSFORM_MULTITEX  30
```

## 4. Mesa: recognising the fixed-function vertex program

Mesa's fixed-function vertex program is remarkably regular.  Dumped from a
running client with `PARHELIA_TRACE_TCL=dump`, this is what **this** driver
receives - a single basic block, vectorised, with the matrix as four
`load_ubo_vec4` reads indexed by vec4 rather than by byte:

```
%1..%4 = load_ubo_vec4(0, 0) (base = 0..3)   ; mvp.transpose rows
%6  = load_deref(VERT_ATTRIB_POS)
%7  = fmul %1, %6.xxxx
%8  = fmul %2, %6.yyyy       %9  = fadd %8, %7
%10 = fmul %3, %6.zzzz       %11 = fadd %10, %9
%12 = fmul %4, %6.wwww       %13 = fadd %12, %11
store_deref(VARYING_SLOT_POS, %13)
store_deref(VARYING_SLOT_COL0, load_deref(VERT_ATTRIB_COLOR0))
store_deref(VARYING_SLOT_TEX0, load_deref(VERT_ATTRIB_TEX0))
store_deref(VARYING_SLOT_TEX1, load_deref(VERT_ATTRIB_TEX1))
```

The shape is not universal: a driver whose `nir_shader_compiler_options` set
`lower_to_scalar` and `lower_uniforms_to_ubo` - softpipe, for instance - gets
the same program as sixteen scalar `fmul`/`ffma_weak` operations feeding a
`vec4`, with byte-addressed `load_ubo`.  The matcher accepts both, which is
why it describes values rather than matching a fixed instruction sequence.

`ph_vs_analyze()` walks that block once and describes every SSA value as one
of: a 32-bit constant, a variable dereference, a `load_ubo` vec4 at a constant
offset, a whole shader input, one accumulated component of a matrix product,
or a complete matrix product.  A shader is accepted only when

* the sole computed output is `VARYING_SLOT_POS` and its value is the full
  four-component product of four consecutive `load_ubo` vec4s with one input,
  in either operand order (`ffma_weak` and `ffma` both accepted), and
* every other store copies a whole shader input into `VARYING_SLOT_COL0`,
  `VARYING_SLOT_TEX0` or `VARYING_SLOT_TEX1`.

Anything else - a texture matrix, GL lighting, fog, a GLSL shader - leaves
`ff_transform` false and keeps the software vertex pipeline.  A texture matrix
is the common case that falls back: Mesa emits a second 4x4 product into
`VARYING_SLOT_TEX0` and the matcher rejects the extra computed output.

The matrix itself is read from the bound vertex constant buffer at the offset
the matcher recorded, and transposed into the WARP `c0..c3` convention.

## 5. Mesa: the draw path

`ph_draw_tcl()` replaces `draw_vbo()` for accepted draws:

1. `ph_tcl_layout()` rejects everything the WARP program cannot express:
   user clip planes, polygon offset, flat shading, polygon fill modes,
   `depth_clamp`, rasteriser discard, primitive restart, instancing, and all
   of the P690 alpha work-arounds (`vertex_alpha`, `alpha_micro`,
   `alpha_micro_cutout`, `cpu_alpha_cutout`, `rgb_alpha_two_pass`,
   `native_sampled_alpha`), because each of those owns the vertex layout.
2. The draw is expanded into an independent-triangle index list.  Strips,
   fans, polygons, quads and quad strips are resolved here, the odd strip
   triangle swapping its first two vertices so the winding survives; the
   hardware draws one triangle per packet anyway.  Every expansion keeps the
   source primitive's winding, which is what the hardware face cull reads.
3. `translate` (the Gallium vertex fetch/convert module) writes the
   object-space stream directly into the batch's vertex staging buffer:
   `x, y, z` then `r, g, b` then `s, t` and `s1, t1`.  This is the same
   layout the depth-bound descriptors already describe, which is why
   `p3d_position_has_z()` now selects the three-component position
   descriptor for `has_depth || transform`.
4. Every emitted vertex is checked against the clip volume (section 6).  A
   chunk that does not fit publishes what already fitted and hands the rest
   of the draw to the software pipeline, using the expanded index list as a
   32-bit user index buffer.

`ph_submit_vertices()` then sets `p3d_state.transform`, the matrix pointer,
the constant-loader entry and the viewport scale/offset, and picks the
transforming WARP program through `ph_warp_entry()`.  `p3d_emit_state()`
already knew how to bind the constant loader and emit the block loads; that
code had never been exercised from Mesa.

Details that the vertex layout forces:

* The quad split uses the draw module's diagonal (`0,1,3` and `1,2,3`), so a
  non-planar quad interpolates the same whether it took the hardware path or
  was handed back to the software clipper.
* Points and lines share `ctx->vertices` with triangles, so a transforming
  draw publishes any pending point or line batch first, exactly as the draw
  stages do.
* The present-quad copy shortcut in `ph_submit_vertices()` reads a
  seven-float window-space stream and is therefore skipped for a
  transforming batch.
* `c8`/`c9` are only programmed from a viewport the state tracker actually
  set, and the matrix is only read from a constant buffer whose size covers
  it; the matcher additionally rejects a matrix at an unaligned or absurd
  constant offset, which is the only bound on an offset that comes out of
  the shader.

Environment variables: `PARHELIA_NO_HW_TCL=1` forces the software vertex
pipeline, `PARHELIA_HW_TCL=false` disables the feature at context creation,
`PARHELIA_TRACE_TCL=1` prints the per-shader matcher result.  With
`PARHELIA_STATS=1` the per-frame line reports how many draws took the
hardware transform, how many the predicate could not express, and how many
were handed back to the software clipper.

## 6. Clip volume: what is checked and why

The vendor stack never asked this silicon to survive `w <= 0`: its own ICD
clipper computes outcode bits for a zero/denormal and a negative `w` and
rejects on them, and the software guard-band `gz` is pinned to 1.0 so near and
far are always clipped exactly (`parhelia-raster-clip.md` §8, and the same
document's conclusion "assume it cannot").  No document traces a consumer of
`o11`, so hardware frustum clipping cannot be assumed.

The driver therefore verifies, per emitted vertex, that

* `w > 0`,
* `|x| <= gx*w` and `|y| <= gy*w`, where `gx`/`gy` are the NDC bounds that
  keep the window coordinate inside the +-8160 pixel guard band that the
  hardware clip-volume registers `0x1120`/`0x1124` carry
  (`parhelia-engine-context-bringup.md` §14.3), and
* `-w <= z <= w`.

That costs sixteen multiply-adds and eight compares per vertex - far less
than one TGSI-interpreted vertex - and it is computed from the already-packed
object-space position, so nothing is fetched twice.  Anything outside keeps
the software clipper, which is still correct, just slower.

## 7. Tests

* `tools/parhelia-gl-tcl-test.c` renders eleven scenes - ortho gouraud quad
  from a vertex array, the same quad as a culled fan in both winding senses,
  one- and two-unit textured quads, a perspective depth-tested cube, the cube
  with a narrow `glDepthRange` and a scissor, a cube crossing the near plane,
  a cube crossing the far plane, a quad that leaves the viewport but stays
  inside the guard band, and a triangle far outside it - and reads every
  pixel back.  Each scene self-checks its coverage.
* `tools/parhelia-gl-tcl-compare.sh` runs the binary twice, once with
  `PARHELIA_NO_HW_TCL=1`, and compares the dumps pixel by pixel, allowing one
  least-significant step for the rounding difference between the two
  transform orders.
* `parhelia-drm-test --transform` drives the untextured program directly from
  the batch builder with an identity matrix and the 256x256 reference
  viewport, and checks the gouraud ramp over the whole surface.
* `parhelia-drm-test --transform-textured-probe` does the same for the new
  textured program without Mesa in the way: it primes `T2` with one
  degenerate pass-through triangle, uploads a matrix that maps the 256x256
  pixel grid to clip space, and checks that the resulting textured triangle
  covers exactly the pixels the pass-through probes cover.
* `parhelia-drm-test --selftest` gained a host-side check that a transforming
  state declares the three-component object-space position and the matching
  stride with and without a texture, so the descriptor change cannot regress
  silently.

## 7a. What the hardware actually did (2026-09-11)

Measured on the P690 target, module `parhelia.ko` with the new programs:

* `parhelia-drm-test --transform`: `covered=18336 expected=18336
  mismatches=0`.  The untextured transform program, the constant loader, the
  matrix upload and the viewport constants reproduce the pass-through probe's
  geometry and its gouraud ramp exactly, over the whole 256x256 surface.
* `parhelia-drm-test --transform-textured-probe`: `covered=18336
  mismatches=0`.  The new single-texture transform program is pixel-exact
  too, at the same geometry.
* `PARHELIA_TRACE_TCL=1` on the GL test:
  `vs hardware-transform=1 mvp=0 pos=0 col=1 st=2/3` - the matcher accepts
  the untextured, one-texture and two-texture fixed-function programs and
  finds the right attribute slots.
* GL scenes with the hardware transform produced byte-identical readbacks to
  the software vertex pipeline for the untextured quad
  (`hash=ddf9f9b99d123dd3` both ways) and the one-texture quad
  (`hash=1353b41ebb708483` both ways).
* **The two-set route wedged the engine**: at the two-unit scene the
  asynchronous fence `0x180` made no progress for 2001 ms and every later
  draw in that boot rendered nothing.  It is now opt-in
  (`PARHELIA_HW_TCL_MULTITEX=true`) and off by default; see section 9.

Two earlier failures on the way there are worth recording because they are
the kind of thing that looks like a hardware problem and is not:

* The first textured transform submit returned `-EACCES`.  The WARP entry
  written to `0x1100`/`0x1104` must be one the kernel handed out
  (`parhelia_warp_entry_allowed()`), and the two new entries were not in that
  list.
* The matcher initially rejected every shader because it only understood the
  byte-addressed, scalarised `load_ubo` form.  This driver receives the
  vectorised `load_ubo_vec4` form; see section 4.

## 8. Cross-file changes

Two changes fall outside the files this work owns and are listed here so they
can be reviewed with their owners:

* `parhelia_submit.c` `parhelia_get_param_ioctl()`: two additional `case`
  labels returning the new WARP entries.  `GET_PARAM` is the only way to
  publish an instruction-store entry to userspace.
* `parhelia_batch.c` `batch_state()`: the vertex-stride check becomes
  `5u + !!p3d_position_has_z(input)` instead of `5u + !!input->has_depth`, so
  a transformed draw without a depth buffer is accepted.  Without it the
  existing `--transform` probe could not pass either: it submits six dwords
  per vertex while the validator computed five.

## 9. Known limits and what remains

* **The two-set (multitexture) transform wedges the engine and is off by
  default.**  `warp_transform_multitex` is `warp_transform` plus
  `mov o3, v7`, `mov o4, v8` and the `mul T2, v5, r1.0000` that
  `warp_textured_multitex` carries.  Its single-texture sibling, which is the
  same program without the `o4` write and without the `T2` write, is
  pixel-exact, so the suspects are the `T2` write inside a program that also
  uses `T0` and `T1`, and the `o4` write in a transforming program.  The next
  experiment is to drop the `T2` write and re-run the GL test's two-unit
  scene; `PARHELIA_HW_TCL_MULTITEX=true` selects the route without a rebuild.
  Until then Quake 3's two-texture world pass keeps the software vertex
  pipeline, which is the majority of its geometry.
* Lighting still runs on the CPU.  The kernel's `warp_lighting` program exists
  and reads `c4` (light direction), `c5` (material) and `c6` (ambient), but
  the matcher only accepts a pass-through colour; a GL-lit vertex program has
  a different NIR shape and is not recognised yet.
* Fog, texgen and the texture matrix are not recognised, so a draw using them
  keeps the software path.
* Polygon offset has no place in the WARP program.  It is a constant-file
  entry `c[0x178] = {factor, units * depth-quantum, 0.5, 786432.0}`
  (`parhelia-polygon-offset-hardware-re.md` §2), read by a resident program
  through `b1`; wiring it up needs the base-register packet, which the kernel
  validator deliberately rejects today.
* User clip planes are block slots `0x16c..0x171` consumed by WARP clip-plane
  fragments, but the binding between those slots and the fragments' registers
  is not closed (`parhelia-raster-clip.md` open item 6), so they stay on the
  software clipper.
* `parhelia_batch.c` skips `parhelia_batch_compact_reglists()` for a
  transforming batch (`!state.transform`), a guard from when the transform
  cadence had never run on hardware.  A transformed batch therefore pays the
  uncompacted register-list cost on top of the per-batch constant group.  Now
  that the cadence is hardware-proven this guard can be re-measured.
* The vendor transform brackets its viewport `mad` with a grid-snap constant
  (`c[b1+8].w` = 1.5*2^19) that the kernel program omits;
  `parhelia-engine-context-bringup.md:5694-5700` attributes a small-primitive
  coverage loss to exactly that difference.  It is the one known functional
  delta between the kernel program and the vendor's.
