# Public vertex texture caller and retained stage inputs

The corrected public VS3 TEXLDL caller joins caller UV bytes to emitted
texture-input attribute 5, a genuinely allocated vertex texture, sampler
lowering, native program variants and complete direct-UV CPU command inputs. This is stronger than the earlier null-resource metadata witness. It
is **not** a Mesa vertex-fetch implementation or hardware/pixel result.
Native terrain remains unsupported pending that implementation and qualification.

`tools/m9-vendor-vertex-texture-public.py` reuses the real normal-surface
context/queue bootstrap from the FAA tools. It supplies explicit public draw
inputs, allocates a texture through real `a3870`, publishes its handle through
`7a770`, and calls the original DP2 draw handler. No shader/compiler/resource
helper is replaced. No texture contents, sampled values, producer results or
GPU completion replies are supplied. Actual final-queue preparation stops before
its first GPU-written status read. The resource's unknown bytes stay unknown.

The caller uses classic FLOAT4 position register 0 and FLOAT4 UV register 7;
the VS3 DCL declares POSITION0 on v0 and TEXCOORD0 on v1. UV.z/w are zero,
so standard VS3 TEXLDL explicitly selects LOD zero. Its public VS copies
position, samples s0, and copies the sample to color. The public PS2 shader only
copies incoming color. There is no pixel texture instruction or fixed-stage
texture bind that could account for the observed sampler relocation.

## Declaration correction and observer scope

The original `f32595e1` captures used legacy register 1 for UV, which maps to
BLENDWEIGHT0 rather than the VS3 shader's TEXCOORD0. Their genuine resource,
sampler and launcher observations remain useful, but they did **not** prove
caller UV binding. Those original bytes and their caller are retained under
`semantic-mismatch-*` names as controls, including their default-coordinate
word `00500b24`. They are not implementation or UV-input proofs.

The actual parser at `10b7b0/10b7b8` reads two **bytes** per legacy register
from `1f66ac`: register 0 maps `(POSITION,0)`, register 1 `(BLENDWEIGHT,0)`,
and register 7 `(TEXCOORD,0)`. The corrected observer reads these actual table
bytes; the checker joins register 7's map to shader DCL semantic 5 and the
actual producer declaration `00100d88,001006b6,04500688,00004b08`. Its third
word selects caller DWORD 4 (UV at byte 16), attribute 5 and the XY components.
Thus the proof depends on real emitted words as well as public tokens.

All instruction observers, including the sampler lowerer's return observer,
are installed before bootstrap execution. A global dispatcher observes the
real return PC; no late hook can silently miss an already translated Unicorn
block. Its observed return and after-state are required by the checker.

## Public resource and shader joins

