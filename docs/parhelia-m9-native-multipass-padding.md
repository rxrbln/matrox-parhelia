# Native-transform multipass coverage and padding

The two new M9128 CPU controls in
`captures/m9-public-multipass-window-20261007` run an actual public VS3
(two vertex fetches) and PS3 (five fragment fetches). The five fragment
samplers reuse two genuinely published, live texture objects. All sampled
texels and GPU effects remain unspecified. Both controls finish the original
CPU draw/resolve construction without a fabricated GPU completion reply.
They reach the unchanged two-fetch exporter's **report admission** refusal
because it accepts two texture lowerings; these controls produce seven.
This is a focused CPU state proof, not a qualified whole native draw plan.

The current failed GL2 implicit fixture remains a separate hardware fact:
all four user draws fell back to CPU transforms because the fragment image
uses several passes; color is wrong by up to 14 channels while depth is
correct. The existing software ALU evaluator reproduces the compiler's exact
two-pass arithmetic on 16,384 channels. That rules out a simple arithmetic
mismatch in that model, not an interpolation or hardware routing defect.
Neither new control supplies or imports GPU pixels into emulation.

## Original per-pass clip and write-mask producer

Both controls create **two distinct original `181fa0` raster lowerers**,
joined through each lowerer's live `+24` resource to its `+3e8` shadow.
This join separates public raster state from initialization, preprocessing,
and the later mode-9 resolve. Each lowerer's `+20` source object is shared.

| Caller input | Both public raster clip pairs | Final active color mask |
| --- | --- | --- |
| viewport `(0,0,320,240)`, mask 15 | min `0`, max `00f00140` | `ffff` |
| viewport `(17,23,160,120)`, mask 0 | min `00170011`, max `008f00b1` | `0000` |

Original rectangle code `138f2c/138f36/138f81/138f8d` writes the four
source words `+258/+25c/+260/+264`. `18284c/182858/182864/18287d` copies
those words into the joined shadow `+a4/+a8/+ac/+b0`. Emitter `fbf25`
writes header `c5c4f6f5`, followed by those four words: F5/F6 and C4/C5
receive the caller-derived bounds. They are not a GPU-produced bound of
transformed vertices. The two controls retain identical raw vertices and
all six source-compiled native code images.

The final descriptor has exactly one target mapping: input 0 to output
slot 0. `182500` reads its source halfword, and `182535..182545` packs the
mask halfwords before `fbed2` writes AB/AC. The store observer includes
halfword writes; a previous DWORD store is not falsely attributed as the
producer of the whole assembled word. Only **AB low16** is interpreted.
Inactive AB/AC halves retain their raw source values and are not normalized.
The first raster mask remains `ffffffff` in both controls; changing the
application's color-write mask affects the final active target, not every
intermediate producer. All source instruction spans are checked against
original DLL SHA `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

## Viable GPU padding route

The M9128 hardware evidence already retained in
`parhelia-m9-fragment-pass-hardware.md`, section 3bd, establishes that a
one-pixel frame reaches the required geometry extent, that scissored padding
counts, and that depth-rejected padding counts (`conv3padz`). Section 3c
requires **one indexed raster draw**, rather than a second padding draw.
Those are device observations; this CPU proof adds the real clip/mask
producer and does not generalize the undocumented counter implementation.

A viable bounded native producer should append 24 **generated** vertices
forming the target-edge frame, with both index windings. Their clip-space
positions must come from a GPU producer, while the application's raw inputs
retain their original GPU transform. Two sufficient write-suppression
contracts are available:

1. A live clip window strictly inside all four target edges, so every frame
   pixel lies outside that same window. The source-proved window can stay
   unchanged. This does not require computing a transformed geometry bound.
2. Depth `LESS`, a normalized depth buffer, no stencil side effects, and no
   fragment depth output. Frame positions at clip `z=w` give depth 1 and
   must fail the first pass's test. Existing first-pass depth/continuation
   coverage semantics suppress their later writes. LEQUAL, depth disabled,
   reverse comparisons and active queries need separate admission.

The current code cannot safely activate either yet. `m9_split_pad` deliberately
refuses `key->vs`, because it writes already-transformed screen positions and
narrows the clip using a CPU-derived transformed bound. The native path's
records are raw attributes; its existing extent scan must not interpret them
as screen positions. `m9_native_vs_bind_texture` also preserves the one-raster-
pass restriction, including the separate shader-coordinate refusal.

The smallest coherent next implementation is a driver-owned **padding
preprocess run**, sharing the app draw's output fields/maps and index draw.
It needs a source-supported GPU MOV/viewport tail (or a guarded GPU select
in the original program) to produce generated border positions. The live
batch currently allows per-run constants, but its program/stage entries are
shared. Either add explicit bounded per-run stage entries for a generated
padding program suite, or reserve a typed internal padding selector key in
the original suite and supply distinct app/pad constant runs. Neither
approach may fabricate transformed app records, reuse a color-write-zero
state for only part of one indexed raster draw, or assume registers survive
stage reconfiguration. The six-field sampled-position layout and
FIRST2/CONTINUE4/GATHER3/LAST3 ownership must remain unchanged. These precise
interfaces, rather than an environment-variable override, are the remaining
implementation work.

## Checks and reproduction

Run `sh tests/m9-public-multipass-window-20261007/check.sh`. It validates both
live-owner chains and original instruction bytes, equal program/vertex
inputs, both clip pairs, the active mask mapping, and eight malformed-owner,
cardinality, source-writer and GPU-input refusals.

`tools/m9-vendor-multipass-window.py` wraps the unchanged public two-fetch
source helper. Its only changed caller inputs are the public viewport,
COLORWRITEENABLE, five PS sampler bindings and public five-TEXLD/sum shader.
Use the existing routing helper's `--runtime-tools`, `--bootstrap-tools`,
`--pci-inventory`, `--pci-resources` and `--dc-snapshot` inputs; add
`--five-fragment-fetches --varyings 5 --texture-size 64 --combine sub`.
For the full control pass `--viewport 0 0 320 240 --color-mask 15`; for the
partial control pass `--viewport 17 23 160 120 --color-mask 0`. `--proof-out`
is the focused state artifact; `--out` remains the unchanged baseline report
path and is intentionally not claimed as a complete five-fragment-fetch
manifest.
