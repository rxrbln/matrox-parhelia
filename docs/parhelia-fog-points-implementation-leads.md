# Fog and hardware point size: what the evidence says, before any code

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Written while implementing the fixed-function features the hardware has and
the driver does not.  Logic ops landed the same day; these two did not, and
this is the state they were left in so the next attempt starts from evidence
rather than from the register map's plausible-looking rows.

## Fog

**The hardware has a fog unit.**  `0x1070` is a fog enable (0/1, CONFIRMED in
`parhelia-fragment-ops.md` section 2), the WARP output convention gives
`o10.w` as the fog factor (`parhelia-p690-warp-isa-re.md` section 3.2), and
the vendor uploads fog parameters as constant blocks - index `0x14e` carries
`{1/(end-start), -start/(end-start)}` for linear fog and `0x154` carries
`{start, end/(end-start), 1/(end-start), density*log2e}`, both CONFIRMED in
`parhelia-block-state-map.md`.

**Mesa does not ask for any of it.**  Measured with `PARHELIA_TRACE_FS=1` on a
`glEnable(GL_FOG)` draw: the state tracker lowers fog entirely into the
*fragment* shader, which arrives as `fmul, fadd, fsat, fneg, fadd, fmul, fmul,
fadd, vec4` - the linear factor followed by a mix toward the fog colour.  The
vertex shader writes position only.  So the hardware fog unit is never
reached, and a fogged draw renders unfogged: measured `ffffff` where the mix
wanted `ff0000`.

Every operation in that expression is already supported by the fragment
data-flow compiler (`fsat`, `flrp`, `ffma`, `fmul`, `fadd` - see
`parhelia_fs_compile.c`).  Two things still stop it:

1. the compiled combiner is only bound on the *textured* draw path, so an
   untextured fogged quad never uses it at all;
2. the combiner's inputs are the texture samples `t0..t3` and the vertex
   colours `v0`/`v1`, and the lowered expression needs the interpolated fog
   coordinate.  The hardware has a second colour (`o2`, colour 1, BIN-only in
   the ISA notes) which is the natural carrier.

So the shape of the work is: map the vertex fog output to `o2`, let the
fragment compiler read it as `v1`, and bind the compiled combiner for
untextured draws.  That is ordinary driver work with no unknown registers -
unlike the alternative of driving the hardware fog unit, which additionally
needs the fog *colour* register, and that one is **not identified**:
`parhelia-fragment-ops.md` section 11.3 warns that the ICD's bank-C indices
0x22-0x27 overlap registers MTXPARD treats as DMA control and trap
(`0x108c`, `0x1090`, `0x109c`), and the `fog` capture confirms the warning -
`0x1090` carries addresses there, exactly as it does in the baseline capture.
Do not write that range on a guess.

## Hardware point size

`captures/gl-matrix/points-secs.txt` writes `0x10fc = 0x42400000`, which is
**48.0f**, in a capture whose scene draws points; the baseline capture never
writes that register.  That is a good point-size candidate and it is the first
thing to confirm.

The caution is that this driver already writes `0x10fc` with `0.2f`
(`P3D_R_TEX_UNK10FC`, one of "the three registers only a same-boot diff could
reveal" in `p3d_state.c`), and the same capture also writes `-8160.0f` and
`-0.5f` there.  A register carrying 0.2, 48.0, -0.5 and -8160.0 in one
capture is either windowed by another selector or is not one register's worth
of meaning.  Resolve that before wiring `pipe_rasterizer_state::point_size`
to it: the cheap experiment is a `parhelia-drm-test` probe that draws one
point at several sizes and reads the covered pixels back, with everything else fixed.

Today the driver expands every point larger than a pixel into two triangles in
Gallium's draw module, which is correct and costs CPU.
