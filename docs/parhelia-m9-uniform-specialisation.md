# M9 shader specialisation on uniform values

Neither native backend has control flow: each `compile_impl` refuses a shader
whose entry point is more than one block.  Two shapes that glmark2 and glamor
draw with are nevertheless reachable in **both stages**, because the value
that decides the control flow is a uniform and is therefore known by the time
the draw is submitted:

* an `if` whose condition reads only uniforms and literals, which folds away
  entirely once the values are substituted;
* a `for` loop whose bound is a uniform -- glmark2's
  `loop:fragment-uniform=true` is `for (int i = 0; i < FragmentLoops; i++)` --
  which unrolls once the trip count is constant.

`m9_specialise.c` holds the three passes both stages use.
`m9_nir_find_inlinable_uniforms` marks the constant-buffer dwords of both
shapes, `m9_nir_inline_uniforms` substitutes the values and then folds, and
`nir_opt_loop_unroll` replaces the loop with its body.  Both stages' NIR
options cap the unroll at `max_unroll_iterations = 32`; a bound past what the
cost model will unroll leaves the loop standing and the shader is refused as
"control flow" rather than compiled to the wrong iteration count.

Measured offline with the glmark2 loop fragment shader
(`m9-fs-compile-test --glsl`, `M9_FS_INLINE=<the dword st sends>`, which is
the float encoding of the bound -- see the next section):

| `FragmentLoops` | dword | result |
|---:|---|---|
| not specialised | -- | refused, "control flow" |
| 0 | `0x00000000` | 9 slots |
| 5 | `0x40a00000` | 19 slots |
| 31 | `0x41f80000` | 71 slots |
| 32 | `0x42000000` | 73 slots |
| 33 | `0x42040000` | 75 slots |
| 100 | `0x42c80000` | refused, "control flow" |

## What the uniform's value is, and what the shader reads

The screen advertises no integer support (`shader_caps.integers = false`,
`nir_shader_compiler_options.no_integers`), and Mesa answers that by storing
an integer uniform in constant buffer 0 **as a float**: `uniform_int_float`
in `src/mesa/main/uniform_query.cpp` writes `(float) *isrc`, so
`FragmentLoops = 5` is the dword `0x40a00000`.  A driver that declares
`no_integers` is the one that lowers its own NIR to match -- gallivm does it
for the vertex path the draw module runs (`lp_bld_nir_soa.c`,
`nir->options->no_integers`).  The fragment path did not, so its NIR still
compared with `ige` and read that dword as the integer 1084227584.

MEASURED on the M9128 on 2026-09-19, `M9X_FS_INLINE_TRACE=1` under
`loop:fragment-uniform=true`:

```text
m9x: fs variant: 1 inlinable uniform(s) dw0=40a00000
m9x: context 0: 3446 draws skipped: control flow
```

So the plumbing was already whole -- one inlinable uniform found, its value
delivered -- and the value was the float encoding of 5 while the loop wanted
the integer.  A trip count of 1084227584 is far past
`max_unroll_iterations`, so the loop survived and the shader was refused.

`m9_nir_lower_integers()` runs `nir_lower_int_to_float` on a shader whose
options say `no_integers`, which is what closes it.  One repair goes
with it: NIR types a UBO load's element index as an integer, so the pass
converts the constant indices of a matrix uniform's columns too, and the
index is put back with `f2i32` and folded -- the same fixup gallivm carries
as `lp_nir_no_integer_intrinsic_fixup`.  Without it the `matuni` fixture
resolves constants a gigabyte past the bound buffer (MEASURED: suite 227 of
228, "fragment constants outside the bound constant buffer").

OFFLINE afterwards, `m9-fs-compile-test --glsl` on the glmark2 loop shader
with the value st actually sends:

| `M9_FS_INLINE` | meaning | result |
|---|---|---|
| not specialised | -- | refused, "control flow" |
| `0x40a00000` | float 5.0 | 19 slots |
| `0x41000000` | float 8.0 | 25 slots |
| `0` | float 0.0 | 9 slots |

`caps.call_finalize_nir_in_linker` is set, which is what gets
`prog->info.num_inlinable_uniforms` filled before `st_atom_constbuf.c` reads
it; `loopuni:0`, `loopuni:5` and `loopuni:24` are listed in the fixture
suite.

## The vertex stage: the same shape, one stage later

glmark2's three default `loop` benchmarks were refused on the **vertex**
side, and reading the option names is what settles why.  `vertex-loop` and
`vertex-uniform` both default to **true** in glmark2, and the default
benchmark strings override neither -- they only set `fragment-loop` and
`fragment-uniform` -- so every one of the three reaches the driver with

