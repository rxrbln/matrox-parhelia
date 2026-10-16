# M9128 FAA CPU input chronology

This bounded audit checks the four receipt-qualified original caller phases,
ending before the actual180 output write wait. It replays every recorded CPU
MDL store and compares actual byte images rather than trusting the exporters'
`unchanged` booleans. The original full manifest checker separately validates
producer identities, typed domains and physical receipt joins. No source GPU
reply, unknown backing byte or hardware action is added by this audit.

| Original caller phase | Known MDL bytes | Known local bytes | Recorded CPU stores |
|---|---:|---:|---:|
| preclear wait | 4968 | 117408 | 1246 |
| post-draw wait | 6028 | 117648 | 1511 |
| output query wait | 6892 | 117648 | 1727 |
| output write wait | 7404 | 117648 | 1855 |

Every earlier CPU-known byte is identical in the later phases. The only new
local input is the actual240-byte compiled C2 program. Both20-byte sentinel
prefixes remain their same original CPU literals. Constants136 bytes and
geometry188 bytes have no CPU rewrites; the1356-byte secondary stream has none.
All7404 final known MDL bytes match chronological actual stores.

Four primary words change0 to4 at224/268/2ac/2f0 during the original prefix,
written by248e7. Actual DLL instructions248dc..248e7 AND the old word with
ffffff84, merge ECX&3 and set bit2. The distinct248f2..248fe producer patches
count through an XOR mask; these four changes must not be labeled count
updates. The tool checks both instruction byte spans in the pinned original
DLL SHA b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43.

Thus uploading the final CPU-known images does not replace earlier constants,
geometry, sentinel or resident metadata values with later CPU values in this
specific180 flow. This does not prove the ordering of CPU/MMIO barriers,
unknown GPU-produced state, the hardware record ABI or correctness of FAA
fragment generation/resolve. Earlier-phase data remains available; no unknown
GPU record is initialized or exported as known input.

```sh
python3 tools/m9-faa-temporal-input-audit.py \
  --captures captures/m9128-device-inputs-20261006 \
  --vendor-dll /path/to/MSerX/MxMD.dll \
  --out /tmp/m9-faa-temporal-input-proof.json

/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/check-faa-preclear-output-write-manifest.py \
  --manifest captures/m9128-device-inputs-20261006/normal-faa-preclear-output-write-wait-input-manifest-101f8.json \
  --source-capture captures/m9128-device-inputs-20261006/normal-faa-preclear-output-write-wait-source-101f8.json \
  --addresses captures/m9128-device-inputs-20261006/normal-faa-preclear-output-write-wait-addresses-101f8.json
```

Both checks pass. Independent corruption controls reject changes to the final
sentinel prefix and prior constants even when supplied snapshot booleans remain
unchanged. Exact input hashes and original instruction pins are retained in
`tests/m9-faa-temporal-inputs-20261007/proof.json`.
