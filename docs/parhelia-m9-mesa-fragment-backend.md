<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9148 NIR fragment backend in the m9x Mesa driver

The clean-room m9x Gallium driver translates NIR fragment shaders into native
M9148 fragment programs (`src/gallium/drivers/m9x/m9_fs_compile.c`).  Vertex
processing stays on the CPU draw module.  The backend emits only instruction
forms present in the vendor corpus
(`captures/m9x-vendor-glsl-programs-20260915`, analysed in
`parhelia-m9-native-glsl-corpus.md`).  Any other shape makes the compile fail,
and the driver then skips the draw with a single warning.

Labels: **OFFLINE** means verified by the host test against vendor bytes.
**DERIVED** means a rule fitted to the corpus.  **UNTESTED** means not yet
run on hardware.  Nothing in this note was run on the M9 target.

## 1. Result (OFFLINE)

`m9-fs-compile-test` (meson suite `m9x`) runs these steps for every corpus
case with a fragment shader:

1. compile it with Mesa's GLSL compiler, using the driver's
   `m9_fs_nir_options`;
2. link it;
3. lower it as st/mesa does (uniforms to `load_ubo_vec4`, samplers);
4. lower its integers to floats, as `m9_fs_lower_integers` does in the
   driver's `finalize_nir` (section 7);
5. run the backend;
6. compare the program with the captured slots from raster `c3` up to the
   vertex entry, and the constants bit for bit, with uniform values taken from
   the harness inputs.

| | count |
|---|---:|
| corpus fragment shaders compiled | 83 |
| accepted by the backend | 23 |
| byte-exact program and constants | 22 |
| raster `88` reproduced | 21 |
| required cases exact | 11 / 11 |

The test also compiles glamor's 99 GLSL programs against
`captures/m9x-vendor-glamor-shaders-20260915`
(`parhelia-m9-glamor-shader-coverage.md`).  Of those, 89 are accepted,
29 are byte-exact and 60 are proven equivalent with
`tools/m9-native-eval.py`.  The required set grows to 23 cases, all exact.
Inlining uniforms that select control flow also makes `fs-loop-break`
acceptable: it is specialized on `f` and compiles to a chain whose layout
differs from the vendor loop.  That chain is not checked for equivalence.

The required cases are all exact: `fs-const`, `fs-uniform`, `fs-color`,
`pr-color-mul-uniform`, `pr-color-add-uniform`, `fs-tex2d`, `pr-tex-mul-color`,
`pr-temp-chain`, `fs-discard-all`, `pr-two-texcoords` and `fs-fragcoord`.

The other exact cases are `fs-varying`, `fs-texcoord`, `fs-mul`, `fs-add`,
`fs-mad`, `fs-mix`, `fs-uniform-float`, `fs-tex2d-mod`, `fs-tex2d-unit1`,
`fs-tex2d-two` and `pr-fragcoord-300`.

* **`fs-loop-fixed` (accepted, not exact).**  NIR unrolls the three-iteration
  loop into a MUL/ADD chain of 15 slots.  The vendor keeps a 44-slot loop
  with control slots.  The chain uses only exact forms, so it is semantically
  equivalent, but its layout differs.
* **`88` not reproduced.**  `pr-two-texcoords` (vendor `0310dff4`, backend
  `0310dff0`) and `fs-loop-fixed`.  The meaning of `88` bit 2 is not decoded.

## 2. Supported NIR

* **Output.**  One `store_output` to `FRAG_RESULT_COLOR`/`DATA0` with mask
  `xyzw`, in a single block without control flow.
* **Varyings.**  4-component `load_interpolated_input` values.  Slots are
  assigned in use order.  At most two raster streams (varyings plus texture
  coordinates) are allowed, because the kernel whitelist accepts only raster
  streams `00..04`.
* **Arithmetic.**
  * `fmul`: register × constant and register × register;
  * `fadd`: register + constant, register + register, and register − literal
    when the literal is negative and equal in all components;
  * `flrp` with a constant `t`, emitted as the vendor SUB + MAD (`op2=02`)
    pair.
* **Constants.**
  * vec4 uniforms (`load_ubo_vec4`) as xy/zw entries;
  * literals as `(v, v)` or `(x, y)` pairs.