```glsl
for (int i = 0; i < VertexLoops; i++)
    d = fract(3.0 * d);
```

MEASURED on the M9128 with the coordinate-stream build, `M9X_DEBUG=1`, one
benchmark a process at `duration=2`:

| glmark2 vertex options | `m9x vs:` |
|---|---|
| `vertex-loop=false` | native |
| `vertex-loop=true:vertex-uniform=false` | native |
| `vertex-loop=true:vertex-uniform=true` (the default) | control flow |

So it was never a branch the backend had to learn: it was the same uniform
the fragment stage had already closed, one stage further up.  The vendor
compiler's own control-flow joins remain recovered but unused
(`parhelia-m9-native-control-flow-re.md`); nothing here emits a branch.

### What the vertex path needed that the fragment path did not

1. **`m9_finalize_nir` marks the vertex stage's inlinable uniforms too.**
   Without the mark `st_atom_constbuf.c` never calls
   `set_inlinable_constants` for `MESA_SHADER_VERTEX` and there is no value
   to substitute.  The dwords are found on the integer NIR, before any
   lowering: a loop bound's `load_ubo_vec4` is the same leaf either way.
2. **Only the backend's clone is lowered to floats.**  The shader the state
   tracker hands over is the one the **draw module** runs, and gallivm
   lowers that for itself (`lp_bld_nir_soa.c`); lowering it in
   `finalize_nir` would do that work twice on the path that still carries
   every fallback.  `m9_vs_native_init` therefore calls
   `m9_nir_lower_integers` on `vs->src`, its private clone, and leaves the
   state tracker's shader alone.
3. **One compiled program per value set, not per output layout alone.**
   The fragment stage keys its variant cache on the values among other
   things; the vertex stage had a single program compiled once at
   `create_vs_state` time.  `struct m9_vs_spec` now holds the clone, the
   compiled program, its refusal and its own output-layout variants, and
   `m9_vs_select_spec` points `vs->cur` at the set the context's uniforms
   ask for.  `M9_VS_MAX_SPECS` caps the cache at 16; a further set falls
   back to the draw module rather than evicting a program a queued job still
   names.
4. **`nir_opt_algebraic_late` moved behind the substitution.**  It is the
   pass that fuses a multiply and an add into the hardware's multiply-add,
   and running it before the unroll leaves the unrolled copies unfused.

`M9X_VS_SPECIALISE=0` compiles without substituting, which is the form the
backend refused before, and is what a measurement of this is read against.

OFFLINE, `m9-vs-compile-test`, the glmark2 loop vertex shader:

| case | result |
|---|---|
| `glmark-loop-literal` (bound written as `5`) | 52 slots |
| `glmark-loop-uniform` (not specialised) | refused, "control flow" |
| `glmark-loop-uniform-5` (`dw16 = 0x40a00000`) | 52 slots |

Specialised on the value st sends, the uniform-bounded loop compiles to
exactly the program the literal-bounded one does.  MEASURED on the M9128,
`M9X_DEBUG=1` names it per shader: `m9x vs: native dw16=40a00000`.

### Verified on the card, pixel by pixel

`m9-mesa-glsl` carries `vsloopuni:0`, `vsloopuni:5` and `vsloopuni:24`: the
corner colour contracted by a loop **in the vertex shader** whose bound is
the uniform `n`, then interpolated.  The body is affine and the barycentric
weights sum to one, so contracting the corners and interpolating is the same
value as interpolating and then contracting, which is what the reference
computes.

MEASURED on the M9128 on 2026-09-20, `M9X_DEBUG=1`, one process a case:

```text
vsloopuni:0   0 of 76800 pixels differ: PASS   m9x vs: native dw0=00000000
vsloopuni:5   0 of 76800 pixels differ: PASS   m9x vs: native dw0=40a00000
vsloopuni:24  0 of 76800 pixels differ: PASS   m9x vs: native dw0=41c00000
```

and with `M9X_VS_SPECIALISE=0` the same case is still exact -- the draw
module renders it -- while the debug line reads `m9x vs: control flow`, so
the fixture says which path took the picture as well as that the picture is
right.

The whole corpus is clean on a freshly booted card with the specialisation
on: **280 cases, 280 PASS, 0 FAIL, 0 non-M9, 0 stream timeouts** (MEASURED,
2026-09-20, library md5 `22eff4de`).
