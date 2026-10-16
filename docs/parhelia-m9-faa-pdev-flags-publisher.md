# Genuine PDEV flags publisher: CPU correction, unchanged GPU contract

The retained M9128 PCI identity is `102b:0540`, revision `1`, subsystem
`102b:22c0`. Actual MM `14ea1` consumes the returned PCI tuple
`40052b102b10c02201000000`; `14f19`/`14f1f` publish value `2` to the retained
miniport owner `+44` and returned MM profile `+1f0`. The source capture is a
distinct re-execution of that genuine PCI producer on retained bootstrap
owners. It supplies no GPU input and does not claim full PDEV admission.

The former controlled MD fixture omitted the shipping prefix `2e011..2e02c`.
The new isolated control executes its six pinned instructions, with the actual
returned MM profile. Actual stores `2e01d` and `2e026` copy `2` to device `+1c`
and hardware shadow `+1051c` before configuration, resource modules or queue
publication. The host does not assign either field. The timeline prefix
starting at `2e02c` is left to the existing fixture.

Static MD `1a9ae` tests hardware `+1051c & 80000003` and selects constructor
field `16` instead of `4` at `1a9df`. The passive `1a9e7` observer records **no
execution** of this branch in the complete bounded FAA public-red flow. This
is a static consequence, not an observed constructor change. The actual
same-run `b5f86`/`b6028` clear producer reads device value `2` but retains bit0
as `0`, so the optional padding branch remains disabled.

The independently captured whole public-red control retains 35 live MDLs,
90 typed operands, 50 nonnull address sites and 25 timeline sites. Its genuine
independent flush posts 5596 bytes and stops before the real GPU status read;
the requested source deadline remains `140`. All retained owner extents,
domains, known input spans, shader/RS bytes, primary/secondary/constants/
geometry/footer bytes, relocation encodings and timelines equal the previous
public-red control exactly. The generated blob and kernel header are byte
identical. Only source provenance differs in the plan.

This closes an omitted upstream CPU publisher. It produces no new GPU
candidate and provides no reason to repeat hardware qualification. Existing
FAA black-output results remain unchanged; the ordinary SYSTEM red success
is a separate control.

All artifacts are under `captures/m9128-device-inputs-20261006`, prefixed
`normal-faa-output-pdev-flags-`. The strict default manifest checker still
requires the original zero-field fixture; only the dedicated producer checker
admits flags2 after proving the actual PCI, MM and MD source joins. It requires
complete GPU-contract equality, rather than relaxing native bounds.

Reproduce validation and generation from the RE repository:

```sh
FAA_CAP=captures/m9128-device-inputs-20261006
FAA_PY=/home/ai/tmp/faaemu/venv/bin/python
$FAA_PY tools/m9-native-faa-pdev-flags-template.py \
  --manifest "$FAA_CAP/normal-faa-output-pdev-flags-input-manifest-101f8.json" \
  --source-capture "$FAA_CAP/normal-faa-output-pdev-flags-source-101f8.json" \
  --contract "$FAA_CAP/normal-faa-output-pdev-flags-contract-101f8.json" \
  --address-audit "$FAA_CAP/normal-faa-output-pdev-flags-addresses-101f8.json" \
  --flags-control "$FAA_CAP/normal-faa-output-pdev-flags-publisher-101f8.json" \
  --pci-flags-proof "$FAA_CAP/normal-faa-output-pdev-flags-pci-producer.json" \
  --baseline-manifest "$FAA_CAP/normal-faa-output-constant-red-input-manifest-101f8.json" \
  --baseline-contract "$FAA_CAP/normal-faa-output-constant-red-contract-101f8.json" \
  --baseline-plan "$FAA_CAP/normal-faa-output-constant-red-native-plan.json" \
  --baseline-blob "$FAA_CAP/normal-faa-output-constant-red-known-inputs.bin" \
  --baseline-header "$FAA_CAP/normal-faa-output-constant-red-kernel-data.h" \
  --plan "$FAA_CAP/normal-faa-output-pdev-flags-native-plan.json" \
  --blob "$FAA_CAP/normal-faa-output-pdev-flags-known-inputs.bin" \
  --kernel-header "$FAA_CAP/normal-faa-output-pdev-flags-kernel-data.h"
```

The native CPU fixture additionally exercises the actual kernel encoder and
vendor-instruction page-clear reference at two placements and all eight page
alignments:

```sh
python3 tools/check-m9-faa-output-native-cpu.py \
  --plan "$FAA_CAP/normal-faa-output-pdev-flags-native-plan.json" \
  --blob "$FAA_CAP/normal-faa-output-pdev-flags-known-inputs.bin" \
  --kernel-header "$FAA_CAP/normal-faa-output-pdev-flags-kernel-data.h" \
  --vendor-clear-proof "$FAA_CAP/normal-faa-clear-page-runs.json" \
  --kernel-source /home/ai/src/parhelia/src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/m9x/m9_faa_native.c
```

The source controls themselves are `tools/m9-faa-device-flags-source.py` and
`tools/m9emu/m9_faa_pdev_flags_control.py`. The former takes `--out`. The
latter takes `--flags-control-out` and the existing read-contract CLI after
`--contract-driver tools/m9-vendor-faa-read-contract.py`, preserving actual
`--mm-flags 0x101f8`, `--public-constant-red` and independent-flush admission.
Use `--runtime-tools tools/faaemu`; the bridge/scatter tools are `tools/m9emu`.
Their hashes and both shipping image hashes are pinned by the dedicated
checker. The new prefix remains opt-in; ordinary/default captures are intact.
