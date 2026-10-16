# M9128 FAA queue through the real MDL miniport request

The genuine `MmFlags=101f8` normal FAA CPU caller now reaches triangle draw
and resolve without an emulated GPU reply. The previous queue boundary was
an unimplemented OS bridge for `EngDeviceIoControl23306c`, not a vendor format
refusal. The bridge accepts only the observed eight-byte input/twelve-byte
output shape and executes actual miniport code. `MxMM.sys6c828..6c83b` selects
the `233000` family with request in EAX and owner in ECX. `7190e` dispatches
`23306c` to `7203d`; that function calls genuine `247f4`/`2482e` wrappers for
the existing bounded NT allocation and mapping model. Actual guest code
returns `{CPU_virtual, MDL_byte_count, MDL_handle}`.

The wrapper supplies LowAddress1MiB, HighAddressU64_MAX, SkipBytes1MiB and
the requested byte count. The bounded host satisfies all owned pages within
the first allowed range. It models neither additional range iterations nor
fragmented/partial allocations. This use of SkipBytes follows the
[Microsoft API contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmallocatepagesformdl).
Synthetic OS physical pages remain distinct from captured GPU addresses.
Actual MDL descriptors, PFNs, live mappings and NT API calls are retained.

The retained control has33 actual allocation/map replies,34 MDLs including
the existing counter owner, and166 observed miniport/wrapper entries.
`normal-faa-mdl-queue.json` records actual outputs, instructions and the
vendor image hash. The checker verifies each reply against the actual
descriptor and page ownership and refuses corrupted reply/PFN or invented
GPU-completion/full-stream claims. The existing host test verifies the real
wrapper's first-range pages and lifecycle and retains its refusal controls.

The frame observer now records relocation payloads from the actual guest
store independently of its contiguous-owner inventory. MDL buffers are
separate owners; no contiguous owner or physical-selector2 interpretation
is synthesized for them. A failed queue initialization also retains its
original error instead of masking it with an unbound queue variable.

This checkpoint proves returned CPU queue/draw/resolve production. It does
**not** prove a complete chronological MDL command-byte/address manifest,
native table consumption, GPU completion, fragments or a working FAA image.
The complete MDL ownership observer and secondary record metadata audit are
separate ongoing work. The existing native image remains black.

Reproduce from the RE checkout:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/m9_faa_mdl_queue.py \
  --runtime-tools tools/faaemu \
  --pci-inventory captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --out captures/m9128-device-inputs-20261006/normal-faa-mdl-queue.json
python3 tools/m9emu/check-faa-mdl-queue.py
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-mdl-host.py
```
