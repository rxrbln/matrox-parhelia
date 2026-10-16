# Terrain vertex-texture metadata, 2026-10-07

The installed glmark2 terrain vertex shader remains unsupported by native
Mesa: `m9_vs_compile.c:2436` rejects NIR texture instructions. It samples
`tDisplacement` for projected position and `tNormal` for the outgoing normal.
This checkpoint closes a bounded **vendor metadata** slice, not that missing
runtime feature. There is no GPU submission, live texture owner, native
fetch result, hardware pixel proof or cap change here.

The genuine vendor GLSL/frontend/optimizer/packer/finalizer compiled the
installed shader into two retained variants. The existing reproducible caller
is `captures/m9x-complex-glsl-finalizer-20261007/vendor-glsl-vertex.py`.
It uses explicit empty backend configuration; its host heap/TLS services are
not shader-result substitutes. Both full outputs are retained under
`captures/m9x-terrain-vertex-texture-metadata-20261007`.

The normal compiler run gives 18/115 slots; a 4096-byte heap gap gives 18/108.
The second ALU program and usage/mask differ. **Native byte or numeric
determinism is not established by these compiler runs.** Their texture and
paired-route descriptors are identical, and the new consumer witness checks
that narrower invariant independently.

`check-m9-terrain-texture-consumer.py` validates retained code/record extents
and then executes real pinned MxMD helpers `17faf0`, `187860`, `187950`,
`187b50`, and `18c2d0`. No vendor helper is stubbed. Its version-104 device,
zero sampler rows and null resource members are explicit host controls;
they do not represent a real texture allocation or its mapping.

| Variant | Texture entries at record+23d | Paired tokens at +28e | Published counts |
|---|---:|---:|---|
| 0 | 1 | 0 | 1 texture / 0 coordinate descriptors |
| 1 | 2 | 2 | 2 textures / 1 paired coordinate descriptor |

Source joins:

- `17fb61..17fbb2`: 20-byte texture descriptors; flag `+8 & 0x20`
  skips the extra `18c3d0` coordinate-expansion path. Every actual terrain
  texture entry has this flag.
- `18789e..1878b5`: descriptor `+8 & 31` selects a 74-byte sampler row.
  The actual terrain entries select samplers 0/1. This logical selector is
  distinct from the sequential output unit written at shadow+4b4+64*unit+c.
- `17fc1a..17fc25`: texture shadow stride is 0x64, working stride 0x88,
  compiler descriptor stride 0x14. These are separate layouts.
- `17fc93..17fd39`: two 4-byte paired tokens form one coordinate shadow;
  `18c2d0` derives its component/routing fields. Shadow stride is 0x28,
  working stride 0x18. Two compiler tokens mean one emitted coordinate
  descriptor, rather than two samplers.
- `17fd57..17fd65`: successful enumeration publishes texture/coordinate
  counts at shadow+380/+394.

The actual helpers refuse controlled five-texture and nine-paired-token
records at four working entries; alternate-policy sampler unit 31 also
returns failure before row use. All four normal/shifted variant executions
give identical translated metadata with different host placements. Pointer
identity is not used as a portable GPU address.

Reproduce the consumer check:

```sh
<faaemu-venv>/python tools/check-m9-terrain-texture-consumer.py \
  --capture-dir captures/m9x-terrain-vertex-texture-metadata-20261007/normal \
  --shifted-capture-dir captures/m9x-terrain-vertex-texture-metadata-20261007/shifted \
  --md-dll <pinned-MxMD.dll> --emulator-dir tools/faaemu \
  --output /tmp/terrain-consumer.json
```

The next required join is the real sampler row/resource transition into
address/format/filter state, its complete EE19 producer/consumer stage order,
and how sampled values reach the later vertex ALU variant. Existing generic
0x74 route entries also carry scratch feedback; their existence alone does
not prove a vertex texture pipeline. Native point/LOD filtering limits and
retained image lifetimes require source derivation before implementation.
