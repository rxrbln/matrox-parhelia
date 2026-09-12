# Q3 white-rectangle alpha-route audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This is an offline audit of the remaining opaque white rectangles in the
saved P690 Q3DM1 screenshots.  It does not change Mesa, the kernel, the
target, or a vendor driver.

## Observations

The comparable 1024x768 captures show an unambiguous difference:

| image | opaque near-white connected regions (pixels, bounds) |
|---|---|
| `q3-target-matched.png` | 2560 `(432,502)-(592,518)`, 1664 `(200,144)-(304,160)`, 1024 `(763,146)-(827,162)`, 896 `(312,144)-(368,160)`, and smaller 16-pixel-high rectangles |
| `q3-alpha-constrained.png` | the same regions at the same bounds |
| `q3-soft.png` | no corresponding rectangle; its largest near-white regions are individual glyph/highlight fragments, at most 54 pixels |

The rectangles have full texture-quad bounds and 16-pixel-high spans.  The
software reference instead retains only the intended glyph or highlight
pixels inside those bounds.  This rules out an incidental presentation tear
or a general R/B swap.  It is consistent with RGBA art whose RGB is white in
transparent texels being composited as though sample alpha were one.

The 640x480 screenshots show the same pattern around the arch/world details:
`q3-final-gate.png` contains five full white rectangles, whereas
`q3-cpu-alpha-fixed.png` and `q3-native-alpha.png` leave only 288 near-white
pixels, without those large rectangles.  Those captures are not camera-locked
and are supporting evidence only.

## State and code evidence

Q3 GL1 renders generic stages by supplying a four-byte colour array,
binding one texture, calling `GL_State(pStage->stateBits)`, and drawing in
`renderergl1/tr_shade.c:928-1000`.  Its shader parser turns `blendFunc blend`
into `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` and disables the depth write unless the
shader explicitly requests it (`tr_shader.c:812-836`).  Fog uses that same
source-alpha pair (`tr_shade.c:599-601`).  These are precisely the state
shapes that render transparent UI, sprites, and blended world details.

Mesa maps that blend pair to the proven Parhelia factor pair
`SRC_ALPHA/ONE_MINUS_SRC_ALPHA`; the emitted register value is `0x0080eced`.
Its single-texture RGBA vertex path uses the verified 8-dword stream
`x,y,r,g,b,a,s,t` and descriptors `0x20c00000, 0x23c50020, 0xa0c70060`.
The isolated `--vertex-alpha-blend` diagnostic passed with a half-alpha red
vertex over `(32,64,96)`, producing the expected alpha-over result.  Thus the
blend factors, vertex-alpha fetch, texture coordinates, and colour target
write are not the likely source of the opaque rectangular backgrounds.

The independent P690 alpha-cutout diagnostic is decisive for the remaining
input.  With a tiled A8R8G8B8 (`fmt 0x32`) texture containing green RGB and
`A=0`, the pixel still painted under the proven comparator and the known
MODULATE alpha combiner.  Replacing only vertex alpha with zero rejects the
same pixel.  The archive confirms that byte three is retained on `GL_RGBA`
upload, but provides no complete sampler/context state that makes texture
alpha reach the fragment alpha pipe.  This is the same input that source-alpha
blending reads.  It explains white transparent areas without invoking an
unproven texture-format conversion or a blend-register error.

The conclusion is a strongly supported diagnosis, not an exact per-quad
trace: the saved Q3 capture lacks material names and a final command buffer
for each rectangle.  A trace that records texture hash, format, blend factors,
and a screenshot marker at each affected Q3 stage would close that remaining
attribution gap.

## Safe next test

Do not change normal Mesa alpha selection, sampler registers, the pipe value,
or texture format.  The narrowly safe next test is a private extension of the
existing `parhelia-drm-test --vertex-alpha-blend` packet, which already uses a
retiring P690 state:

1. retain its one texture, `MODULATE`, RGBA descriptor, source-alpha blend,
   and clear colour `(32,64,96)`;
2. use a white `A=0` texture with vertex alpha one; the sample must retain the
   clear colour;
3. change only texture alpha to `A=255`; the sample must become white;
4. replay opaque texture with vertex alpha zero; the sample must again retain
   the clear colour.

Run the test first in the same layout-2 backing selected by the established
`PARHELIA_TEXTURE_TILED=1` Q3 run.  A separate layout-0 instance may then
test ordinary small UI backing.  No new register values or sampler bits are
needed, and the result distinguishes a failed texture-alpha contribution from
the already-proven vertex-alpha/blend path.

Even a failure of case 2 does not supply a hardware implementation.  It only
confirms the existing diagnosis.  A practical fallback would need a separately
validated, tightly fingerprinted CPU alpha-blend compositor with correct
filtering, clipping, depth ordering, blend math, and tiled target coherency;
the existing CPU cutout path cannot be widened to those draws without that
proof.
