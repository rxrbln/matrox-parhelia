# Vertex sampler header policy: actual caller and CPU helper proof

The sampler header bit30 is a shared context policy, not vertex versus
fragment stage. A genuine public mode0 caller uses `40040918` for TEXLDL;
the source constructor and sampler builder, with the actual GLSL implicit
2-D descriptor and nonzero policy, produce `00040118`. This closes that
bounded CPU header contract. It does not qualify native GPU vertex sampling.

The capture directory is
`captures/m9x-vertex-texture-header-policy-20261007`.
`direct-helper.json.gz` retains the successful direct-UV public caller and
helper controls. `mode1-boundary.json.gz` retains the genuine mode1 context
initialization followed by the strict missing callback in its triangle path.
The latter is not a completed public mode1 draw.

## Initialization and consumer source join

The sole direct `be290` caller, `75489`, passes parent `753e0` argument8,
`[EBP+24]`, as initializer argument3. Initializer `be4ee/be4f9` reads
`[EBP+10]` and stores it at snapshot+c. The original public callback `bec70`
extracts the high WORD of request+10 at `becd6/becdf`; `bedad` supplies it to
`753e0`. The explicit caller requests are kind4/mode0 and kind4/mode1.
Alternative constructor caller `d0521` supplies literal1 at `d050f`; its
complete API identity is not asserted here.

Two-pass observation first discovers the header's unique live allocator
owner (ordinal706, 212 bytes, offset0). The second fresh execution observes
that allocation's relative first16 bytes from bootstrap onward. It sees the
vendor poison initialization and actual `be4f9` stores0/1. No captured CPU
or GPU address is used to execute a command or select a resource.

`11a68f..11a69a` copies header+c to shader shadow+de. `187978` compares the
**full DWORD** against zero, then `18797f..18798e` sets bit30 exactly when
zero. This is independent of descriptor+0c bit8, which selects explicit LOD
operation9 at `187a51`. The vertex and pixel program headers in the original
public mode0 caller share the same live snapshot; assigning bit30 by shader
stage is contradicted by the actual producer.

## Bounded actual helper controls

After capturing the unchanged successful public caller, the tool executes
real `be290`, `11a650`, and `187950` on its retained header/compiler/resource
and sampler owners. The constructor's EDI comes from the actual outer
context+c, as at `75482`, rather than the top-level device object. The
explicit constructor policy is0/1. Only semantic texture metadata is taken
from the actual GLSL frontend compiled-object record; its pointers and code
addresses are never replayed. That direct descriptor is
`00800006 00030002 008f04a0 0ffffc00 00000000`.

The sampler builder produces `40040118`/`00040118`; header+c and shadow+de
are0/1 and the live resource identity stays fixed. This is a CPU helper
witness under declared policy controls. Texture bytes, sampled values,
producer records, GPU work and GPU completion are not supplied or inferred.
The existing native fragment header uses the same nonzero-policy result;
initial native vertex work can preserve that bounded header without copying
the mode0 D3D TEXLDL word.

`composed-zero-lod.json.gz` is a separate public MOV-composed UV.xy plus
constant-zero Z/W control. It still produces variants8/2/28 and launches
FIRST/MIDDLE/LAST. It is evidence that composed coordinates require another
producer, not a direct-UV two-launch proof. Its unjoined payload exporter
boundary remains recorded.

## Verification

Run the checker with the pinned DLL:

```
python tools/check-m9-vertex-texture-header-policy.py \
 captures/m9x-vertex-texture-header-policy-20261007/direct-helper.json.gz \
 captures/m9x-vertex-texture-header-policy-20261007/mode1-boundary.json.gz \
 captures/m9x-vertex-texture-header-policy-20261007/gl-compiled-object.bin \
 --dll xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll
```

Seven exact source byte pins and five rejected mutations check caller policy,
writer provenance, stage inference, resource identity and LOD-mode confusion.
The public capture tool accepts `--context-mode`, observer-only
`--watch-header-allocation/--watch-header-offset`, and optional
`--probe-gl-header-policy <compiled-object> --gl-record-offset 0x320`.
Its mode1 boundary intentionally exits unsuccessfully after saving evidence.
GPU pixels and full terrain remain pending implementation/qualification.
