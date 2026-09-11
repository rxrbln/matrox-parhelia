# Parhelia hardware polygon offset: exact GL command path

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

Parhelia does not expose polygon offset as five linearly addressed MMIO
registers.  The five key-`0x05` entries emitted by the vendor GL ICD are input
records for the Linux vendor kernel's state translator.  Three are software
enable predicates.  The factor and units are transformed and uploaded to the
GPU's internal constant file as:

```
c[0x178] = { slope_factor, depth_unit_bias, 0.5f, 786432.0f }
```

The upload uses the already proven opcode-`0x41` block-load mechanism.  There
is no evidence-based physical MMIO address for `c[0x178]`, and none is needed.
Writing an invented address such as `0x1000 + 0x15*4` targets the byte-indexed
register file instead and corrupts unrelated state.

The saved board capture independently proves the translated values and the
draw-side activation state for `GL_POLYGON_OFFSET_FILL`:

```
c[0x178]              = { 1.0f, 0x1.000002p-24f, 0.5f, 786432.0f }
0x10f0 bind/arm mode  = 0x20000086 / 0x00000086
draw header           = 0xd7080001 (ordinary 0x97080001 plus bit 30)
```

For the captured S8Z24 surface, `0x33800001` is the correctly rounded
single-precision result of `1.0f / 16777215.0f`.  The API call was
`glPolygonOffset(1.0f, 1.0f)`.

## 1. API state is not a hardware register list

The 64-bit Linux ICD stores the two API floats without conversion in its
polygon object:

```
libGL64.asm 0x1ce100  state+0x1c = factor
libGL64.asm 0x1ce105  state+0x20 = units
```

Its validator at `0x11256d` emits five key-`0x05` records:

| input slot | value |
|---|---|
| `0x15` | raw IEEE-754 factor |
| `0x16` | raw IEEE-754 units |
| `0x17` | point-offset enable |
| `0x18` | line-offset enable |
| `0x19` | fill-offset enable |

`captures/bankb-polyoffset-0528.txt` records this input stream directly.  For
`glPolygonOffset(3,7)` with fill offset enabled it contains:

```
key 0x05, count 3
slot 0x15 = 0x40400000       factor = 3.0f
slot 0x16 = 0x40e00000       units  = 7.0f
slot 0x19 = 1                fill enable
```

This capture established the logical slots, but did not establish physical
register addresses.  The unstripped vendor kernel object supplies the missing
translation.

## 2. Linux translator, end to end

Key `0x05` dispatches through `mtx1734`.  Slots `0x15` and `0x16` become raw
float fields `HWSTATE+0x1e48` and `HWSTATE+0x1e4c`.  Slots `0x17..0x19` become
bits 3, 4 and 5 of `HWSTATE+0x1be1`.  All five mark raster dirty group
`HWSTATE+0x1bef` bit 0.  The enable values never enter a hardware-register
payload.

Raster validator `mtx740` (`mtx_parhelia.o` `.text+0x864f0`) activates offset
only when the enable matching the current polygon mode is set:

```
(fill_enable  && polygon_mode == 3) ||
(line_enable  && polygon_mode == 2) ||
(point_enable && polygon_mode == 1)
```

The exact arithmetic at `0x865e6..0x8662a` is:

```
factor_hw = factor
units_hw  = units * depth_surface->unit_scale

if (!(raster_flags & 1)) {
        factor_hw *= depth_direction_sign;
        units_hw  *= (float)configuration_scale * depth_direction_sign;
}
```

`depth_direction_sign` is selected by depth format from the confirmed table
`{0,0,-1,+1,-1,+1,+1,+1,0}`.  `configuration_scale` is read from a driver
configuration object; treating its normal value as 1 is inferred, while the
captured resulting value is hardware evidence.

The depth-surface scale is derived earlier by `mtx1040` at
`0x12292..0x122cc`:

```
unit_scale = 1.0f / (float)(format->depth_mask >> format->depth_shift)
```

This gives `0x33800001` for a 24-bit depth mask.  Therefore the ICD records are
raw API values, while the actual GPU constant is scaled.  Describing the
factor/units as unscaled hardware register values confuses the two stages.