These are source addresses in pinned MxMD.dll, SHA256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`:

- DP2 CREATEVERTEXSHADER 45, `dfc20`: payload handle/declaration-byte-count/
  code-byte-count, then declaration and bytecode. Declaration goes through
  `10f060`; real shader installation calls `10f210`, then frontend `191130`,
  optimizer `1958a0`, finalizer `10ec20`.
- DP2 SETVERTEXSHADER 47, `dfec0`: selects the actual created handle. The
  declaration/parser handle is used instead of a fabricated compiler mode.
- Public texture-stage packet 25 with stage 257 selects the vertex sampler bank.
  Actual resource binding reaches `108fc0`; it writes the wrapper to
  `ctx+2518 -> +14 + unit*4a +10`. `105340/105cf0` select the same bank for
  public filter/wrap state. Fixed/pixel stages are not bound by this caller.
- `17faf0/187860` select the actual compiled texture descriptor and vertex
  sampler row. `18b4e0` returns 1 and publishes the real resource wrapper,
  vtable `1f3a90`, derived tagged root, dimension, pitch and filter fields.
  The resource wrapper matches the actual handle publisher exactly.
- Resource selector 2 joins the retained constructor layout to final descriptor
  fields. The checker derives index78 from aligned horizontal extent, layout
  selector, flags bit3, flags+14c bit2 and format code; index79 derives from
  height and depth. The 128 case sets index78 bit21 through actual constructor
  flags. It is not forced to the 64 case's descriptor constant.

Both independent caller runs use point MIN/MAG, no mip filter, wrap U/V,
64x64 or 128x128 profile12 RGBA allocation. Their actual retained texture extents
are 17,408 and 69,632 bytes. Both have zero known texel bytes. Index78 changes
from `cc010040` to `cc210080`, index79 from `00010040` to `00010080`; real
lowering produces operation `40040918`, filter `00009200`. The texture root
coincidentally has the same allocator position in these runs; the later native
program root changes with allocation size. All addresses derive from current
owners and source semantics. None is a replay parameter.

## Direct UV two-launch sequence

The actual linked caller produces two compiler variants:

| Variant | Slots | Texture descriptors | Role in submitted command inputs |
|---|---:|---:|---|
|0|8|0|Final raster consumer, C2 entry 0|
|1|28|1|Vertex producer consuming texture, C2 entry 8|

The actual retained C2 BO has 37 known slots, including linked termination.
The first real launch uses entry 8; the sampler resource relocation kind 0b
occurs before that launch. The final real launch uses entry 0 and no texture
entry. Separate retained producer output owners feed the final stage through
kind 1b..1e bindings; the CPU proof never fills these owners with sampled values.
The submitted index88 producer word is `01103f00`; its texture-input group
bit 20 is set. Final index88 is `01089ff0`, with that group disabled. These are
observations of the real emitter, not recommended Mesa literals. Correct mask,
source attribute/coordinate placement and final bindings must derive from the
compiled program and current owners.

The capture retains public packets, actual compiled objects/code owners,
working sampler/backend states, CPU-written DMA chronology, sparse known
resource inputs, PTE/alias checks and relocation ownership candidates. It
keeps namespace ambiguity explicit rather than selecting an invented domain.

The corrected `--coordinate-offset` / `transformed-uv.json.gz` control adds
a standard VS3 ADD to UV before TEXLDL and produces variants 8/2/28 and
three real launcher calls in producer, texture-consumer, raster order. Its first 2-slot
producer has no texture input group; the next 28-slot producer has one. The
original two-launch exporter refuses unjoined payload key `248f2` with a
retained KeyError boundary instead of exporting this sequence as a complete
three-stage contract. The actual CPU draw caller and real queue preparation
reach this boundary; no failure or success of GPU execution is inferred. This extra-coordinate-producer path
remains implementation work. Direct-UV support must refuse such dependencies
until that producer and its additional binding contract are implemented.

## Reproduction and limits

Retained proofs and hashes are in
`captures/m9x-vertex-texture-public-20261007`. The source input inventory,
PCI resources and DC snapshot are the established
`captures/m9128-device-inputs-20261006` inputs. Imported bootstrap/helper source
hashes are included in each proof. Example:

```sh
<faaemu-python> tools/m9-vendor-vertex-texture-public.py \
  --runtime-tools <faa-runtime-tools> --bootstrap-tools <m9emu-tools> \
  --pci-inventory captures/m9128-device-inputs-20261006/pci-inventory/inventory.json \
  --pci-resources captures/m9128-device-inputs-20261006/pci-resources.txt \
  --dc-snapshot captures/m9128-device-inputs-20261006/dc-banks-idle-e4611b75.csv \
  --texture-size 64 --out /tmp/public-vs64.json
python tools/check-m9-vertex-texture-public.py \
  captures/m9x-vertex-texture-public-20261007/public-vs64.json.gz \
  captures/m9x-vertex-texture-public-20261007/public-vs128.json.gz \
  captures/m9x-vertex-texture-public-20261007/transformed-uv.json.gz
```

Run the caller separately with size 128 for the second control, and with
`--coordinate-offset` at size 64 for the transformed-coordinate control. The independent
checker validates public token/declaration sizes, actual sampler owner and
selector 2 layout derivation, unknown texels, bounded compiled-code owners and
retained C2 entries. It rejects the wrong legacy semantic, default emitted UV coordinates, wrong
vertex sampler owner, false known texel and code outside its retained owner. These are CPU proof negative controls;
no pixels or successful native hardware fetch are inferred.
