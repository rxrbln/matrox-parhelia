# Independent private output producer review

Reviewed source checkpoint `92747896` in `wip/m9-faa-output-finish` and kernel
checkpoint `3a5ec1d` in `wip/m9-faa-secondary-kernel`, using actual signed
unfinished-output/independent-flush evidence `7498c93b`. This review used only
local files and CPU emulation/fixtures. It found no blocker in the reviewed
bounded private producer; it does not establish hardware retirement, rendered
pixels, public Lock admission or return from the interrupted original copy.

Exact reviewed file SHA256 values:

| File | SHA256 |
| --- | --- |
| `tools/m9-native-faa-output-template.py` | `ac17ccb127ab094403162fd3f35afd0450b148ad0b637ac01fce6dc2823ae583` |
| `m9x/m9_faa_native.c` | `2809079052f222f9605dac3e9821e94bbbb8d8837e14b6a3a8d62278c9750f85` |
| `m9x/m9_faa_native_data.h` | `9d16529686a242a043a33b60b32772708278dd88d26ba33b20a7fe063fad262c` |

Independent regeneration from the new source manifest, source capture,
contract and direct-address audit reproduced the staged plan, 124928-byte
blob and kernel header byte for byte: 31 backing owners, 51 address fields,
25 timeline fields and source primary length 5596. Source-known resource
spans and actual MDL-written spans are the only exported bytes. The ordinary
output has no exported pixel span; resource aliases share a backing owner;
each SYSTEM view remains bounded by its actual mapped prefix. The actual
counter proxy is kept separate from raw NT MDL ownership.

All observed nonnull typed operands and direct secondary endpoints join live
owners with their actual expressions. The direct interval end marker 2 keeps
the source's GPUvirtual owner. Queue, constants, geometry and ordinary output
use private flags-30/PTE-b mappings. The new counter node aliases the
transport's existing coherent completion page through ordinary PTE-a mapping,
and `106c` selects its live GPUvirtual address with tag 1. `1068/1070` retain
their separately derived physical common-buffer words at offsets 20/40 with
tag 2. No new completion allocation or independently seeded success value is
introduced. Owners and the completion alias survive submission and timeout;
teardown follows bus-master stop.

The primary transport clone is admitted only after rejecting primary
self-references. The native transport owns its physical primary and posted
length, while indirect secondary and geometry owners retain their independent
GPUvirtual bindings. The already armed transport does not overwrite the new
counter selection; command-slot switches update only the primary base.

The clear helper patches ordinary pointers and timeline fields before moving
the already patched suffix. It regenerates page masks/counts from the live
companion allocation, updates its count header and final submission length,
and preserves every other known suffix byte. The source's same-run padding
flag zero and logical 151-page extent remain explicit bounded guards. The
header derives the actual final timeline requirement 140, and log counts use
array lengths instead of the old 53/26 counts.

The independently executed kernel CPU fixture passed two different native
placements at all eight page alignments. It compares the clear against actual
vendor instructions, preserves the patched suffix, secondary commands,
geometry and unknown output contents, and checks the actual 140 footer plus
refusal before modifying corrupted or undersized inputs. The test command,
with paths expanded to these isolated worktrees, was:

```
python3 m9x/tests/faa-output-template-cpu.py \
  --plan RE/captures/m9128-device-inputs-20261006/normal-faa-output-native-plan.json \
  --blob RE/captures/m9128-device-inputs-20261006/normal-faa-output-known-inputs.bin \
  --vendor-clear-proof RE/captures/m9128-device-inputs-20261006/normal-faa-clear-page-runs.json
```

`RE` denotes the reviewed `wip/m9-faa-output-finish` directory. The source
generator's own CLI independently ran `check-faa-output-manifest.py` before
producing the comparison artifacts. This validation closes this fixed
diagnostic's producer/bounds/namespace/variable-length glue; arbitrary shader,
texture, depth, dimensions and public copy contexts remain outside its scope.

## Subsequent physical qualification and scope correction

The integrated runtime `df1f49e` retires at `0x1d0` on real M9128 but the
ordinary output is entirely black; see [the failed hardware image](../tests/m9-faa-ordinary-output-20261006/README.md).
The self-reference check establishes explicit pointer safety of the primary
clone. It does not establish behavioral equivalence of physical-tag2 primary
fetch to the vendor's retained virtual-tag1 primary, including implicit
frontend/cache contracts. That source/native difference remains under
investigation. Structural fixture PASS must not be interpreted as rendering
PASS or full submission-domain equivalence.
