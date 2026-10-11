# P690 Q3 sampled-alpha: current Mesa delta

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This read-only audit compares the saved working P690 Q3 state with the active
Gallium state builder.  It identifies the remaining difference that matters
for Q3 alpha-tested and source-alpha blended RGBA art.  No target, driver, or
source code was changed.

## Result

The open driver has the proven *final* pieces of the one-texture route, but it
does not have the vendor's texture-stage compiler output.  The missing output
is the only evidence-backed difference that can select sampled alpha on P690.
It cannot safely be guessed as an MMIO bit.

The white rectangles are consistent with this gap: Q3's transparent texels
have white RGB and zero alpha, so routing their alpha as one produces the full
texture-quad rectangle.  This is not a generic blend or RGBA-upload failure.

## Compared state

| item | saved vendor P690 Q3 input | active Mesa P690 path | conclusion |
|---|---|---|---|
| upload/format | `GL_RGBA`; A8R8G8B8 sampler format `0x32` | `PIPE_FORMAT_*8A8_UNORM` selects `0x32` in `parhelia_context.c`; XRGB alone selects `0x33` | the RGBA descriptor is present |
| P690 texture ownership | `0x1200 = 0x00040101` | `p3d_state.c` emits `0x00040101` when `single_texture_pipe` is set for `102b:2539` | correct safe one-pipe route; `0x00040103` is prohibited because it wedges P690 |
| sampler layout | vendor layout-2 example is `0x11e0 = 0x32020040` | tiled texture sets `texelcode = 2`, then packs `fmt`, texelcode and width into `0x11e0` | layout-2 and format agree when `PARHELIA_TEXTURE_TILED=1` is selected |
| final RGB/alpha ALU | texture address plus saturated `MUL t0,v0` halves | `p3d_state.c` co-issues `0x1264/0x1260 = 0x08000002/0x08000000` and `0x126c/0x1268 = 0x01400000` for one-texture `MODULATE` | final alpha instructions are not omitted |
| alpha test and blend | comparator is `0x12e4`; Q3 uses source-alpha over | Mesa packs the comparator and maps `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` to the proven `0x12e8 = 0x0080eced` | vertex-alpha cutout/blend works, so these downstream stages work |
| stage-alpha source | key `0x0e`: `ALPHAOP=SELECTARG1`, `ALPHAARG1=TEXTURE`, `ALPHAARG2=DIFFUSE` | Mesa directly emits the final `MUL t0.a,v0.a` instruction; it does not submit a key-`0x0e` equivalent or its compiled output | this is the material unresolved difference |

For the normal Q3 vertices, `v0.a` is one, so `MUL(t0.a,v0.a)` and
`SELECTARG1(TEXTURE)` would have the same mathematical result *after* texture
alpha arrived.  The direct P690 A=0 probe disproves that arrival: it uses the
safe pipe, layout-2 A8R8G8B8 descriptor, known alpha ALU and comparator, yet
the RGB still paints; changing only vertex alpha to zero rejects it.  Therefore
changing the algebraic final ALU from `MUL` to `SELECT` is not a safe fix.

Mesa normally does not write the vendor example cache word
`0x11e8 = 0x00500000`; the bounded `--depth-alpha-cutout` diagnostic does.
The existing P690 result is unchanged with that value, so it is an observed
difference but not an alpha-routing candidate.

## Why the compiler boundary is real

The saved Linux vendor object accepts key-`0x0e` fields 4--6 into a per-stage
descriptor, copies them to a compiled record at `+0x10/+0x14/+0x18`, then
uses `mtx950`/`mtx713` to populate dynamically allocated sparse-map nodes.
The saved Q3 trace retains the input record but no resulting final command
buffer.  The Windows D3D binary independently lowers alpha stage arguments
into intermediate records.  Neither retained binary names a final P3D
register or a fixed context offset for that selected alpha route.

Thus direct injection of key `0x0e`, a `0x46` object, a cache value, or a
sampler-pipe variation would conflate driver-private context with hardware
state.  It would not be a controlled test, and paired pipe `0x00040103` has
already wedged the target.

## Safest next work

For a native implementation, obtain one vendor P690 capture on a separate
known-good stack.  Render matched one-unit, layout-2 RGBA A=255 and A=0 draws
with fixed clear colour, vertex alpha one, `GL_MODULATE`, and alpha test (then
alpha-over).  Preserve, for every final draw, key `0x0e`, its copied record,
the selected `mtx950` map-node values, key `0x46`, final sampler/combiner/
alpha packets, a result pixel and a retired fence.  Diff A=255 against A=0.
Only a result-correlated complete delta can enter a private diagnostic.

Until then, the only correctness implementation is the existing opt-in,
tightly fingerprinted CPU fallbacks: `PARHELIA_CPU_ALPHA_CUTOUT` for the two
measured depth-writing cutout images and `PARHELIA_CPU_ALPHA_BLEND` for the
measured small HUD atlas primitives.  They should not be generalized to world
alpha-over draws without proving filtering, clipping, depth/stencil ordering,
blend math, and tiled-target coherency.

Related evidence: `parhelia-p690-q3-sampled-alpha-final-audit.md`,
`parhelia-p690-sampler-alpha-emission-re.md`,
`parhelia-p690-texture-alpha-context-re.md`, and
`parhelia-q3-white-rectangle-alpha-route-audit.md`.
