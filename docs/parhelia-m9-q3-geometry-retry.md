# Paired-source retry of the actual Q3 slot refusal

The bounded actual Q3 diagnostic in
`tests/m9-constant-broadcast-hardware-20261007/q3/proof/native-fallback-1.txt`
reconstructs to 226 slots and a 74-pair constant span with the genuine
specialization DWORD zero = float four. The source SHA is
`328342e4db4504179c6ab96c8c3cba155d90540bdcfd0ad3f259f54b0af6dfd6`.
The source, original inlinable offsets/words and UBO prefix are retained; the
reconstructor rejects unfamiliar instructions/flags/control flow.

Its register-dot graph triggers geometry preparation, splitting four paired
sine chains into eight scalar chains. A bounded trial preserves the supported
paired graph and packs eligible scalar uniform keys using their existing UBO
byte offsets. Mesa implementation `7d48f21` changes this shader to 176 slots
and a 61-pair span, within the existing 188/63 caps. It first compiles normally;
only a still-refused result may adopt a fitting trial. Already admitted
program bytes remain unchanged. This is a source-specific count, not proof
that every slot refusal in the timedemo uses this graph.

The saved pre-geometry clone belongs to the original ralloc parent. Adoption
exchanges the NIR graph and program together, enabling later sampler variants
to start with that same paired source. Failed compilation, exceeded limits
and clone allocation failure retain the original source owner and program.
Programs retain only independent arrays/scalars; no queued BO/fence ownership
changes. Identity-only constants remain exact, including MAD addends, DOT,
MINMAX and CMP. Literal bits, coordinate keys and the uniform-zero sentinel
are excluded from high-lane reuse.

`tools/check-m9-vendor-constant-lanes.py` runs the hash-pinned actual MxMD
vertex constructor/frontend/optimizer/native-instruction packer, recording
all 0x44-byte native objects and packed instructions. Explicit x/y/identity
normalized reader inputs prove high-lane broadcast three for MOVK, MUL_RK,
MAD_RKR multiplier and MAD_RKK multiplier. In both packed slots the x/y
controls differ only by the corresponding two swizzle bits. No output linking
is supplied, so optimizer return zero is expected. This witnesses CPU compiler
encoding, not a linked public shader, GPU execution or a manufactured reply.

`tools/check-m9-geometry-retry.py` invokes the actual native helper and the
independent NIR/decoded-word evaluators. Its 2,048 deterministic attribute/UBO
trials produce bit-identical old/new outputs. Streams are joined by their
varying semantic location because preparation can reorder their stores. Both
retain the same sine approximation error (maximum normalized error
0.00466012955 with broad random coefficients); no improved numerical accuracy
is claimed. Additional controls cover unchanged admitted images, both limits,
clone OOM, original parent ownership, a subsequent sampler-coordinate variant,
the prior constant-only retry and the genuine five-attribute refusal. Printed
source instruction/flag/control-flow roundtrips match all three actual dumps.
Assertions remain active after the production NDEBUG headers; this does not
claim that the unavailable debug NIR validator executed.

```sh
python3 tools/check-m9-geometry-retry.py \
  --captures tests/m9-constant-broadcast-hardware-20261007/q3/proof \
  --driver-source /absolute/path/to/m9x-driver \
  --mesa-build /absolute/path/to/mesa/objs-m9x-llvm \
  --loader /absolute/path/to/matching/ld-linux-x86-64.so.2 \
  --out /tmp/m9-q3-geometry-retry

python3 tools/check-m9-vendor-constant-lanes.py \
  --dll /absolute/path/to/pinned/MxMD.dll \
  --emulator-dir /absolute/path/to/tools/faaemu \
  --out /tmp/m9-vendor-constant-lanes.json
```

The compact evidence is in `tests/m9-q3-geometry-retry-20261007/`. The complete
DSO build, all 94 existing decoded reference cases/five refusals, complete
batch test and bounded diagnostic lifetime fixture pass. These host proofs
establish no new physical shader acceptance, rendered pixels or FPS result.
A shader-qualified pixel/record fixture remains necessary for case one.