* **Direct moves.**  A uniform or literal colour; an interpolated varying
  written straight to `0x2d`; the texel moves `R,G <- 00`, `B,A <- 02`.
* **Templates** (vendor sequences reproduced verbatim):
  * a constructor of four distinct scalar literals or float uniforms
    (`fs-const`, 8 slots; entries in y, w, x, z order);
  * `vec4(gl_FragCoord.xy * s, gl_FragCoord.z | literal, literal)`, 7 slots,
    with the constant `(0, H-1)` resolved per drawable;
  * an unconditional `discard` followed by a varying passthrough (8 slots).
* **Textures.**  `texture2D` with a varying coordinate, up to four fetches
  per pass.  The last sampler used takes texel pair `00/02`.  A unit is a
  fetch, not a texture: fetches that agree in sampler, coordinate stream and
  transform share one unit, and a fifth distinct fetch is refused, because
  the vendor splits it into another framebuffer pass.  `shadow2D` is
  supported, with the comparison reference as the third component of the
  coordinate stream (`parhelia-m9-mesa-depth-textures.md`).  A projective
  fetch is accepted only when the projector is component 3 of the same
  varying, and the driver then divides per vertex, which is exact only while
  the projector is constant over a primitive; the sampler does no divide of
  its own (`parhelia-m9-mesa-projective-textures.md`, MEASURED).

## 3. Vendor layout rules (DERIVED, reproduced OFFLINE)

* **Lanes.**  Each vector operation is a zw slot followed by an xy slot.
  Destination `0x2d` uses `op0` 1d (R,G) and 1e (B,A); temporaries use `1c`.
* **Order.**
  * Interpolations come first, highest varying slot first.
  * Then operations by expression depth.
  * With one operation level both output lanes are written directly.
    Otherwise the final zw lane goes to a temporary and a trailing
    `B,A <- reg` move is appended.
* **Register allocation, scanning backward over the slots.**
  1. A value is allocated at its last use and freed at its definition.
  2. Computed values take `11`, then `13`.  A value read by the last slot, or
     one found with both busy, takes a fresh general register.  General
     registers are numbered forward, starting after the registers used by
     interpolants and texels.
  3. An interpolated xy lane read by the `R,G` slot takes `1a`.  Other
     interpolants take the base pair (`00/01`, or `04/05` behind a texture),
     then `1a..1f`.
* **Constants.**
  * Entries are numbered in backward order of first use.
  * Equal literals share an entry, except inside the gl_FragCoord template.
  * The span ends with `(0,1), (1,0.5)`.
* **End marker.**  `W1` bit 30 is set on slot `max(n,4)-4`.  Programs shorter
  than 4 slots are padded with `ffff801c a01fc00f`.
* **Operand `e`.**  Interpolation and register–register slots carry
  `e = 0x3f` when the program has constants and `0` otherwise.  Moves to
  `0x2d` always carry `0x7f`.
* **Raster `88`.**
  * Bits 27:24: varying slot mask.  Bit 21: second texture.  Bit 19:
    gl_FragCoord.  Bit 20: set for GLSL forms.
  * Bits 15:4 flag register pairs `08/09 .. 1e/1f` that the program leaves
    unused.  `10..13` never count, and interpolating straight into `0x2d`
    counts as `1a`.
  * Low nibble: `3` for one texture (clears bit 4), `f` for two (clears bits
    6..4), `c` for discard (clears bits 5..4).
* **Raster declaration.**
  * Varying slot `k` is attribute `k+1` on stream `k`, declared as the pair
    `0d88`/`06b6`.  Bit 31 (the flat link) is set for colours.
  * Position, `c` and `d` follow as `0688`, `31b6` (stride 6) and `0688`.
  * `22` bits 11:8 hold the declaration word count minus one.
  * Strides are packed pairwise into `10..12`.
  * `m9-batch-test` checks these words against the corpus.

## 4. Coverage as of 2026-10-02

The list this section used to carry predates the piglit campaign of
2026-10-02; most of it is implemented now.  Details and measurements are in
the m9x commit messages and `src/gallium/drivers/m9x/M9_BRINGUP.md`.

**Added (MEASURED on the M9148 unless noted):**

