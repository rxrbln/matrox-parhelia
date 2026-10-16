# Bounded retry for the actual Q3 constant-span refusal

The Q3 dumps in `tests/m9-constant-broadcast-hardware-20261007/q3/proof/`
retain unspecialized source, actual inlinable DWORD offsets and specialization
words. `tools/m9-fallback-recompile.py` reconstructs their bounded NIR grammar
and applies the same specialization and late algebraic/DCE passes. Unsupported
instructions, flags or control flow stop reconstruction; it does not guess.
All three printed instruction/flag/control-flow roundtrips and ordinary counts
match: 137 slots/69 constant pairs, 226/74, and the five-attribute refusal.
The declared constant span includes the existing thirteen tail pairs.

Mesa driver commit `4a1ce01` first compiles normally. Only a constant-span
refusal whose existing stage lengths fit can try `nir_opt_vectorize` on a
clone. It adopts the clone's emitted program only when both configured limits
fit. Existing caps, instruction encoding, source specialization, GPU
transforms and queued ownership remain unchanged. Slot-limited and failed
trials retain the original program; clone failure follows the same rule.

For actual dump zero, the trial changes 137/69 to 161/61, within 188/63.
`tools/check-m9-constant-retry.py` executes the actual wrapper and independent
NIR/decoded-instruction evaluators. Across 2,048 deterministic input/UBO
trials, old and new outputs are bit-identical; both have the same existing sine
approximation error (maximum normalized reference error 0.000180726).
Controls cover unchanged source NIR, clone allocation failure, slot and
constant limits, unchanged admitted image, and all three actual refusals.
Production NIR libraries are compiled with `NDEBUG`; generated harness checks
use an explicitly active assertion macro after the production headers. They
do not claim the unavailable debug NIR validator ran.

```sh
python3 tools/check-m9-constant-retry.py \
  --captures tests/m9-constant-broadcast-hardware-20261007/q3/proof \
  --driver-source /absolute/path/to/m9x-driver \
  --mesa-build /absolute/path/to/mesa/objs-m9x-llvm \
  --loader /absolute/path/to/matching/ld-linux-x86-64.so.2 \
  --out /tmp/m9-q3-constant-retry
```

The existing 94-reference/five-refusal suite, full DSO build, batch checks and
bounded diagnostic lifetime fixture pass. These are host proofs, not a new
hardware shader acceptance or FPS claim. A separate whole-public-Q3 pixel
fixture must qualify the newly admitted shader before benchmarking.
