# Transformed vertex UV producer: actual relative command patches

The corrected public ADD-dependent UV shader produces three real compiler
variants (8/2/28 slots) and three real launch calls: coordinate producer,
texture-consuming vertex head, then raster. The two-slot producer has no texture
descriptor; the 28-slot head has one. Its GPU outputs remain unknown.

`tools/m9-vendor-vertex-coordinate-producer.py` adds a read-only instruction
dispatcher before bootstrap to the established genuine public caller. It observes
`24870`, `248f2` and `24900` while leaving all caller, resource, shader and command
inputs with their original producers. It never supplies a shader result, address,
driver return or GPU reply. The retained trace is
`captures/m9x-vertex-coordinate-producer-20261007/transformed-uv.json.gz`.

The previous generic exporter boundary at `248f2` is a chronology mismatch,
not an unknown sampler address format. At `25691..256a3`, relocation kind 1
stores the current command slot in queue field `+158` and returns before the
ordinary address encoder. Those early metadata rows contain null pairs. Later,
the real `24870` invocation carries a live command target in EAX and command
slot in ESI. `248f2..248fe` encodes

```
new_word = (old_word & 0xfff00000) |
           ((((target - slot) & 0xffffffff) >> 2) & 0xfffff)
```

Both addresses belong to the same retained OS DMA owner and are DWORD aligned.
The actual producer patches `49d00000` to `49d00090` and `49d000a2` (144 and
162 DWORDs). The trace joins each pre/post observation to the real CPU store at
`248fe`; the original command emission at `fa0b7` is also retained. These values
are observations, not portable command literals or replay parameters.

The final address-only metadata snapshot reports zero for these null-pair rows.
It must not be used to replace the earlier CPU patch chronology. The strict
exporter boundary remains present in this checkpoint: no complete transformed
UV command manifest, hardware execution, GPU fetch or correct pixels is claimed.
The direct-input native path must continue refusing a computed UV until a bounded
coordinate-output owner and its texture-consumer bindings are implemented.

The checker verifies three exact DLL instruction pins, bounded compiled-code
owners, the three actual launch roles, live DMA owner bounds and both CPU patch
stores. Six mutations reject wrong stage order, code owner, patch target, lost
command bits, a false complete export and a late observer.

```sh
<faaemu-python> tools/m9-vendor-vertex-coordinate-producer.py \
  --runtime-tools <faaemu-tools> --bootstrap-tools <m9emu-tools> \
  --pci-inventory captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --coordinate-offset --out /tmp/vertex-coordinate-producer.json
<faaemu-python> tools/check-m9-vertex-coordinate-producer.py \
  captures/m9x-vertex-coordinate-producer-20261007/transformed-uv.json.gz \
  --dll xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll
```
