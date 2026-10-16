# Texgen scene appearance reference, 2026-10-07

The corrected ordinary native screenshot has a complete silhouette and no
periodic sawtooth bands. Its dark, stretched grain is consistent with the
independent software reference here. This is a qualitative scene comparison,
not an equal-angle hardware pixel oracle or an all-GL2 qualification.

The parent-owned physical run used canonical Mesa `3b02e61`, DSO prefix
`53195275`, default RECORD1/VARYINGS2, queue1, FAA0/vblank0 and a 1024x768
window. It completed 2108 GPU user draws with no CPU user draw. Real point
`130180` retired; the later observation remains healthy at raw `130181`.
The printed 703 FPS belongs to that observed run; this reference does not
establish a controlled performance gain.

The installed vertex/fragment sources and captured inputs are joined in
`captures/m9x-texgen-software-reference-20261007`. The full job-2 input remains
in the existing `m9x-texgen-fused-inputs-20261007` capture. The replay helper
requires its exact 144-slot C2 hash and complete 36-vertex/48-constant-pair
shape before reconstructing matrices. It renders only through a checked
llvmpipe EGL context; it does not create native commands or modify the driver.

The record's first vec4 is **normal**, its second is **position**. The actual
C2 loads the first through the NormalMatrix operations and the second through
the projection operations. The captured cube positions are close to +/-1;
the normals include averaged corner/edge directions. Record ordinals alone
are insufficient semantic labels. All actual input and matrix words are
retained in the small derived `inputs.bin` with hashes in `proof.json`.

[SceneTexture 2023.01](https://github.com/glmark2/glmark2/blob/2023.01/src/scene-texture.cpp)
selects cube/crate-base, a directional light and generated asin coordinates.
Cube corner UVs cover approximately 0.3041..0.6959. Side faces can collapse
one coordinate, stretching the grain. There is no ambient term.
[The upstream texture loader](https://github.com/glmark2/glmark2/blob/master/src/texture.cpp)
reverses decoded PNG rows before upload and uses CLAMP_TO_EDGE; the reference
matches those choices. These mechanisms explain the observed appearance.

`actual-input-pose.png` uses the captured unrotated matrices.
`scene-rotation-43.png` adds source-defined X/Y/Z rotations of 43.2 degrees,
corresponding to 36 degrees/second at a nominal 1.2 seconds. This is an
illustrative pose, **not** a measured match to the hardware screenshot time.
The latter is retained as `native-texture-texgen.png`; `before-mask.png`
retains the prior failure. No image is reordered or substituted into a native
readback. The reference implements the same shader equations in software
GLSL rather than interpreting the native ISA.

Reproduce with Pillow, EGL/GLES2 headers and the matched T2 userspace:

```sh
python tools/m9-texgen-software-reference.py \
  --t2 /srv/t2/build/x86-64-desktop-26.9.15-desktop-x86-64-nocona-linux \
  --asset /usr/share/glmark2/textures/crate-base.png \
  --out /tmp/m9-texgen-reference
```

The tool fails on a changed C2 image or incomplete input spans. The retained
asset SHA identifies the installed PNG without duplicating it. The old
captured pass-through C2 independently has no non-output scratch destination
in the newly excluded r10/r12 pairs; its existing bf00 mask already reserves
all its scratch pairs. The CPU-fallback sawtooth issue remains separate and
open. No corresponding runtime change is proposed from this audit.
