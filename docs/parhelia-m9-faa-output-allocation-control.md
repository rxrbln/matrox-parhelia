# Whole lowered FAA output allocation control

The genuine public constant-red output flow was captured again after one
additional ordinary 1024×768 resource request. The returned allocation stays
live; no FAA graph owner, shader, mapping result or GPU reply is substituted.
This finite control found no newly omitted active allocation-dependent field.
It does not prove arbitrary allocations or GPU rendering.

`tools/m9emu/m9_faa_output_allocation_control.py` invokes real `a3870` after
the actual frontend constructors and before the normal sampled primary. Its
caller request selects profile 12, request 2, width 1024, height 768 and the
ordinary one-layer fields. The returned resource has no source-known content.
The existing output exporter then runs unchanged, including genuine public
CREATEPS54/SETCONST57/SETPS56 constant-red input, the internal ordinary output
request and the independent real queue finish stopped before GPU status.
The existing conditional PDEV/display admission and internal output-helper
boundary remain unchanged.

Both complete source checks pass. Both flows contain 90 typed operands,
50 nonnull native encoded sites, 25 actual timeline stores and 124920 known
input bytes. The extra allocation adds one resource/owner; it does not become
a draw or resolve operand. Every changed active lowered byte belongs to a
proved typed encoder, direct endpoint, timeline or allocation-page clear.
Source bindings, domains, dimensions, shader input, raster shadows, trigger,
public triangle commands and real final deadline are unchanged.

Of 96 differing known bytes, two lie outside those encoded fields. They are
the already classified inactive AC halfword at secondary offset `0x3dc` in
the one-render-target case. The real `fbee8` store copies AA shadow `+0x24`;
the active AB word at `0x3d8` remains `0xffff`. The exact lowerer bytes and
one-RT producer hash are pinned by the checker. This proves observed copied
destination bytes and their inactive role, not universal initialization of
the upstream stack residue. The native flow retains its own bounded inactive
source bytes; this evidence proposes no patch based on that residue.

Artifacts are `captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-`
`shifted-{input-manifest,source,contract,addresses}-101f8.json`, with the
`allocation-control-101f8.json` caller/source hash sidecar and
`allocation-comparison.json` report. They remain separate from the original
red capture and native plan.

Run the strict audit from this repository with the Unicorn/pefile Python:

The established local interpreter is `/home/ai/tmp/faaemu/venv/bin/python`.
Root independently ran the audit with that interpreter and reproduced the
two complete source checks and whole changed-byte classification below.

```sh
python tools/m9emu/check-faa-output-allocation-control.py \
  --baseline-manifest captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-input-manifest-101f8.json \
  --baseline-source captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-source-101f8.json \
  --baseline-contract captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-contract-101f8.json \
  --baseline-addresses captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-addresses-101f8.json \
  --shifted-manifest captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-shifted-input-manifest-101f8.json \
  --shifted-source captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-shifted-source-101f8.json \
  --shifted-contract captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-shifted-contract-101f8.json \
  --shifted-addresses captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-shifted-addresses-101f8.json \
  --allocation-control captures/m9128-device-inputs-20261006/normal-faa-output-constant-red-allocation-control-101f8.json \
  --out /tmp/m9-faa-allocation-comparison.json
```

There is no GPU execution in this control and no image claim. The real
constant-red hardware result still has a black ordinary destination despite
color-dependent main stores. A genuine ordinary draw into the same SYSTEM
destination is a separate useful admission/write-domain discriminator.