At `0x8662a..0x8668e`, `mtx740` writes `factor_hw` and `units_hw` to dwords 0
and 1 of block `0x178`, marks that block dirty, and later `mtx1342` emits it.
Dwords 2 and 3 remain the initialized raster constants `0x3f000000` and
`0x49400000`.

## 3. Exact GPU packet

The generic block-load framing from `mtx1342` is:

```
header = 0x41000000
       | ((count << 17) & 0x007e0000)
       | ((count * 4 + 3) & 0xffff);

header, count, first_slot, 1, 0, count * 4 payload dwords
```

For one `c[0x178]` vector this is:

```
0x41020007, 0x00000001, 0x00000178, 0x00000001, 0x00000000,
factor_hw, units_hw, 0x3f000000, 0x49400000
```

As with every constant upload, the standard constant-loader destination
preamble and loader program bind must precede the block-load run.  Its
temporary `0x1000 = 0` selects the loader's local stream class.  It is not a
polygon-offset register.

The decisive live packet is in
`captures/gl-matrix/polyoff-secs.txt:17305`.  It carries:

```
41020007 00000001 00000178 00000001 00000000
3f800000 33800001 3f000000 49400000
```

The same draw subsequently binds and arms the per-vertex pipeline with
`0x10f0 = 0x20000086` and `0x00000086`, then submits header `0xd7080001`.
The baseline triangle has low mode/class `2` and header `0x97080001`.  Thus
mode bit 7, the low mode change `2 -> 6`, and draw-header bit 30 are part of
the vendor's active fill-offset command path.  They must not be discarded as
noise when reproducing that path.

The capture's `0x1000 = 2` belongs to the ordinary draw's stream-0 memory
class: its vertices happened to reside in class-2 memory.  It is unrelated to
polygon offset.  Replaying it with a class-0 vertex address caused the earlier
engine stall.  An open driver must preserve the actual vertex buffer address
and memory class.

## 4. Windows cross-check

`MTXPARD.dll` independently advertises both `DEPTHBIAS` and
`SLOPESCALEDEPTHBIAS` in its raster capabilities.  Its render-state handlers
at `0xadd55` and `0xadd8a` store the supplied dwords in `ctx+0x2104` and
`ctx+0x2100`; the legacy integer `ZBIAS` handler at `0xadcec` derives both a
constant bias and a slope-scale bias using a factor of `0.25f`.  This confirms
that Parhelia has separate constant and slope terms and that the vendor does
not CPU-adjust every submitted vertex.

The Windows D3D state compiler is not an address oracle for the Linux GL
constant layout.  It selects different fixed-function program fragments and
reuses nearby constant components for D3D state.  The Linux `mtx740` gate plus
the live GL packet are the controlling evidence for the GL driver.

## 5. Implementation boundary

The evidence supports the following implementation for the captured triangle
fill path:

1. Select offset per draw from the fill/line/point enable that matches the
   current polygon raster mode.
2. Scale `units` by the depth-buffer quantum.  For S8Z24 this is the float
   `1.0f / 16777215.0f`, bits `0x33800001`.
3. Upload the complete `c[0x178]` vector through opcode `0x41`.
4. Use the captured active mode `0x86` and set draw-header bit 30 while offset
   is active; restore the primitive's ordinary mode and clear bit 30 when it
   is inactive.
5. Leave stream base/class `0x1000` determined solely by the vertex buffer.

The following points remain unproven and should be isolated in hardware tests
before broad exposure:

* The existing from-scratch test rendered the same triangle coverage after
  uploading `c[0x178]`, but did not draw equal-depth overlapping polygons.  A
  depth-enabled two-pass probe must confirm the sign and magnitude for positive
  and negative factor/units.
* Only `GL_POLYGON_OFFSET_FILL` has a complete outgoing board capture.  The
  Linux binary proves the line/point enable selection and constant arithmetic,
  but the exact final mode/header combination for native lines and points has
  not been captured.
* The meaning of mode `0x86` below the observable facts (bit 7 set, low nibble
  6) is not named by either binary.  Use the exact captured value rather than
  assigning speculative bit names.
* The block constant file is internal state addressed by opcode `0x41`.  No
  physical MMIO mapping for block `0x178` is proven, and probing one is
  unnecessary for the DMA command path.

