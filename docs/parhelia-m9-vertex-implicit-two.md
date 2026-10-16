# Two implicit vertex fetches: bounded source contract

The original GLSL frontend, optimizer, packer and finalizer compile
`texture2D(image, uv) - texture2D(other, uv)` into one record containing two
implicit 2-D descriptors. The shader uses direct input XY. The frontend
probe retains its controlled empty backend scope; it is not a complete
linked GLSL draw or GPU execution.

`captures/m9x-vertex-implicit-two-20261007/compiler` contains that independent
compile. `caller.json.gz` is a genuine public D3D TEXLDL two-fetch SUB caller,
with distinct retained 64x64 and 128x64 texture owners. `helpers.json` then
executes original `be290`, `11a650` and `187950` using explicit policy 0/1
and the GLSL-produced semantic descriptors on those two live owners.
Captured metadata pointers are not supplied to the helper. Each descriptor
has its own new owned CPU buffer.

Both original helpers emit `40040118` with zero policy and `00040118` with
nonzero policy. Sequential bank 0/1 and each resource, sampler, size, format
and address remain unchanged. Every other live backend word is preserved.
The completed public caller has already consumed backend dirty/pending
words at +0/+58; the helper compares its immediate before/after snapshots,
not an earlier lowering snapshot for those two words.

The existing source pins at `187978` use full-DWORD policy, and `187a51`
separately selects the explicit-LOD operation from descriptor bit8. This
proof extends the prior single-descriptor policy control to two distinct
owners. It does not establish a complete mode-1 public draw, sampled values,
linked packed register masks, or GPU pixels.

For native admission, [GLSL 1.20 section 8.7](https://registry.khronos.org/OpenGL/specs/gl/GLSLangSpec.1.20.pdf)
selects the base image for a vertex-stage implicit lookup. The current
native runtime additionally requires level 0 only, nearest min/mag,
no mip filter or bias, normalized repeat XY and identity swizzle. Under
those restrictions, implicit and explicit zero LOD share the same bounded
sample operation. This does not admit gradients, bias, computed coordinates,
nonzero LOD, other texture dimensions, mip chains or linear filtering.

Verify the sealed CPU proof:

```
/home/ai/tmp/faaemu/venv/bin/python tools/check-m9-vertex-implicit-two.py \
 captures/m9x-vertex-implicit-two-20261007 \
 --dll xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll
```

The checker joins both source descriptors and both actual resource/bank
owners, original DLL instruction bytes, all four helper controls, and nine
negative controls. The wrapper `tools/m9-vendor-vertex-implicit-two.py`
reconstructs the real public caller and repeats the CPU helper controls.
Native two-fetch, packed and sampled-position switches remain opt-in pending
hardware qualification. Installed terrain remains a feature target, not a
completed application proof.
