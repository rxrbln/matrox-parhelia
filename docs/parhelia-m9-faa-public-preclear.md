# Public clear before the FAA triangle

The bounded public constant-red caller now inserts a genuine full black
`CLEAR42` immediately before its original `DRAW52`. Original FAA enable,
viewport, FVF and stream stride precede the clear. The caller changes only
its CPU command storage and length. Neither graph dirty flags nor the
renderer, resource descriptors, shader results or GPU replies are changed.

This is a **prefix qualification**, not a returned whole DP2 draw or a
rendered image. The later receipt-qualified observer, installed before any
public command execution, corrects the initial boundary interpretation:
actual `df9d0` enters with one full 320×240 rectangle and flags=1, black
color, depth=1, stencil=0. `75730` returns 1; `dfa52` invokes the genuine
renderer callee `13de20`, `dfa54` observes its returned EAX=1, and `dfa61`
returns from the clear handler. **All these CPU events precede the first
`4e2fc` common-buffer +0x20 read.** That read belongs to subsequent original
draw admission. Its desired query deadline is 0/0; no value is invented.
No preprocessing launch or output helper has executed at this boundary.

The historical signed capture's prose saying the stop precedes clear
renderer dispatch was wrong. Its bytes, typed operands, actual CPU writes,
owners and boundary remain unchanged. The corrective early observer and
same-prefix hardware receipt are retained by
[the continuation proof](parhelia-m9-faa-preclear-receipt-continuation.md).
The clear handler's CPU return is distinct from whole DP2 request completion
and GPU effects.
The pending source stream is not the old init-only profile. Its first 4,868
bytes exactly equal that source prefix, followed by 100 new CPU-produced
bytes. These include a second `b5e70` page clear of the actual sampled
**primary** owner, logical extent 0x50000 (80 pages), and E3 timeline b1.
The initial clear remains the companion's 0x97000 (151 logical pages).
The primary clear is reached during real public target admission; its
presence is not proof that it repairs the final FAA output.

The whole original draw caller remains stopped after the clear handler. A separately scoped genuine `23b80`
request flushes this prefix and stops before `23bc3`'s completion load:

| Item | Source result |
|---|---:|
| Posted primary bytes | 4,980 |
| Final desired source point | c0/0 |
| Retained native owners | 28 |
| Actual typed operands / non-null native sites | 36 / 4 |
| Actual direct secondary endpoints | 0 |
| Actual timeline stores | 14 |
| Source-known input bytes | 122,388 |
| Ordinary output owner | absent |

All 34 live MDL owners, 33 mapped flags30/PTE-b bindings and the one
flags110/PTE-a counter proxy remain source joined. Extra public-shader
allocation contents are unknown unless an actual CPU writer proves them.
No output allocation or zero-filled opaque input is fabricated.

The generated `m9_faa_clear_segments` has `{owner, bytes, start, end}`:

| Role | Owner | Logical bytes | Payload start | Payload end |
|---|---:|---:|---:|---:|
| Companion | 4 | 0x97000 | 0x11d8 | 0x1224 |
| Sampled primary | 1 | 0x50000 | 0x1328 | 0x1354 |

These are structural source offsets and logical extents for the bounded
profile. Native GPU bases must come from its own retained allocations.
Both six-word prefixes are `4545ace6,2,0,45e62b2a,454545e6,ffff`; the
count headers are `20000012` and `2000000a`. Both payloads end before
`4545e3e6,1`, followed by their separately tracked timeline words.
The generator checks actual producer PCs, contiguous CPU-known stores,
owner/extent joins and real vendor instruction arithmetic for both clears.
It emits `M9_FAA_PRECLEAR_CONTROL`, `M9_FAA_MULTI_CLEAR`, the two-segment
array, and absent-output phase guards. Native implementation must validate
every segment and intermediate capacity before any mutation, apply existing
address/timeline fixups first, then regenerate segments backwards so each
earlier edit moves the already-correct later suffix.

Reproduce with the established Unicorn/pefile interpreter, from the RE root:

```sh
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
  tools/m9emu/m9_faa_phase_capture.py --phase preclear-wait \
  --runtime-tools tools/faaemu --bridge-tools tools/m9emu \
  --pci-inventory captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --source-out /tmp/preclear-source.json --manifest-out /tmp/preclear-manifest.json \
  --addresses-out /tmp/preclear-addresses.json

PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
  tools/m9-native-faa-phase-template.py --manifest /tmp/preclear-manifest.json \
  --source-capture /tmp/preclear-source.json --addresses /tmp/preclear-addresses.json \
  --plan /tmp/preclear-plan.json --blob /tmp/preclear-inputs.bin \
  --kernel-header /tmp/preclear-data.h
```

Retained artifacts are
`captures/m9128-device-inputs-20261006/normal-faa-preclear-wait-*`.
`tools/check-m9-faa-preclear-prefix.py` checks that signed source/plan/header,
the exact init prefix, unchanged known resource bytes, two placements ×
eight companion × eight primary page alignments using actual vendor clear
instructions, and source refusal controls. Its backwards rewrite is an
offline source reference; it does not execute the native C producer or GPU.
The original init-only and post-triangle plans, blobs and headers regenerate
byte-exactly. Native C and physical prefix retirement were independently qualified by
the borrowed-owner correction and real c0 receipt; neither proves final FAA pixels. Any later emulator continuation requires receipts joined to
this **c0 prefix**, its live counter channels and timeline translation.
Existing b0 init-only or 140 draw/output receipts do not qualify this prefix.
