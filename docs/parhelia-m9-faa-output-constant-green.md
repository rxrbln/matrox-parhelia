<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Whole ordinary FAA output with a public constant-green shader

This is a separate source control for the unresolved interpolation dependency.
It does not identify the black-image cause or prove hardware FAA rendering.
The real DrawPrimitives2 caller receives CREATEPIXELSHADER54, constant57 and
binding56 requests before its unchanged triangle. The shader is standard
`ps_2_0 mov oC0,c0`, with `c0=(0,1,0,1)`. The real handlers normalize, bind
and compile it. No internal compiler state or instruction word is patched.

`tools/m9-vendor-faa-read-contract.py --public-constant-green` installs the
caller-owned prefix through `m9_faa_public_constant.py` after the real allocator
is available, before the real public draw. The baseline option is unchanged.
The retained contract includes exact original draw bytes, full public prefix,
shader tokens and helper hash. The public draw returns1 with HRESULT0.
The same normal sampled primary, FAA graph, render states, viewport and
320-by-240 triangle remain admitted. Public `a74b0` copy admission remains
unclosed; ordinary output continues through the explicitly scoped internal
`ac1f0` helper, as in the baseline.

The complete fresh source artifacts are named
`normal-faa-output-constant-green-{input-manifest,source,contract,addresses}-101f8.json`.
The original copy stops before its actual `4e2fc` common-buffer read. The
independent real `23b80` finishing call appends its twelve-byte footer, posts
the live primary and stops before `23bc3` reads GPU status. Neither call
receives a GPU counter value or pixel data. The output's entire `50000`
extent remains source-unknown and the temporary remains live.

| Source result | Interpolated baseline | Constant-green control |
| --- | ---: | ---: |
| Primary before/after flush | 5584 / 5596 | 5584 / 5596 |
| Secondary intervals | 604 + 772 | 604 + 748 |
| Constants | 120 | 136 |
| Geometry bytes / MDL offset | 188 / 80 | 188 / 100 |
| Typed operands | 90 | 90 |
| Nonnull callback fields + direct endpoints | 47 + 4 | 46 + 4 |
| Timeline fields / desired low | 25 / 140 | 25 / 140 |
| Native owners / source-known blob bytes | 31 / 124928 | 31 / 124920 |

Byte quantities/counts are decimal; geometry offsets and desired low are hex.
Actual triangle kind`1e` at secondary offset`440` becomes a null/control
operand. Its indexed payload is register03. It is preserved as the actual
zero value and receives no native address fixup. The two direct secondary
endpoint pairs use the actual shorter604/748 intervals. No old1376-byte
secondary or51-field address inventory is reused.

The actual240-byte program SHA256 changes from `2efd8e0b...` to
`46e77ecc6215fd403a354d0c8a9ace2c16868e434d87f6fb02efae2a6614fe55`.
Only its first sixteen bytes, the two FS slots, change. VS and terminal
bytes16..239 are identical. All other known resident resources are identical,
including the115160-byte RS database,128-byte table,32-byte metadata, both
1024-byte texture inputs and both sentinel initializers. The actual
preprocess constants retain104 identical bytes; the actual raster range
becomes the public16-byte green constant followed by its prior16-byte span.
The188 geometry bytes remain identical at the new aligned offset.

The whole manifest checker now exposes its existing assertions as a callable
core. Its baseline CLI retains the original strict counts and program hash.
The separate `check-faa-output-constant-green.py` checks this explicit control's
exact public tokens, resident retention, constants and geometry in addition
to all original MDL/PFN/metadata, chronology, typed operands, direct sites,
counter branch, clear flag and footer checks. It does not accept arbitrary
programs or relax the baseline case. The native generator requires an explicit
`--public-constant-green-control --baseline-manifest PATH` selection and runs
that control checker before deriving the new plan.

The profile-specific artifacts retain the old plan/source untouched:

```text
normal-faa-output-constant-green-native-plan.json
normal-faa-output-constant-green-known-inputs.bin
normal-faa-output-constant-green-kernel-data.h
```

Manifest SHA256 is
`6dd806d783ef7b16ed11534c522bad470ee80f3308c389265af9de346356c010`.
Blob SHA256 is
`90a0b8d5ddd8599d607b6749f8aac863225499aa1f290075c7e2deb130640c18`;
header SHA256 is
`deb884f88dc54377bd32d1654de5a55b1321dd0c985abbecbd160fa4501facee`.
The plan pins all four source artifact hashes and the baseline manifest hash.
Kernel6adb92f now selects these complete tables with the default-n
CONFIG_DRM_M9X_FAA_CONSTANT_SHADER_CONTROL build option and distinct firmware
m9x/faa-constant-green-known-inputs.bin. Existing private gates/ownership are
maintained. Both profile builds and actual ownership fixtures pass.
[Physical qualification](../tests/m9-faa-constant-green-hardware-20261006/README.md)
reaches140 without GPUtimeout, but all visible pixels are black and main/AA
hashes match the interpolating baseline. The control did not resolve FAA.

Validation passes the complete source control checker and the unchanged
baseline checker. Regenerating the baseline still gives its exact original
plan and blob. The actual native encoder and variable-clear CPU fixture,
with its explicit candidate header argument, passes50 fixups/25 timelines at
two native placements and all eight page alignments. It verifies source
encodings, counts, shifted suffix, secondary/geometry preservation, unknown
output preservation, actual140 footer and rejection before mutation.

Reproduce capture through the existing `m9_faa_output_manifest.py` exporter:
use the normal whole-output arguments, `--continue-to-gpu-input`, actual
`101f8` registry control, independent flush/address audit, this read-contract
driver, and its additional `--public-constant-green` flag. Validate with:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-output-constant-green.py \
  MANIFEST --source-capture SOURCE --contract CONTRACT --address-audit ADDRESSES \
  --baseline-manifest BASELINE

/home/ai/tmp/faaemu/venv/bin/python tools/m9-native-faa-output-template.py \
  --public-constant-green-control --baseline-manifest BASELINE \
  --manifest MANIFEST --source-capture SOURCE --contract CONTRACT \
  --addresses ADDRESSES --plan PLAN --blob BLOB --kernel-header HEADER

python3 m9x/tests/faa-output-template-cpu.py --plan PLAN --blob BLOB \
  --kernel-header HEADER --vendor-clear-proof CLEAR_PROOF
```
