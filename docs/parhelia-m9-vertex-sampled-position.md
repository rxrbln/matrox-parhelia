# Sampled position in the original four-stage vertex pipeline

The original M9128 public compiler accepts five varyings when a vertex texture
sample changes clip Z. It writes position in the **sampled CONTINUE** stage,
not in the unsampled FIRST stage. This closes a source interface distinction;
it does not qualify native pixels or implement terrain admission.

Two genuine CREATEVS/SETVS/DRAW controls use the existing live two-texture
caller (64x64 and 128x64, explicit LOD zero). The additional public expression
is `position.z + 0.25 * sample0.x` or `position.z + 0.25 * sample1.x`. The five
varyings still consume both samples, their difference, input UV and input
position. No texel, intermediate vertex result or GPU completion is supplied.
The exact pinned MxMD source is SHA256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

| Actual role | Baseline slots | Sampled-position slots | Packed fields written |
|---|---:|---:|---|
| Raster fragment program | 19 | 19 | Raster outputs |
| Unsampled FIRST | 4 | 4 | 2, 3 |
| Sampled CONTINUE | 8 | 13 | **1 (position)**, 4, 5, 0 |
| GATHER | 8 | 8 | Four ordinary varying streams |
| LAST | 26 | 26 | Final position and first varying |

The five compiler records include the fragment program. There are four actual
preprocessing launcher calls followed by the raster call; interpreting all
five records as vertex stages would give the wrong stage interface. Original
C2/C3 operands join the live program owner and entries 19/24/38/47 for those
four stages, and entry zero for raster. The source-known 592-byte linked image
contains their exact code, with only the observed scheduling bit added to one
instruction in each record. The checker verifies code-owner bounds, typed
program references and every linked span.

Switching the clip expression between sample0 and sample1 changes exactly one
native word in CONTINUE: its scalar `MUL_RK` reads preserved r08 or r04.
The fragment program and all three other vertex programs are byte-identical,
including their comparison against the earlier five-varying subtraction
control. CONTINUE preserves live sampled pairs before overwriting their raw
texture-unit banks r00/r02 and r04/r06.
Its two final instructions retain the stage input pair r46's W and add the
sample-scaled scalar to its Z in packed output r11. These are the original
MOV, MUL and ADD forms already decoded by the compiler; the check does not
assign values to any fetched or producer register.

The emitted CONTINUE declaration has one normal float4 input and separate
coordinate attributes 5/6. Routes 74..77 are disabled for this stage. Thus this
control does not prove that position is fetched through one of those packed
texture routes, or that enabling samplers in native FIRST is sufficient.
The typed B0/B2/B4/B6 references instead join the same retained profile-80
packed owner. Their EE19 descriptors bind fields 1/4/5/0 respectively;
FIRST's B0/B2 references bind fields 2/3 of that same owner. The checker
derives owner identity from each observed operand/resource join, rather than
using a captured GPU address as a portable allocation.

The original output-register rule maps r11/r13 to **B0**, so the position
writer is field1, not field0. LAST route0 reads field0 into r00/r02 and copies
it to its varying output; route1 reads field1 into r04/r06 for the reciprocal
and position epilogue. Both route references join the same packed owner.
Their read descriptors include the original two-dword view bias (`EE190218`
and `EE190618`), distinct from the writer descriptors `EE190018`/`EE190418`.
The strict check retains that distinction and the exact original consumers.
This corrects the position-field attribution in checkpoint e0d264b: the raw
reports and the observed partition were accurate, but that earlier semantic
label selected the last bound field instead of the target for r11.

The real sampler constructor `187860` and lowerer `18b4e0` retain sequential
units 0/1, descriptors from the exact textured compiler record and the two
published texture wrappers. Both texture owners have zero source-known texel
bytes. The generic input exporter still stops at the original relative-patch
branch `248f2`; this evidence qualifies the CPU-known shader/typed binding
interface, not a complete portable command submission. The separate patch
chronology remains documented in
[the coordinate-producer proof](parhelia-m9-vertex-coordinate-producer.md).

The next native change needs a live per-stage output partition. Current Mesa
fixes FIRST to position plus two varyings and CONTINUE to the remaining
varyings; `texture_head_only` deliberately refuses a sampled FIRST dependency.
For this source shape, FIRST must publish two texture-independent requested
streams while CONTINUE publishes position and the other three, with its
normal input declaration, constants, packed field bindings and last-use
ownership derived from that stage's live roots. Native field zero currently
names position; the vendor control uses field one. The native position field
and GATHER/LAST consumer routes must remain coupled to the chosen writer,
rather than importing that source field number as a universal layout.
Removing the existing refusal alone would leave that contract wrong.

The installed terrain shader has texture-independent tangent, binormal, UV
and view-position outputs, plus sampled position and normal. That makes a
bounded partition plausible, but its implicit two-fetch LOD/header policy
and complete live register/mask interface still require qualification. These
explicit-LOD controls do not authorize implicit LOD or native terrain by
themselves. No Mesa code, cap, default gate or GPU command is changed here.

Reproduce the strict check (two controls and fifteen mutations):

```sh
<faaemu-python> tools/check-m9-vertex-sampled-position.py \
  captures/m9x-vertex-sampled-position-20261007
```

The retained reports include exact public request bytes, code owners, original
constructor instruction pins, typed operands, live resource metadata and all
PCI/resource/DC input and emulator-module hashes. To reproduce each report,
use `tools/m9-vendor-vertex-texture-position.py` with the same pinned bootstrap,
runtime and actual PCI/resource/DC input paths as the earlier routing caller,
adding `--position-sample first` or `second`, `--varyings 5 --combine sub`.
All operations run locally in the CPU emulator; no target access is needed.
