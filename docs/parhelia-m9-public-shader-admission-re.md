# Genuine public constant-PS FAA admission

Paired `normal-faa-shader-{baseline,ps-constant}.json` captures run the actual
normal Surface-owned target association and D3D draw callback. The PS case
adds caller-owned DP2 CREATEPIXELSHADER54, SETPIXELSHADERCONST57 and
SETPIXELSHADER56 before the unchanged FVF0x44 triangle request. Its standard
ps_2_0 program is `mov oC0,c0`, with c0 green. Internal context modes, compiler
state, allocated resources and GPU replies are not replaced. Vendor/input
hashes and instruction pins are retained.

Actual CREATE dfff0 runs on a20-byte shader cache miss at10df3d, followed by
a nonzero binding lookup at108680. The actual240-byte C2 program at1b52000
changes only its first16bytes (two FS slots). All subsequent slots, including
the preprocess/VS entry at slot4, retain baseline bytes. The115160 known
resident database bytes at1152000 are identical. This proves one real public
shader-to-program join; it does not establish arbitrary shader support.

| Source | Baseline | Constant PS |
|---|---|---|
| Raster constants |10c4000..10c4010 (16B)|10c4000..10c4020 (32B)|
| Preprocess constants |10c4010..10c4078 (104B)|10c4020..10c4088 (104B)|
| Geometry |10c4080..10c413c (188B)|10c4100..10c41bc (188B)|

The added green vector precedes the original raster constants. Preprocess
constants and geometry are byte-identical at their new addresses. Bounds
come from actual kind13/14/17/18/19/1a operands and allocated owners; CPU
snapshots are checked against chronological DMA writes. Generic jobs must
rebuild these dynamic ranges even when nested compiled04 remains62af9.

Actual mode0 follows f8edd, helper mode8 f8b2d, resolve mode9 f8e36. These
internal modes differ from resource descriptor selectors. Generic FAA enable
is observed at1392e4 from real configuration/validator sources. The lowerer
sees nested compiled04=62af9. Final merged F0/EF/D7 remain
c4008600/f8004055/20005ca3 in both cases. Only fae93→faf84 calls which execute
the merge participate in merge equality checks; early returns are excluded.

```sh
python3 tools/m9emu/check-faa-shader-admission.py \
  captures/m9128-device-inputs-20261006/normal-faa-shader-baseline.json \
  captures/m9128-device-inputs-20261006/normal-faa-shader-ps-constant.json
```

Regenerate with vendor Python and `m9_faa_shader_admission.py`, canonical
`tools/faaemu` runtime, sampled inventory/resources/DC inputs and
`--case baseline` or `--case ps-constant`. No GPU execution/completion, pixels,
cache-hit reuse or unknown resident allocation tails are asserted.
