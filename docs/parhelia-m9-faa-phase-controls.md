# Independent initialization and public-draw controls

The unchanged retired main/AA hashes across two genuinely compiled fragment
programs do not establish that those stores contain rendered fragments. The
following controls separate the actual initialization work from the actual
public draw before asking the GPU to execute either complete profile. They do
not assign an ABI to the stored words or claim GPU effects from CPU production.

`m9_faa_phase_capture.py` selects two CPU caller boundaries in the existing
normal sampled-surface flow with the public registry control `MmFlags=101f8`:

- `init-only` stops after the returned public context constructor and before
  calling the public triangle helper.
- `post-triangle` executes the unchanged public `beeb0` triangle to its actual
  return (1, HRESULT 0), then stops before invoking any output helper.

The boundary raises an explicit observer/caller exception. It does not change
vendor instructions, internal state, return registers, context mode, resource
contents or GPU counters. Neither uncalled operation receives a return value.
Each retained frame independently invokes the real final queue producer
`23b80`, with its actual queue reference `+94=1`, and stops at `23bc3` before the
first `8b09` GPU status load. No reply, retirement, release or CPU pixel copy is
supplied. This independent request has its own source-produced ending and
address/timeline inventory; it is not a truncated full-output command stream.
The separate full-output and constant-green captures remain unchanged.

| Actual profile | Typed operands | Nonnull address sites | Timeline sites | Posted primary | Final target | Native owners | Known input bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Initialization only | 36 | 4 | 13 | 4880 | `b0` | 27 | 122288 |
| After public triangle, before output | 70 | 41 | 21 | 4968 | `120` | 30 | 124316 |

The initialization profile retains a 4868-byte fully CPU-written primary prefix
and produces its real 12-byte inline final packet. The draw profile retains that
same 4868-byte initialization prefix byte for byte, adds the actual pending draw
submission, and finishes differently: its real final 16-byte counted secondary
is at secondary offset `560`; the `121` token is at `56c`. Its last 12 primary
bytes are `2c/2d` endpoints referencing that secondary. It therefore has six
direct secondary fields, not the four in the existing whole output profile.
Checking its last primary word against a timeline value would be wrong.

All previously known resident input images match between the controls; the
draw adds the actual baseline 240-byte shader program. Initialization has no
triangle constant/geometry source bytes. The draw has 120 bytes of constants,
188 bytes of geometry, and actual counted secondary bytes; the final secondary
addition is captured again during its independent queue finish. The program,
metadata, command, input, mapping and unknown allocation domains are retained
from each specific frame, rather than borrowed from a larger profile.

## Explicit clear inventory

Passive `b4fe0` entry hooks retain its actual descriptor, resource target and
selected `b5030` or `b5e70` route. Resource identities are joined to the returned
owner graph. Both profiles contain these five initialization requests:

| Actual target | Resource logical extent | Selected route |
| --- | ---: | --- |
| Companion HZ | `3000` | `b5030` |
| Helper HZ | `3000` | `b5030` |
| AA companion | `97000` | `b5e70` |
| Ordinary context helper | `800` | `b5030` |
| Ordinary context helper | `20080` | `b5030` |

The draw adds one `b5030` request against the first sentinel's `80`-byte
resource. No request at these clear entry points targets the main store.
This establishes the explicit CPU clear request inventory, not its GPU-written
results or an exhaustive absence of implicit initialization inside engine
commands. Descriptor snapshots retain observed bits; unused descriptor fields
are not promoted to initialized input memory.

Both same-run captures pin device `+1c=0` and the retained `b6028` padding flag
zero. Their inline companion clear is the actual 151 logical pages, within its
152-page backing allocation. The native generator reuses the source-proved
page-run arithmetic and derives the masks/count from the live companion base.
It moves the already rebased suffix and adjusts the submitted primary length
if the new alignment changes the packet size. Unknown owners/tails are never
CPU cleared or filled with speculative GPU data.

## Native preparation and qualification