* booleans: `b2b1`/bool uniforms as select conditions, `ball`/`bany`
  reductions scalarised, boolean `ieq`/`ine`/`ixor` compared as floats,
  `fsign`/`ftrunc` lowered in NIR;
* `gl_FrontFacing` (fs-frontfacing form, `op2 = 0x02` on `7b` against
  (-0.5, 0.5); CW front is a `face_flip` variant), all 12 window/FBO x
  native/draw-module x winding cases;
* `gl_FragDepth`: `op2 = 0x19` class 3 clamp to [0,1], then class 0 into
  destination `0x21`, with raster `r89` fifth class field 1 and `ef` 0x6800;
* 1-D textures as planes one texel high; constant/uniform coordinates
  through the per-vertex transform bias; coordinate components gathered in
  any order (shadow reference not in z, cube/volume order, texture2DProj
  vec3); a literal/uniform bias folded into the sampler LOD bias;
* derivatives through the captured DDX/DDY slot (`ffff9100 a00fc00f`, then
  `W1 = 8c020c00 | y<<14 | aswz<<8 | aswz<<4`).  They are **per 2x2 quad**:
  dFdx from the quad's first engine row, dFdy from its left column, so the
  GL_NICEST per-pixel accuracy tests stay WARN (hardware);
* computed discard (section 4a below);
* blending: SUBTRACT, REVERSE_SUBTRACT, MIN, MAX, constant-colour factors
  (constant re-upload, no recompile), separate RGB/alpha factors,
  SRC_ALPHA_SATURATE; destination alpha of an alpha-less target reads one;
  several colour targets are blended one target per pass;
* sRGB texture decode after the fetch (`c <= 0.04045 ? c/12.92 :
  exp2(2.4*log2(...))`, ~57 slots per unit; worst error 0.23 of an 8-bit
  step), which gives GL 2.1.

**Still refused:** more than six interpolated vec4 inputs in one pass (an
engine limit; the vendor splits passes), out of interpolation registers in
the splitter, control flow that
specialisation does not fold, "no value splits this shader", a per-fragment
texture bias (the literal-bias two-pass route is CAPTURED; varying bias
and mip-selection semantics still need the prepared vendor probes in
`parhelia-m9-txb-capture-boundary.md`, and remain unimplemented), the shadow reference clamp
for fixed-point depth, dual-source blending, sRGB render targets, and logic
ops (captured, `parhelia-m9-logic-op.md`, not yet implemented).

### 4a. The kill slot (MEASURED 2026-10-02)

The GLSL kill is `w0_make(a, 7f, 7f, 0x40, cls, 0x1c)` with W1
`0xafdfc000 | aswz<<8 | a>>3`.  It kills where `(0 cls v)` holds for the
scalar the a-swizzle picks: cls0 v>0, cls1 v<=0, cls2 v>=0, cls3 v<0,
cls4 v==0, cls5 v!=0.  No predicate setup is needed; the fixed-function
alpha kill (b operand, W1 `a3c0000f`) is a different instruction.  A shader
that discards every fragment skips its draw (exact by the GL spec).

**Open artifact:** in up to three lanes of a 2x2 quad the kill result of the
quad two places earlier in the span is applied (lane x odd, y even is never
affected), 60-160 pixels per 64^2/128^2 draw.  It also hits the alpha-test
path and a replay of the vendor's own fs-discard-cond, whose vendor
readback is exact -- so some engine state differs from the vendor's.  The
ruled-out list is in `M9_BRINGUP.md` and `BUGS.md`; an MMIO snapshot of the
vendor stack rendering fs-discard-cond (on the 2.6.30 vendor host, not
nano) would settle it.

**Never write index `c7` (0x131c) mid-job:** it stalled the engine
(2026-10-02, only reachable by relaxing the kernel validator).

### 4b. Fragment register roles

| register | role | basis |
|---|---|---|
| `00`, `02` | texture results arrive here; there is no separate texture-load slot | CAPTURED |
| `10`..`13` | scratch and interpolation | established |
| `18`..`1b` | destination colour when blending or for logic ops; a blended program's general run stops at `0x17` | MEASURED |
| `1c` | interpolation spare | established |
| `21` | depth output (`gl_FragDepth`) | CAPTURED |
| `7b` | front-facing value | CAPTURED |
| `7d`, `7e` | specials the logic-op tail reads | CAPTURED |

