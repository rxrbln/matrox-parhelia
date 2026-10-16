<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Whole ordinary FAA output with a public constant-red shader

This source control distinguishes constant red from the previously tested
constant green. It does not establish a fragment-record ABI, identify the
black-image cause or prove FAA pixels. The initialization-only hardware phase
leaves main/AA/pool zero. The returned-public-triangle phase introduces the
same entire main/AA byte images seen after the older full sampled resolve.
Those effects occur before output; their dependence on actual shader color
remains a device question.

The fresh capture uses real public CREATEPIXELSHADER54, constant57 and binding56
requests followed by the unchanged public triangle. The standard shader is
`ps_2_0 mov oC0,c0`; the new caller-owned constant is `(1,0,0,1)`. The old green
helper is unchanged. A separate `m9_faa_public_constant_red.py` supplies only
caller DP2 storage and length through the same real `beeb0` entry. Vendor
instructions, internal enums, compiled programs and captured green constants
are not patched. The public call returns1/HRESULT0.

The retained sampled primary/context and geometry are unchanged. Output is
still the bounded internal `ac1f0` ordinary-destination experiment below
unclosed public `a74b0` admission. Its original CPU copy stops before the first
actual `4e2fc` completion-page read. A distinct real `23b80` finishing call
appends twelve bytes and posts the real primary, stopping before `23bc3`
reads GPU status. Neither flow is given a GPU counter reply, returned copy
or pixel data. The live ordinary output's entire `50000`-byte extent remains
source-unknown.

The complete source artifacts are
`normal-faa-output-constant-red-{input-manifest,source,contract,addresses}-101f8.json`.
The capture preserves35 live MDL owners,34 raw MDL-prefix joins, one typed
counter proxy,90 actual typed operands,50 nonnull address fields,25 timeline
fields and same-run allocation metadata, direct endpoints and clear flag.
Primary lengths are5584/5596, secondary intervals604/748, constants136 and
geometry188 at offset`100`. The actual desired counter is`140`. Counts and
lengths are decimal; offsets/counter are hex. The null kind`1e` operand is
source-validated before omission from the relocation list.

The red and green source programs are the same240 known bytes, SHA256
`46e77ecc6215fd403a354d0c8a9ace2c16868e434d87f6fb02efae2a6614fe55`.
All VS, RS, metadata, texture and sentinel input bytes remain identical.
The entire primary, secondary and geometry source spans also match. The
104 preprocessing constants match, and the32 raster constants differ only
in the first16-byte public `c0` vector. Thus the new control preserves the
constant program and tests actual red versus actual green, rather than
interpolation versus constants.

The native generator derives a complete new owner/span/fixup/timeline plan
from this fresh capture. It never patches the old green blob. Generated
artifacts are:

```text
normal-faa-output-constant-red-native-plan.json
normal-faa-output-constant-red-known-inputs.bin
normal-faa-output-constant-red-kernel-data.h
normal-faa-output-constant-color-comparison.json
```

The header uses the common `M9_FAA_*` definitions and contains31 retained
owners,50 sites,25 timelines and124920 source-known input bytes. Every plan
field other than source provenance and blob hash is identical to green.
The whole blobs differ at exactly four bytes, all in owner29's actual raster
constant at offsets2,3,6,7 (blob offsets`1e16e`, `1e16f`, `1e172`, `1e173`).
The comparison checker verifies all other bytes and the complete owner,
encoder, mapping, clear and timeline contracts.

Manifest SHA256 is
`38f86877684f6f990cc216680795f6c90bf40c515afa5d7ac4d409a629bdb22b`.
Red blob SHA256 is
`2cccc5f1c5a73802f94de63ebd0f89032b068427e268f2a46f82815b300e9422`;
header SHA256 is
`06168531488ddd85e5b7d23c235f3749d8767057630ef765a81634bdb8004061`.
The plan pins all four source artifacts and both reference manifests.
A separate exclusive private build selector must choose the entire header
and distinct `m9x/faa-constant-red-known-inputs.bin` firmware. Existing full,
green and phase profiles remain separate. This checkpoint changes no kernel
profile or runtime gate.

Validation passes the red-specific public-token/constant/whole-source checker,
all existing source/domain/MDL/metadata/address/clear/footer guards, and the
whole red/green plan comparison. The new RE CPU fixture extracts the actual
canonical kernel encoder and page-clear helpers. It passes two placements
and all eight vendor-instruction clear alignments with suffix, all other
owners, geometry and unknown-output preservation, correct finishing token,
and rejection before mutation. Both original baseline and green checkers
pass; regenerated baseline/green plans, blobs and headers are byte-identical.
The baseline CLI and green checker reject the red control.

Reproduce with the existing full exporter arguments and
`--public-constant-red`, actual registry control`101f8`, independent finishing
helper and address observer. The source checker and generator require the
retained baseline and green manifests:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-output-constant-red.py \
  MANIFEST --source-capture SOURCE --contract CONTRACT --address-audit ADDRESSES \
  --baseline-manifest BASELINE --green-manifest GREEN

/home/ai/tmp/faaemu/venv/bin/python tools/m9-native-faa-output-template.py \
  --public-constant-red-control --baseline-manifest BASELINE --green-manifest GREEN \
  --manifest MANIFEST --source-capture SOURCE --contract CONTRACT \
  --addresses ADDRESSES --plan PLAN --blob BLOB --kernel-header HEADER

python3 tools/check-m9-faa-output-native-cpu.py --plan PLAN --blob BLOB \
  --kernel-header HEADER --kernel-source KERNEL_M9_FAA_NATIVE_C \
  --vendor-clear-proof captures/m9128-device-inputs-20261006/normal-faa-clear-page-runs.json

python3 tools/check-m9-faa-constant-color-plans.py \
  --green-plan GREEN_PLAN --green-blob GREEN_BLOB --red-plan PLAN --red-blob BLOB
```
