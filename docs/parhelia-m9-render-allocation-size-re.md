# M9148 render-resource usable extents

The M9 resource-layout constructor has an end-of-image correction which is
significant when a surface ends exactly on both allocation axes: a small
format-dependent number of elements pushes the usable extent into one more
allocation quantum.  It also selects small-resource quanta independently from
the descriptor row span.

Run the hash-pinned checker from this repository:

```sh
python3 tools/m9-render-allocation-evidence.py
```

## Exact constructor formula

The complete Windows constructor at `MxMD.dll` `0x45eb0..0x464ef` has
SHA-256 `76a740184ea94b5413cb2e23630061714c88f94a4e92f977da8fe995a68f8d5c`.
The end correction at `0x463af..0x46419` and byte-size leaf at
`0x46470..0x464e5` are pinned independently by the checker.  In the names used
by the constructor, the formula is:

```text
gap = (alloc_height - raw_height + 1) * alloc_width - raw_width
extra = end_guard ? max(end_pad(element_bits) - gap, 0) : 0
bits = (alloc_width * alloc_height * layers + extra) * element_bits
usable_bytes = align(bits >> 3, allocation_quantum)
```

`end_guard` is argument flag `0x100`.  The allocation captures prove it active
for the resource route under review: using the correction reproduces all 538
layout-bearing requested extents, and 467 of them have a positive correction.
The 128-byte table at `0x465a8`, followed through the jump table at `0x46594`,
gives:

| element bits | 1 | 8 | 16 | 32 | 64 | 128 |
|---:|---:|---:|---:|---:|---:|---:|
| `end_pad` elements | 32 | 8 | 8 | 8 | 4 | 2 |

The multiply uses the aligned allocation dimensions at layout `+0x184/+0x188`
and layer count `+0x130`.  It then converts elements to bytes and rounds to
the byte quantum at `+0x174`.  Descriptor word 1 continues to contain the raw
vertical extent; allocation padding is not encoded there.

## Captured 32-bit vectors

The combined eleven GL2 allocation streams contain 538 matching records and
24 distinct geometry/format tuples.  Their request field `+0x1e8` is the
constructor's usable extent.  The returned field `+0x218` remains identical
in 516 records.  Twenty-two repeated special 1x1 records request `0x200` with
alignment `0x20` and return `0x24000` with alignment `0x1200`; the constructor
formula still reproduces their requested `0x200`.  This is a separate
allocator-policy expansion.  Address spacing likewise belongs to the
allocator and is not substituted for either extent.

For M9148 32-bit category 2, the final horizontal quantum is 32.  Small
resources use vertical quantum 8 and allocation quantum `0x400`; the ordinary
device-mode-zero route uses vertical quantum 32 and allocation quantum
`0x1000`.

| raw dimensions | aligned dimensions | gap / extra | usable bytes |
|---:|---:|---:|---:|
| 16x16 depth | 32x16 | 16 / 0 | `0x800` [CAP] |
| 32x32 color/depth | 32x32 | 0 / 8 | `0x1400` [CAP] |
| 128x128 color/depth | 128x128 | 0 / 8 | `0x11000` [CAP] |
| 320x240 color/depth | 320x256 | 5120 / 0 | `0x50000` [BIN/DER] |

The first three values occur as request and response fields in the pinned GL2
capture.  The last follows from the same constructor with the captured M9148
ordinary table row.  Its `0x50000` result happens to equal the distance between
the captured 320x240 color and depth bases, so the old spacing observation was
numerically right but did not establish the general formula.

## Exact Z16 category-4 route

Format code `0x5c` selects 16 element bits and category 4 in the constructor's
format table.  On the captured M9148, device mode is zero; this is independently
identified by the category-2 ordinary vertical quantum of 32.  Category 4 has:

| state | horizontal allocation quantum | vertical quantum | byte quantum |
|---|---:|---:|---:|
| small | 32 | 4 | `0x200` |
| ordinary | 32 | 32 | `0x800` |

The state predicate at `0x460d8..0x4616a` first applies the non-1-bit width
limit of 64, then compares against twice the ordinary category table entries,
and finally checks the layer count.  For category 4, M9148 mode zero, it is
exactly:

```text
small = width <= 64 && height <= 64 && layers <= 15
```