## 5. Driver wiring (UNTESTED)

* **Shader creation.**  `create_fs_state` keeps the verified pass-through
  shader unchanged.  Any other shader is compiled on a NIR clone, and its own
  VRAM `c2` BO receives `[fragment slots][captured vertex program slots
  4..39]`.  The vertex program is assumed position independent: the vendor
  images show the same vertex code at entries 4, 9 and 0x13.
* **Preprocessing pass.**  `c3` is the fragment slot count.
* **Raster pass.**
  * `c3 = 0` and `88` from the backend.
  * Constants are resolved from constant buffer 0 at draw time and written to
    the system BO at `+0x1000`, with `8c = 00500000 | count << 8` and
    `8e - 8d = 8 * count`.
  * The declaration, strides and `22` come from `m9_raster_fs_init`.
* **Varyings.**  `COL0` uses the pass-through colour view 0.  Other varyings
  are fetched from the draw module output (TGSI semantic of the varying slot)
  and copied as float4 records into CPU transient views 4 and 5.  The CPU
  view form was measured with the REPLACE texcoord probe.
* **Job batching.**  The job key includes the fragment state and the resolved
  constants.
* **GLSL level.**  The screen already reports `glsl_feature_level` 120.  It is
  left unchanged: an unsupported GLSL shader fails compilation and its draws
  are skipped with a warning, so no incorrect program is submitted.

## 5a. Blending (OFFLINE)

The M9 has no fixed blend stage: the vendor lowers `glBlendFunc` into the
fragment program.  `captures/m9x-vendor-glamor-blend-20260915` holds glamor's
solid and texture composite programs under every factor pair glamor's
composite table and component-alpha remapping produce (sources ZERO, ONE,
DST_ALPHA, ONE_MINUS_DST_ALPHA × destinations ZERO, ONE, SRC_ALPHA,
ONE_MINUS_SRC_ALPHA, SRC_COLOR, ONE_MINUS_SRC_COLOR), cleared to alpha 0.5
(`tools/m9x-glamor-blend-capture.sh`, `tools/m9-glamor-blend-analyze.py`).

**Vendor rules (CAPTURED)**

| | |
|---|---|
| ONE/ZERO | the unblended program, `f0=c4008400` |
| ZERO/ZERO | blended program, `f0=c4008400` |
| any other pair | `f0=c5008200` |
| colour writes | go to temporaries with `cls=4` instead of `0x2d` |
| destination | read from `18` (R,G) and `1a` (B,A) |
| `88` | bit 4 cleared; bit 5 also cleared when temporaries reach `08` |
| constants | a `(0,0)` pair precedes the `(0,1),(1,0.5)` tail; blend operations use the tail as the constants 0 and 1 |

**Instruction semantics (MEASURED).**  `m9-native-eval.py --validate` reproduces
the vendor readback of 23 of the 24 solid-source cases with these forms:

* `op2 02` MAD: `a·b + c`;
* `op2 05`: `a·b + c·(1 − b')`;
* `op2 07`: `a·K + c·(1 − b')`;
* `op2 0a`: `a·b + c·b'`;
* `op2 04` ADD, loading `K − reg.a`;
* `cls 4` saturates.

Here `b'` is operand b read through `W1[11:10]`.  ONE_MINUS_DST_ALPHA/ZERO uses
an `op2 02` form with `W1[28]` set that is not modelled.  Texture cases use the
vendor rel_sampler stub, which has `cls 5` sample slots, and are not evaluated.

**Backend (OFFLINE).**
* `m9_fs_compile()` takes an optional `struct m9_fs_blend`.
* After encoding, colour writes are redirected to two temporaries with
  `cls 4`.  A temporary is free if no slot names it at all, or if the last
  slot that names it is at or before the one writing that colour lane: the
  lanes are written last, so such a register is dead from there on.  Taking
  only the never-named ones refused every shader that used the whole pool --
  glmark2's `jellyfish` among them, and thirteen of the 182 corpus shaders
  under `SRC_ALPHA/INV_SRC_ALPHA` (OFFLINE, 133 supported to 146).