The phase-specific generator invokes the strict whole-profile checker before
building a plan, blob and header. Each source-known MDL byte follows its unique
live descriptor/PFN/compact-PTE mapping. Flags30 owners use the observed B
mapping policy and `[0,pagecount,0]` companion metadata; the retained common
counter alias uses the separately proved flags110/A policy. Direct secondary
start/end fields, all observed timeline stores, mapped `106c`, and the two
physical common counter channels retain their source domains. The source
restore image and source-init/cache ownership requirements remain unchanged.

Actual kernel encoder/clear helper tests pass for both profiles at two native
placements and all eight page alignments, including source encoder equality,
unknown-byte and other-owner preservation, exact moved suffixes, the correct
final tracker owner/offset, and rejection of corrupted/undersized clear inputs.
The existing normal output plan, blob and generated header remain byte-exact.

Generated headers use `M9_FAA_PHASE_CONTROL=1`, `M9_FAA_HAS_OUTPUT=0`, and
`M9_FAA_OUTPUT_OWNER=ffffffff`; `M9_FAA_PHASE_INIT_ONLY` distinguishes the two.
No ordinary output owner is invented. The current native output-owner helper
returns NULL before using that sentinel index, so no output read is admitted.
Source graph main/aux/companion/pool owners stay 2/3/4/22 in both controls;
primary command owners are independently derived as 26 and 27. All generic
native allocation/upload/encoder loops use the generated arrays, not the
larger output profile's owner count.

Build each entire header with a distinct firmware name
`m9x/faa-init-only-known-inputs.bin` or
`m9x/faa-post-triangle-known-inputs.bin`. Keep both controls and the constant
shader control mutually exclusive, default off, and preserve all existing
private-device/source-init/cache/one-shot gates. The positive sampled-surface
CPU control can precede submission; these phase cases have no output READ or
pixel oracle. Observe retained main/AA/pool/other owners before and after real
retirement. An initialization hash match versus a draw hash match is hardware
evidence still to be collected by the target owner; neither is implied by
these CPU fixtures.

Host-only complete module builds using canonical kernel `73c2a45` plus each
whole header/distinct firmware string passed. Module SHA256:

- Init-only: `d9d1ccf18e61e1edc0dd9032243cd33d86b031c87dd3e8c752bdbd29c2863eeb`.
- Post-triangle: `2e25c44bcd544482088008dc039ae5715fa6f839761847b77445f15bf6ab1834`.

These are isolated build checks, not deployed hardware candidates. The parent
owns integration, final build selection and all physical qualification.
The outer full-PDEV/global engine admission audit remains separate.

## Reproduction

From the RE repository, run each phase with:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/m9_faa_phase_capture.py \
  --phase init-only --runtime-tools tools/m9emu --bridge-tools tools/m9emu \
  --pci-inventory captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --source-out /tmp/faa-init-source.json --manifest-out /tmp/faa-init-manifest.json \
  --addresses-out /tmp/faa-init-addresses.json
```

Use `--phase post-triangle` with distinct outputs for the second independent
run. `tools/m9-native-faa-phase-template.py` consumes only a phase's manifest,
source and address artifact. `tools/check-m9-faa-phase-comparison.py` checks
both retained captures and the identical initialized prefix. The native helper
fixture is `tools/check-m9-faa-phase-native-cpu.py`, with explicit plan/blob,
phase header, actual kernel C source and vendor page-run proof arguments.
All signed artifacts are under
`captures/m9128-device-inputs-20261006/normal-faa-{init-only,post-triangle}-*`.

Canonical integration is kernel `d2821c3`: optional mutually exclusive
`DRM_M9X_FAA_INIT_ONLY_CONTROL`/`DRM_M9X_FAA_POST_TRIANGLE_CONTROL`, excluded
by the constant shader control, with compile-time mixed-selection refusal.
All four root full builds and exact firmware metadata checks pass.
`m9-native-faa-image DEVICE --phase-control` requires nonzero real retirement
and actual absent-output `ENODATA`; it performs no pixel oracle.
[Root integration evidence](../tests/m9-faa-phase-integration-20261006/README.md).
