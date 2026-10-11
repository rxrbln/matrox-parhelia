# Parhelia Q3 depth UI additive blend audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

The remaining depth-bound one-texture Q3 UI trace carries blend-factor values
`5, 2` for both RGB and alpha with `ADD`.  These are the vendor intermediate
(`D3DBLEND`) values, not Parhelia register nibbles and not Gallium values:

| value | intermediate name | GL name | Parhelia RGB/alpha nibble |
|---:|---|---|---|
| 5 | `D3DBLEND_SRCALPHA` | `GL_SRC_ALPHA` | `6 / 6` |
| 2 | `D3DBLEND_ONE` | `GL_ONE` | `1 / 1` |

Thus the trace is `glBlendFunc(GL_SRC_ALPHA, GL_ONE)` with `GL_FUNC_ADD`, not
the usual alpha-over form (`SRC_ALPHA`, `ONE_MINUS_SRC_ALPHA`).  The direct
hardware encoding is:

```
P3D_R_BLENDCTL (0x12e8) = 0x00802c2d
```

This agrees with the Windows factor translator in `mtxpar.dll`/PARD:
`D3DBLEND_SRCALPHA` maps to nibble `6` and `D3DBLEND_ONE` to nibble `1` for
both pipes.  The existing ioquake capture also records the GL alpha-over call
(`0x302`, `0x303`), so the two Q3 UI blend shapes must remain separately
identified rather than treating vendor factor `5` as a hardware nibble.

Gallium represents these factors as `PIPE_BLENDFACTOR_SRC_ALPHA = 3` and
`PIPE_BLENDFACTOR_ONE = 1`; its inversion bit makes
`PIPE_BLENDFACTOR_INV_SRC_ALPHA = 19`.  The values therefore cannot be
compared directly with the trace's `5,2`.

## Diagnostic scope

The safe probe is a depth-tested, single-texture, RGBA-vertex triangle using
the already verified stream:

```
x, y, z, r, g, b, a, s, t
0x21c00000, 0x23c50030, 0xa0c70070, 0
```

It must bind an opaque white RGBA texture, clear a private colour target to
`(32,64,96)`, use red vertices at alpha `0.5`, and enable Z24 `LESS`.  The
sampled result is approximately `(160,64,96)`: source red contributes 127.5,
while the destination is retained unchanged.  An alpha-over packet would
instead give approximately `(144,32,48)`, making the test decisive even with
Parhelia's normal dither rounding.  A replay after clearing Z to zero must
leave the result unchanged.

Mesa selects this form only when every fixed-function condition of the probe
is present: one texture, `MODULATE`, vertex colour, enabled depth compare, no
alpha test, `ADD`, and matching `SRC_ALPHA, ONE` factors on RGB and alpha.
Other blend states retain the RGB stream.

## P690 result

`parhelia-drm-test --depth-vertex-alpha-add /dev/dri/card0` passed on the
target with the temporary staged driver:

```
PASS: GPU clear 0x5a39c7 fence=0x505e0 mismatches=0/65536
PASS: GPU clear 0x204060 fence=0x505f8 mismatches=0/65536
PASS: depth-alpha LESS occlusion preserved sample 0xa04060 fence=0x50658
PASS: depth-vertex-alpha blend fence=0x50658 sample=0xa04060
      expected=0xa04060 untouched=0x204060
```

The exact sample confirms the source contributes half-red `(128,0,0)` while
the destination `(32,64,96)` remains whole.  The Z replay confirms the packet
also retains the established depth-test behaviour.  This proves the Q3 blend
shape on hardware and supports the narrow Mesa gate above; it does not cover a
broader blend or texture path.

## Evidence

- `captures/ioq3-p690-working-draw.txt:324` records the Q3 GL blend call.
- `docs/parhelia-gl-feature-map.md` maps the vendor values 5 and 2 to source
  alpha and one.
- `docs/parhelia-blend-alpha-encoding.md` gives the complete Windows/Linux
  factor translation and `0x00802c2d` for `GL_SRC_ALPHA, GL_ONE`.
- `src/util/blend.h` defines the Gallium factor enum and inversion bit.