There is a second packed-tail minimum predicate at `0x4631a..0x4637a`.  On the
ordinary flag-zero route it forces at least 32x16 allocation dimensions when
`width <= 16 && height <= 16 && layers < 15`.  This predicate is distinct from
small/ordinary state selection.

Using `end_pad(16) = 8`, the exact Z16 vectors are:

| raw dimensions | state / aligned dimensions | usable bytes |
|---:|---:|---:|
| 3x3 | small / 32x16 | `0x400` |
| 16x16 | small / 32x16 | `0x400` |
| 32x32 | small / 32x32 | `0x0a00` |
| 128x128 | ordinary / 128x128 | `0x8800` |
| 320x240 | ordinary / 320x256 | `0x28000` |

No saved live allocation uses format `0x5c`; these five values are deterministic
results of the hash-pinned binary constructor and tables, not live allocation
observations.

## The small state selects the address calculator (MEASURED)

The small/ordinary split is not only an allocation quantum.  A surface of at
most 64 by 64 is stored by the mode-2 aperture calculator `m9x659`, the one
`parhelia-m9-blocked-layout-boundary.md` recovers for texture storage; larger
surfaces use the local calculator `m9x474`.

Measured on the M9148 with the `rtt32` and `rtt64` fixtures, which render a
traceable gradient into a 32x32 and a 64x64 texture and sample it back.  With
the target descriptor carrying the ordinary tiled bit, the readback is the
composition `m9x659^-1 . m9x474`: predicting every texel that way reproduces
the captured image exactly, 1024 of 1024 texels at 32x32 (`m9x659` state
depth 1, category 0) and 4096 of 4096 at 64x64 (category 1).  Clearing the
bit for small targets makes both fixtures exact.

Two further facts follow:

* The allocation sizes agree with the same split.  A 16x16 surface is
  allocated `0x800`, which is the `m9x659` footprint; the `m9x474` footprint
  of the same shape is `0xc00`.  Every small vector in the table above is at
  or just above its aperture footprint, and several are below their local
  one.
* The texture-unit kind field does not select the calculator.  Binding a
  32x32 render target as kind `21` instead of `01` left the sampled image
  byte for byte identical, so the sampler picks the calculator from the
  surface geometry.  Only the descriptor the raster target is bound with
  decides how the engine writes.

Driver consequence: `m9_render_surface_layout()` reports the small state and
`m9_draw_params_init()` leaves descriptor bit 21 clear for it, so the target
descriptor is `cc01xxxx` below the boundary and `cc21xxxx` above it.  The CPU
window already took the small kind for these surfaces.

## Clean-room audit

The kernel diagnostic's only render shape is 320x240 RGBA8.  Its current
`320 * align(240,32) * 4 == 0x50000` allocation agrees with the complete
formula for that one vector.

The guarded 1024x768 KMS front buffer has the same aligned-end condition.
The exact constructor result is `0x301000`, one 4-KiB allocation quantum past
the visible `4096 * 768 == 0x300000` bytes.  Current `m9x_dumb_create()` keeps
the visible height and pitch but allocates through `M9_WARM_RENDER_BYTES =
0x301000`; this matches the vendor formula.

Mesa's `m9_render_surface_layout()` now implements the end-gap correction,
the category-specific small vertical quanta, the fixed 32-element row-span
quantum, and the independent 32x16 packed-tail minimum.  For the supported
single-layer 16/32-bit subset its exact geometry is:

```text
small = width <= 64 && height <= 64
alloc_width = align(width, 32)
alloc_height = align(height, small ? (cpp == 4 ? 8 : 4) : 32)
if width <= 16 && height <= 16:
    alloc_width = max(alloc_width, 32)
    alloc_height = max(alloc_height, 16)
```

The usable-extent formula above follows this geometry in both resource
creation and import validation.  The verifier also rejects the prior
provisional implementation, which used a 16-element small horizontal quantum,
used vertical quantum 4 for 32-bit color, and omitted the 32x16 minimum.

The checker includes three negative controls.  Disabling the end guard yields
the incorrect `0x1000` 32x32 RGBA extent; using the provisional 16-wide/4-high
small Z16 geometry misses the 32x16 minimum; and a one-byte mutation in the
pinned end-correction slice changes its hash.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
