# M9 native polygon offset: captured D0/D1 recipe

2026-10-06. This evidence is **M9**, using the saved Linux M9 raster capture
and MSerX MxMD.dll SHA256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
The original Parhelia 0528 capture in `native-geometry-20261004` uses another
command grammar and is not the source of this M9 implementation.

## Previously missed native command pair

`captures/m9x-linux-gl15-ff-20260915/raster/batch.tgz`, archive SHA256
`832c3972e2a0b8507dddbe1c27df111efb518f2b8321e8d79a7baa34a23806ab`,
contains the offset triangle in execution 13 of the five `offset_fill_*`
cases; the no-offset control is execution 12. The older raster summary did
not list D0/D1. They are present in the actual command bytes:

| API factor | API units | D0 units | D1 factor |
|---:|---:|---|---|
| 0 | 2 | `34000001` | `00000000` |
| 0 | 8 | `35000001` | `00000000` |
| 0 | -4 | `b4800001` | `00000000` |
| 1 | 0 | `00000000` | `3f800000` |
| -2 | 1 | `33800001` | `c0000000` |

The packet is exactly `4545d1d0 units factor`, after the raster declaration
and before linked state at index 69. D0 is the IEEE float result of
`units / 16777215.0f`; the denominator is **2^24-1**, so the observed low bit
must remain one. Rounding to a power-of-two reciprocal loses that bit.
D1 is the unchanged IEEE float slope factor. Raster EF changes from
`0000f055` to `0400f055`: bit 26 enables the offset. Disabled offset leaves
that bit clear, independently of cached D0/D1 contents.

Across these six vendor cases the complete VS/FS program image, 44-float
preprocessing constant span and eight-word linked record hash identically.
The offset draw moves its preprocessing span from system +10 to +510; it
is not a vertex shader patch or a replacement matrix. Hashes and packets
are in `captures/m9-depth-bias-20261006/emitter-and-linux.json`.

## Genuine MSerX consumer

Actual instructions `0xf7280..0xf72f5` test backend cache dirty bit 0x10,
emit header `4545d1d0`, copy cache +98 and +9c to its two values and clear
that dirty bit. `tools/m9emu/depth-bias.py` executes this bounded consumer
for every captured pair plus a clean-cache no-emission control. Only the
cache input and a preallocated stream are controlled fixtures; allocation,
context bootstrap, runtime selectors and GPU execution are not claimed.
The output pins the actual instruction-span hash. No callback replaces
the emitter or reports synthetic allocation success.

The saved vendor readbacks corroborate the arithmetic. In the source
`tools/m9x-raster-capture.c`, the slope is 0.5/96 and the sampled center has
unbiased depth 0.4192708333. The vendor reports:

| factor/units | read depth |
|---|---:|
| 0/0 | 0.41927078 |
| 0/2 | 0.41927090 |
| 0/8 | 0.41927126 |
| 0/-4 | 0.41927055 |
| 1/0 | 0.42447916 |
| -2/1 | 0.40885416 |

These are the expected `factor * max(abs(dz/dx), abs(dz/dy)) + units/r`
changes at D24 quantization, with `r=2^24-1`. The flat no-offset comparison
triangle remains 0.49999997. Exact saved result lines are retained beside
this proof; this is vendor hardware evidence, not our new native test.

## Implementation scope and validation

The candidate retains native GPU preprocessing and enables raster bias for
filled triangles with enabled depth testing, a valid D24 attachment, finite
factor/units, scaled
units and clamp zero. Other offset modes, split raster shaders and explicit
fragment-depth writers retain the existing CPU path. Bias words are part of
the job state key and command budget. CPU-transformed draws never receive
this hardware bias a second time. Preprocessing EF remains unchanged.

With depth testing disabled, ordinary colour-only draws remain native and
emit no bias pair or EF26, even with a D24 attachment and offset enabled.
If the fragment program reads fragment position, the existing CPU offset
stage remains responsible for its pre-shading z; offset is not blindly
ignored for that program. The single-pass schedule itself preserves the
key EF26 (its depth selection is schedule-based, not `depth_enabled`), so
there was no demonstrated single-pass bit-loss. This tightening limits
hardware activation to the state proved by vendor captures.

