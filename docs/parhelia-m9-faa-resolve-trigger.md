# Normal M9 FAA resolve lowerer

The genuine surface-owned normal frame dispatches `abc20` through actual
compiler mode9. Its resolve span contains 177 raw indexed DWORDs, unlike the
two counted triangle secondaries. It ends in `[45fcf3ec,0,0,00f00140]` for the
320x240 origin-zero output rectangle.

`abc20` packs the origin into fragment shadow+30: low16 x, high16 y, written at
`abc70`. It packs `(right-x)` and `(bottom-y)` in the same order into shadow+34,
written at `abc7e`. The genuine `f8e36` compiler calls `fadf0` first, then writes
`45fcf3ec` at `f8e94`, zero at `f8ea1`, shadow+30 at `f8eb9`, and shadow+34 at
`f8ed1`. The trigger is emitted only after preceding restore/save and complete
state lowering. It is not a substitute for that state or ownership sequence.

`tools/check-m9-faa-resolve-trigger.py` first verifies the strong normal graph
and raster-state audits. It then verifies compiler mode9, actual caller
rectangle, exact shadow values, actual four contiguous CPU writes/producer
PCs/live DMA owner, raw indexed grammar, final trigger position, and retained
primary/companion/HZ/main/auxiliary identities and descriptors. The resolve's
actual F0/EF/D7 words are e0000400/20007000/0; AD remains the main extent>>5.
The passive capture pins all seven raster and four trigger writer instruction
sequences against the shipping binary.

Artifact: `docs/traces/m9-vendor-faa-normal-frame-20261006.json`.
Receipt: `docs/traces/m9-vendor-faa-resolve-trigger-20261006.json`.

The CPU path completes real queue dispatch but supplies no GPU retirement or
image. This exact minimal rectangle/state proof does not admit arbitrary
indirect pool/sentinel resources, infer untouched bytes, or define generic
shader variants. The private kernel producer must preserve the actual opaque
saved-state lifecycle before enabling execution.
