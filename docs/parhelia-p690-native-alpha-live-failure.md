# P690 native sampled-alpha live failure

Copyright 2026 Rene Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This note diagnoses the first ioquake3 run with Mesa commit `55fd4aa`, which
binds the application RGBA texture on unit 0, a padded RGB alpha companion on
unit 1, and moves `t1.r` into fragment alpha.  The target was the P690 at
`192.168.1.111`.  The implementation was rolled back after the test; this note
does not change production code.

## Whole-frame degradation coincides with VRAM placement failure

The first capture, `/tmp/q3-native-alpha.png`, still contains the normal world
image plus the known patterned HUD.  Later captures progressively lose passes
and end with large white, black and overexposed areas.  This is not evidence of
a persistent combiner or depth register leaking into later draws.

The kernel recorded an asynchronous placement failure during the native run at
01:39:51:

```
parhelia 0000:01:00.0: async VRAM pin failed: relocation=2 handle=3
size=1228800 pin_count=0 ret=-12
parhelia 0000:01:00.0: async submit allocation failed in VRAM relocation pin
(packets=21 commands=166 relocs=6)
```

The failed 1,228,800-byte object is a 640 by 480 32-bpp render surface.  This
proves severe VRAM placement pressure and matches the time of the degraded
capture.  It does not by itself prove that Mesa lost the draw: the asynchronous
path handles `-ENOMEM` by disabling async/packet packing and retrying the same
batch synchronously after the kernel drains its queue.  If that synchronous
retry also returns `-ENOMEM`, `ph_drain_batch()` copies the error to the
persistent `ctx->submit_error`; draw, clear, transfer and flush then omit all
later work.  That behavior exactly explains successive partially drawn frames,
but a repeat with `PARHELIA_TRACE=1` must record the final submit return to make
the causal chain conclusive.  Either way, the unnecessary companion footprint
is a demonstrated placement regression which must be removed.

The live trace contains 344 unique state lines and 312 unique texture handles.
Counting each handle once, their padded physical dimensions give 7.578125 MiB
for one 32-bpp companion.
`ph_prepare_alpha_companions()` currently allocates both `alpha_rgb_handle`
and `premul_rgb_handle`, even though the one-pass native route binds only the
former.  The unused premultiplied set doubles the derived texture footprint to
15.15625 MiB.  Minimum 64 by 64 padding also makes every 2x2, 4x4, 8x8, 16x16
and 32x32 source consume at least 16 KiB per companion.

The first production correction should therefore make companion preparation
explicit about which derived images are needed.  The native one-pass route
must allocate and upload only `alpha_rgb_handle`; the legacy two-pass route is
the only consumer of `premul_rgb_handle`.  The existing asynchronous
wait-and-synchronous-retry path should be retained.  If the drained synchronous
retry still cannot place one request, emit a diagnostic before setting
`ctx->submit_error`; retrying it indefinitely cannot create another aligned
VRAM slot.  Removing the unused companion set is the first correction to test
against that condition.

## The HUD failure is still a spatial unit-1 sampling problem

The 64 by 64 padding experiment did not remove the horizontal HUD pattern.
The native trace confirms that the affected 32 by 32 resources received 64 by
64 companions and selected the one-pass route, so a missing selector or an
unselected companion does not explain the retained artifact.

The target's `pak0.pk3` also resolves two previously anonymous live hashes.
Computing the driver's FNV-1a value over the decoded BGRA upload bytes gives
`0xba1b38b73dc17fdd` for `gfx/2d/numbers/one_32b.tga` and
`0x822adefe62cfcfde` for `gfx/2d/numbers/zero_32b.tga`.  These are the first two
resources at X positions 2 and 34 in the visibly banded `100` HUD counter.
The corruption therefore changes the spatial silhouette of known source
alpha; it is not an unknown animated resource or an incorrect draw selector.

The passing `--p690-rgb-companion-cutout-probe` is narrower than its summary
line suggests.  Its mask is cleared uniformly to all-zero or all-one and all
three vertices use `(s1,t1) = (0,0)`.  It proves all of the following:

* physical-pipe-2 unit 1 can supply `t1.r`;
* the separate-alpha replicate encoding is correct;
* alpha rejection precedes colour and depth writes; and
* accepted fragments write depth.

It does not test coordinate variation, filtering, wrapping, tiled texel
addressing, or any texel beyond the sample at the origin.  The multitexture
WARP probe likewise keeps the fragment sampler on unit 0 when it tests the
second coordinate attribute.  No retained probe currently verifies a spatial
image fetched through unit 1.

Mesa commit `551317e` adds that missing test to the offscreen companion probe.
It uses a 64 by 64 asymmetric mask whose expected output identifies both axes:

* unequal X and Y cuts plus a diagonal term make a binary mask which cannot
  pass after a constant, transposed, wrapped or incorrectly tiled fetch;
* draw two triangles covering a known rectangle with unit-1 coordinates
  spanning `(0,0)` through `(1,1)`;
* compare every pixel in the first alpha-tested red silhouette; and
* replay opaque green at equal Z under LESS, requiring prior accepted pixels
  to stay red and prior rejected pixels to become green.

The command remains `--p690-rgb-companion-cutout-probe`; success now reports
both `spatial-bad=0` and `spatial-depth-bad=0`.  Unit 0 stays constant during
the spatial pass.  If the result fails, vary the
second-coordinate descriptor/output routing independently of the already
proved physical-pipe-2 TA source.  Only after that test passes should Q3 use
the native route.  Changing padding again or changing the alpha comparator
cannot discriminate the remaining fault because the present uniform probe
already proves those parts while bypassing the failing spatial behavior.
