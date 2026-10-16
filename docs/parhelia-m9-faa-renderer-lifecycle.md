<!-- SPDX-License-Identifier: GPL-2.0-only -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# Renderer publication and fragment-pointer reset boundaries

The bounded public-red source fixture selects its renderer through the real
fresh queue activation. It does not substitute a renderer vtable or compiler
mode. This closes a possible wrong-subclass selection inside that activation
prefix; it does not establish full Windows PDEV admission or correct GPU
rendering. The final output request remains the explicit internal `ac1f0`
experiment below the unjoined public read admission.

At `23123`, actual queue `+30` is null. The genuine activation allocates `644`
bytes, directly calls `f7eb0` at `23140`, and publishes the returned object to
queue `+30` at `2314b`. That prefix contains no PCI, chip or profile branch.
`f7ebf` unconditionally installs `1f48e8`. A passive write observer retains
exactly this one write to the renderer's vtable word through the complete
public triangle and internal ordinary-output request. All ten `f85c0` calls,
seven `fadf0` calls and the final `f8e36` use that same renderer and queue.
Five real `f5730` calls come from `f4f50` at `f5197`, with return `f519c`.
They are direct calls selected by the program-shadow dirty bit, not an
alternate renderer vtable. The observer's field named `stream` at `f5730`
records ECX, which is the renderer passed by `f4f50`; its command stream is
renderer `+4`.

The retained MM property-query replies and registry control `101f8` are the
same real runtime/configuration bridge used by the existing fixture. The
queue source remains `1a260 → 230f0 → 1ff90`; its shader-source alias is the
returned renderer `+c`. This proves the actual fixture's CPU publication,
not an independently completed public device-construction call chain. The
separate miniport/PDEV admission audit owns that remaining boundary.

`RESET_FRAG_PTR` is renderer vtable slot `+1c`, pointing to `fab30`.
Its emitter writes `E6=3, E6=3, A1=A3=A4=0`. The trace executes no `fab30`,
`aa926`, `aaf05` or `acb20`. Absence alone does not establish an omitted reset.
Two source-proven indirect reset sites, `aa926` and `aaf05`, occur in the
sampled-resource copy helper `aa750`. Both follow a genuine `b4fe0` operation,
and both are gated by a whole-shape operation plus source outer `+18` and
nested outer `+3c`. They are not unconditional prerequisites of `ac1f0`.
The admitted `a74b0` read prefix, `a74f3..a752d`, contains only the request and
resource `+3fc` tests, argument extraction and the call to `ac1f0`. It emits
no prior copy/finalizer/reset. `acb20` is a separate owner-cleanup path; its
pinned body invalidates the cached sentinel pair and releases resources,
without a direct `fab30` call.

The current ordinary-output flow instead runs `fce50 → fc8f0`: dirty bit8
selects the old saved sentinel and pool pairs; `fc99b` invokes stream slot98
(`24f20`) before the A1/A3/A4 saves. Its pool range is `+284/+2c4`. The new
ordinary destination has a null sentinel, so the subsequent `fcb60` supplies
no new sentinel range. These are actual source choices, not a demonstrated
native omission. The exact GPU semantics of the pool transition, state-save
width and resulting main/AA record schema remain unproven. No reset, pool
initialization or opcode change is proposed by this audit.

The source capture stops at genuine `afbe0` entry, before CPU copy, waits or
GPU inputs. It retains the real public-red caller request, full source queue
bytes and returned owners. There is no emulated GPU execution/completion.
The checker pins shipping instructions, constructor/vtable publications,
actual routes, source/runtime joins and the separately gated reset callers.
Wrong vtable, repeated publication, an added reset, a nonfresh renderer and
a supplied completion are rejected.

Reproduce locally, without target access:

```sh
PYTHONPATH=tools/faaemu:tools/m9emu /home/ai/tmp/faaemu/venv/bin/python \
  tools/m9emu/m9_faa_renderer_lifecycle.py \
  --contract-driver tools/m9-vendor-faa-read-contract.py \
  --lifecycle-out /tmp/faa-renderer-lifecycle-red.json \
  --public-constant-red --runtime-tools tools/faaemu \
  --bridge-tools tools/m9emu --scatter-tools tools/m9emu \
  --pci-inventory captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --out /tmp/faa-renderer-lifecycle-contract.json \
  --source-out /tmp/faa-renderer-lifecycle-source.json

/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-renderer-lifecycle.py \
  captures/m9128-device-inputs-20261006/normal-faa-renderer-lifecycle-public-red.json
```

The signed artifact links the retained source and contract by SHA256, and
pins the observer itself. The checker reads the shipping MD image by SHA256.
This is a source-admission/lifecycle audit, not a new hardware candidate.