* Each lane then gets `S·fs + D·fd`, built only from the forms above:
  * fused `op2 07` for a constant source factor with an inverted destination
    factor;
  * `op2 05` when the inverted operand is the source lane;
  * MUL/MAD register forms otherwise;
  * a `K − reg.a` temporary for other inverted alpha factors.
* Programs using registers `18..1b`, and the stipple/select constant
  templates, refuse blending.  Interpolants avoid `1a/1b` while blending.
* `M9_FS_BLEND=src:dst m9-fs-compile-test` plus `m9-native-eval.py --compare
  --shaders` gives EQUIV for glamor's solid composite program in 23 pairs.
  ONE_MINUS_DST_ALPHA/ZERO is unverified, because its vendor reference uses
  the unmodelled form.

**Driver (UNTESTED).**
* Each fragment state compiles one variant per blend state on first use, with
  its own `c2` image.  The job key carries the variant.
* Supported:
  * ADD equations with equal RGB and alpha factors (SRC_COLOR in alpha counts
    as SRC_ALPHA);
  * factors ZERO, ONE, SRC_ALPHA, DST_ALPHA, SRC_COLOR and their inverses.
* Not supported:
  * ONE_MINUS_SRC_COLOR as a source factor.
  * Other equations, separate factors and constant factors.  Those draws are
    skipped with a warning, although the screen reports
    `blend_equation_separate` for the ES 2.0 version glamor requires.  The
    captured FF equations (`parhelia-m9-gl15-ff-capture.md`) are the source
    for completing this.
* Blended lines keep `f0=c8008400`; that combination is not captured.

## 6. Hardware test plan, safest first

1. `fs-uniform`: a constant colour move only, with no streams beyond the
   verified set.
2. `fs-color` through the backend (GLSL cls 0 form rather than the fixed
   cls 4 image).  Compare against the pass-through readback.
3. `pr-color-mul-uniform` / `pr-color-add-uniform`: temporaries `00/1a`.
   Traceable per-pixel check (corner colours ×/+ uniform).
4. `pr-temp-chain`, `fs-mix`: scratch `11/13`, a general register and the
   trailing move.
5. `fs-const`, `fs-uniform-float`: constant moves into `00/01` through
   `11/13` (the vendor form, not the locked-up `1c` form).
6. `pr-two-texcoords`: a second stream from CPU view 4 with a traceable
   coordinate pattern; check `88` bit 2 against the vendor value.
7. `fs-discard-all`: KIL; the target must stay cleared.
8. Enable textures only after the texture storage layout is measured.

For each step: a lock section, one job, readback through `SURFACE_ACCESS`, and
a requester retirement check before the next case.

## Reproduction

```sh
python3 src/gallium/drivers/m9x/m9_fs_corpus_gen.py \
    ~/src/parhelia/xf86-video-parhelia > src/gallium/drivers/m9x/m9_fs_corpus.h
/home/ai/tmp/m9x-ninja.sh
objs-m9x/src/gallium/drivers/m9x/m9-fs-compile-test      # M9_FS_VERBOSE=1 for diffs
M9_FS_DUMP=pr-temp-chain objs-m9x/src/gallium/drivers/m9x/m9-fs-compile-test
```

## 7. Integers (MEASURED)

The screen declares `no_integers`, and Mesa answers that by storing an
integer uniform in the constant buffer **as a float**
(`uniform_int_float`, `src/mesa/main/uniform_query.cpp`).  A driver that
declares it is the one that lowers its own NIR to match: gallivm does it for
the vertex path the draw module runs, and `m9_fs_lower_integers()` does it
for the fragment path, in `finalize_nir` so that every later pass -- uniform
specialisation included -- sees the float form.

NIR types a UBO load's element index as an integer, so the pass converts the
constant column indices of a matrix uniform too; they address the buffer, and
are put back with `f2i32` and folded.  Without that repair the `matuni`
fixture resolves constants past the bound buffer (MEASURED on the M9128).

Against the build before the lowering, the whole 182-case corpus differs in
three lines: `fs-loop-uniform` compiles instead of being refused for control
flow, `glamor/xv-uyvy`'s refusal moves from `i2f32` to `ftrunc`, and the
tally goes from 157 to 158 supported.  59 exact and required exact 23/23 are
unchanged, so no program the vendor captured is altered by it.
