<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Complex GL2 bump: source-derived pair masks and real clip qualification

The optional native five-stream path now carries the actual glmark2 height-bump
clip coordinates correctly on M9128. One 1024x768 diagnostic run on canonical
Mesa `05c7c29` / DSO `8f39f59c`, normal kernel `73c2a45` and boot `6d6d22e6` passes all
1056 clip components across 264 vertices against independent submitted-input
matrix math, with all 1440 logical indices valid. 156 GPU user draws complete
with zero user CPU fallback, and the actual final `f400`/raw `f401` stays retired,
queue-empty and unwedged in the observed two-second follow-up. The actual
window PNG shows a textured, lit object.

The source correction belongs to the packed FIRST/CONT metadata calculation.
Actual shipping GLSL/frontend/optimizer/finalizer execution on the complex
bump source remaps outputs to odd `11..1f` while retaining scratch `00..0f`. The
vendor `150dc0` calculator couples both output-pair availability bits when either
pair is occupied. A labelled synthetic usage control adding scratch `10` gives
fc00, rejecting the prior driver's prefix `fe00`. The complete linked compiler
objects, native words and shifted-heap control are retained separately in
`captures/m9x-complex-glsl-finalizer-20261007`. The corresponding Mesa helper
now applies this source-derived closure only to complete bound packed targets;
all shader instruction bodies, dependencies, slot counts and earlier features
remain. Actual bump masks are `fc00`/`ff80`.

The compact physical archive is
`captures/m9x-glmark-bump-coupled-mask-20261007`. It keeps only matching context 0
job 313, complete source/index/constants/C2/run metadata, the 30752-byte retired
record, strict unmodified oracle, PNG and necessary loader/transport/kernel
logs. Both real producer and wait point are 62272. `check-archive.py` verifies
raw hashes, repeats all clip/index references and rejects wrong-W and wrong-job
negative copies without GPU access.

This is an actual clip/index transport qualification and visible output,
not a full independent application pixel comparison. The PNG at 1.2 seconds
is not synchronized to final job 313. The printed 37 FPS includes complete
input/program dumping and record readback, so it does not establish a
controlled performance gain. The previous failing `5f78a2d` hardware binary
also lacked component slicing `bd792ac` and selected-BE-owner wait `21a446f`;
source evidence supports the mask mechanism strongly, but the hardware run
is not a one-change binary A/B experiment.

The ordinary default remains capped at two varyings. This one application
case does not qualify all GL2 operations or larger source/stream counts.
The complete EE19 consumer/+58c source-policy join and remaining native
operation gaps are still separate work. No target action was performed by
the archive or source-analysis agent; root alone executed this bounded run.

## Single capture-free measurement

Root then ran one three-second height-bump measurement on the same
Mesa `05c7c29` / DSO `8f39f59c`, normal `73c2a45` /boot `6d6d22e6`, at 1024x768 windowed
with one active display, FAA=0, vblank=0 and the same optional queued packed
pipeline. Actual result: 417 FPS / 2.400 ms, 1251 GPU user draws and 0 CPU user
draws; final `84ac0`/raw `84ac1` is retired with an empty, unwedged queue stable
after two seconds. No input/program dumping, record readback, profiling or
window capture ran during this measurement. Loader artifact hashes were
checked. The compact raw proof is
`captures/m9x-glmark-bump-clean-bench-20261007`.

The approximately 11.3× difference from the 37 FPS diagnostic is removal of
capture/readback overhead, not an 11× code improvement. The older 85 FPS corrupt
geometry run is an invalid baseline. This is one present implementation's
bounded scene result, not a controlled speedup experiment or a replacement
for the separately archived clip/image qualification.
