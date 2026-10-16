# Normal M9 FAA input checkpoint

The actual normal surface-owned vendor CPU frame now has a reproducible
known-byte/address manifest in
`captures/m9128-device-inputs-20261006/normal-faa-input-bytes.json`.
The runtime source is canonical `xf86-video-parhelia` revision `a9501a70`.
No GPU execution, hardware fetch or completion is supplied. Capture stops
before the genuine `23bc3` retirement counter load.

Run from the `wip/m9-faa-emulator` checkout:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9emu/m9_faa_inputs.py \
  --runtime-tools /home/ai/src/parhelia/xf86-video-parhelia/tools/faaemu \
  --pci-inventory /home/ai/src/parhelia/xf86-video-parhelia/captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources /home/ai/src/parhelia/xf86-video-parhelia/captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot /home/ai/src/parhelia/xf86-video-parhelia/captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --out captures/m9128-device-inputs-20261006/normal-faa-input-bytes.json
python3 tools/m9emu/check-faa-inputs.py \
  captures/m9128-device-inputs-20261006/normal-faa-input-bytes.json
```

The capture retains the full normal frame and complete
`m9_faa_operands.capture(frame, retirement)` manifest: 40 resource candidates,
92 actual relocations, every original pair, kind, payload, write slot, slot
owner and all matching resource/system-DMA/common-buffer owner candidates.
All actual payloads match the observed vendor dispatch table and corresponding
encoding expression. Domain annotations use the decoded address tag and
retain matching candidates; `replay_domain_selected` remains false.

The chronological DMA write list has 1,844 writes. The checker reconstructs
memory from these writes and verifies each stage-end sparse snapshot,
including the retirement packet. Snapshots must be applied in capture order;
the final memory image alone does not establish earlier command contents.

| Source | Actual address | Known CPU-written bytes |
| --- | --- | --- |
| Compiled program | GPU `1b52000` | 240, 30 instruction slots |
| Preprocess entry | GPU `1b52020` | Slot 4 in that program |
| Raster entry | GPU `1b52000` | Slot 0 in that program |
| RS database | GPU `1152000` | 115,160 |
| Preprocess constants | DMA `10c4010..10c4078` | 104, 13 pairs |
| Raster constants | DMA `10c4000..10c4010` | 16, 2 pairs |
| Geometry source span | DMA `10c4080..10c413c` | 188 |
| Metadata | GPU `1170000`, `1172000` | 128, 32 |
| Small texture parents | GPU `1181000`, `1191000` | 1,024 each |
| FAA sentinel | GPU `1119000` | 20 literal bytes |

Resource byte exports require an existing retained CPU mapping, sparse
CPU-write validity, matching CPU-published PTEs for every covered page, and
matching within-page byte offsets. The capture makes no new vendor map call.
Existing raw CPU spans are retained separately when this proof fails.
The interior small-texture resources at `1181400` and `1191400` have page
matches but lack an exact linear byte-offset proof, and have no relocation
uses in this frame. Their raw map bytes are not exported as resource bytes.

The sentinel's five actual DWORDs are `a4a3a1a1, 28041968, 0, 0, 0`.
Its remaining 108 bytes are unknown. Pool6 at GPU `11e1000` has 1,408 unknown
bytes; its genuine preparation packet occurs during resolve, after the draw.
The source manifest does not turn that later preparation into a pre-draw
zero initializer. Main, auxiliary, companion, HZ, scratch and primary
contents require actual GPU execution; untouched guest RAM is not evidence.
The program and database allocation tails are also left unknown. Known
source spans/entry observations alone do not prove hardware fetch bounds.

System DMA `10c4000` numerically overlaps allocated GPU-resource ranges;
every candidate is retained. Its constants and geometry are identified by
the genuine decoded tag2 plus actual OS DMA ownership, not by choosing a
resource solely from the numeric overlap.

The reproducible capture matches the previous retained input scan's complete
operand manifest and all resource/system-DMA known-byte spans byte for byte.
The capture records hashes of actual PCI/DC input files and the vendor DLL.