The real attachment admission is D24: `m9_format.c` only advertises depth
storage for `S8_UINT_Z24_UNORM` and `X8Z24_UNORM`, code 7d/cpp4, and
`m9_screen.c` attaches HZ only for that code. Z16 is sampler-only and cannot
satisfy the required attachment/HZ predicate.

Kernel admission accepts only the finite two-scalar D0/D1 packet in that
order, with no relocation expectation; D0 and D1 stay outside the general
index whitelist. Malformed/NaN/infinite packets remain refused, including
when the FAA diagnostic index list names them.

Offline checks completed:

* Full existing Mesa batch suite and context compile PASS. Independent
  captured pairs match bit for bit; biased jobs add three raster words,
  no relocations and no preprocessing changes. Disable returns to the
  baseline stream; inconsistent bias state is refused atomically.
* Actual extracted kernel parser: 82 finite pairs accepted, 17 malformed
  packets refused, no scalar relocations PASS. This does not replace a full
  module build.
* New `m9-mesa-glsl polyoffsetd`: llvmpipe 26.2.4 **0/76800 wrong pixels**.
  Sixteen cells cover coplanar and sloped geometry, both signs, units and
  slope magnitudes, zero, disable after nonzero state and Q3's exact -1/-2.
  `polyoffsetnotestd` keeps the same D24 attachment but disables depth testing;
  all 32 draws must render, with the final green overlays everywhere. Its
  llvmpipe reference also passes 0/76800. The no-depth-attachment control
  `polyoffset` passes 0/76800 as well.

**M9 native validation is pending** a controlled reboot with the typed
kernel update and this Mesa candidate. No 100% native Q3 claim or FPS gain
is made from these offline proofs. The last deployed Q3 baseline remains
56.6 FPS and 99.2% native transforms; the precise remaining fallback has
`offset_tri=1, factor=-1, units=-2, clamp=0, unscaled=0`.

Reproduce the genuine instruction/capture proof:

```
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/depth-bias.py
python3 tools/m9-kernel-depth-bias-check.py /path/to/kernel/m9x
```

Native bounded workflow: compile this fixture, run `polyoffsetd` with FAA
off and `M9X_DEBUG=1 M9X_NATIVE_VS=1`, then `polyoffsetnotestd` and one
ordinary texture control.
The depth fixture must pass all pixels and show no native fallback or
skipped draw. A short Q3 gameplay profile after that can establish whether
the exact remaining offset draw has become native; no repeated timedemo
is needed for this coverage proof.

## 2026-10-08: GL units take two D24 LSBs (deliberate departure from the vendor)

The vendor GL ICD applies **one LSB per unit**: D0 for units 2 is
`2/(2^24-1)`, and its own readbacks above step by 2, 8 and -4 LSBs for
units 2, 8 and -4. The D3D handlers carry raw depth-unit floats and have no
GL unit scale to compare against.

On the M9128 that scale does not satisfy GL's resolvable difference.
MEASURED with `tests/m9-piglit-full-20261008/zprec.c` (2000 full-window
quads under piglit's near=1/far=infinity projection, D24 readback): stored
depth lies within ±1 LSB of the exactly rounded window depth, and the
distribution is identical for the native vertex program and for the draw
module (`M9X_NATIVE_VS=0`). With one-LSB units, piglit
`gl-1.4-polygon-offset` measures an ideal MRD of 2 bits against an actual
1 bit and fails, 4/4 natively. The draw module's offset stage already uses
`units * mrd * 2` for CPU-transformed draws (the same D24 convention as
radeonsi), and that path passed 4/4.

Mesa m9x `9e52901` therefore doubles the GL units before the unchanged
captured encoder (`m9_rast_depth_bias`), so native and CPU-transformed
geometry in one frame get the same offset. MEASURED after the change:
units=1 gives a 2-LSB step in 2000/2000 native samples (was 1 LSB in
2000/2000). Slope factors are unchanged. Quake3's `-1/-2` now matches what
its CPU-path draws already received.
