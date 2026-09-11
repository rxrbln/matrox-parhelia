# Gallium masked and rectangular clear design

This note specifies the GPU path for Gallium colour, depth, and stencil clears
in the current Parhelia Mesa/DRM stack.  It is an implementation recipe, not a
claim that the code already has these semantics.  At present
`parhelia_context.c:ph_clear()` discards `color_clear_mask`,
`stencil_clear_mask`, and `scissor_state`, and any depth or stencil request
overwrites the complete S8Z24 word over the complete framebuffer.

The required operation is the already working 2D solid fill with a bit-plane
write mask.  It needs no CPU readback or per-resource depth/stencil shadow.

## Evidence and chosen engine path

There are three independent witnesses for the operation.

* The open `reference/p3d_clear.c:p3d_emit_clear()` emits a type-2 surface
  descriptor, `DWGCTL=0xc0`, `FCOL`, `DSTXY`, and `LEN`.  It has been checked on
  P690 for both format `0x32` colour and format `0x36` Z24S8.  In particular, a
  depth surface cleared to raw `0x00ffffff` subsequently passes a LESS draw and
  reads back as that value outside the triangle.
* The vendor XAA `SetupForSolidFill` path in
  `matroxdriver-x86_64-cvs-20090511/xserver/7_4_99/mtx_drv.so`, at
  `.text+0x536d0`, stores the bpp-expanded foreground at shadow offset `+0x2c`
  and the bpp-expanded planemask at `+0x24`.  The recovered dirty map identifies
  those as `FCOL 0x1314` and `PLNWT 0x1324`.  Its rectangle call at
  `.text+0x539b0` writes `DSTXY` and fires with `LEN`.  Thus `PLNWT` is a real
  read-modify-write bit-plane mask for an opcode-0 fill, not merely 3D colour
  state.
* Vendor `DeviceBitmap::ClearSurface`, in that binary at `.text+0x7c8e0`, emits
  tag `0xc5c08f8e`, slot-3 base/cfg, `DWGCTL=0x00000cc0`, `FCOL=0`, followed by
  tag `0x2121e3c2`, `DSTXY=0`, and `LEN=(height<<16)|width`.  `0xcc0` and the open
  driver's `0xc0` differ only in destination slot `[11:10]`.

The proprietary GL depth clear also has an opcode-4 WARP rectangle path.  That
path forcibly writes `PLNWT=0xffffffff` and therefore cannot implement
depth-only, stencil-only, or partial-stencil clears without another unknown
masking mechanism.  It also carries vendor fast-clear bookkeeping which is not
present in the open Mesa/DRM streams.  The current open driver has already
proved actual byte writes through opcode 0, so the opcode-0 route is the exact
fit for Gallium masks.

One correction is required in the existing open packet.  `p3d_emit_clear()`
writes `0xffffffff` to `0x12c0`, although the confirmed plane mask is `0x1324`.
Full clears happen to inherit an all-ones `PLNWT` from engine/state setup.  A
masked clear must explicitly write `0x1324` in its own packet.  Retain the
existing `0x12c0` write until its independently proven clear-path role is
understood; add `0x1324` rather than substituting it.

## Exact fill packet

Extend the reference clear emitter with a `uint32_t write_mask` argument.  In
this order, its single opcode-zero register-list packet is exactly 15 dwords:

```
dword  value/meaning
  0    0x0000000d                 header: packet dwords - 2
  1    0x8b8a8988                 indices 88,89,8a,8b
  2    0x00000000                 0x1220 slot-0 base; GEM VRAM relocation
  3    surface_cfg                0x1224 slot-0 cfg
  4    0x00000000                 0x1228 slot-1 base; same GEM relocation
  5    surface_cfg                0x122c slot-1 cfg
  6    0xc5c0c9b0                 indices b0,c9,c0,c5
  7    0xffffffff                 0x12c0, retained proven clear setup
  8    write_mask                 0x1324 PLNWT
  9    0x000000c0                 0x1300 DWGCTL: opcode 0, GXcopy, dst slot 0
 10    value                      0x1314 FCOL, raw destination-format word
 11    0x21e3c2c1                 indices c1,c2,e3, terminator
 12    0x00000000                 0x1304 SRCXY, unused by solid fill
 13    (y << 16) | x              0x1308 DSTXY
 14    (height << 16) | width     0x138c LEN; this write fires the fill
```

`surface_cfg = (pitch_pixels & 0x3fff) | (2 << 16) |
(format << 24)`.  The production formats are `0x32` for the B8G8R8A8/X8 colour
target and `0x36` for native Z24S8.  Coordinates and extents occupy 16-bit
halves.  The current allocation limits (1024 by 768 with a 14-bit pitch) are
well inside those fields.

Both base operands are zero in userspace and receive
`PARHELIA_RELOC_VRAM` relocations for the same handle at packet dwords 2 and 4.
The kernel opcode-zero validator already admits indices `0xb0`, `0xc9`, `0xc0`,
`0xc5`, `0xc1`, `0xc2`, and the special high index `0xe3`; no DRM ABI or kernel
validator change is needed.  `0xc9` is also on the kernel state-shadow safe
list, but the packet has surface relocations and therefore cannot be elided.

Use the rectangle geometry itself for a scissored clear.  Gallium gives a
half-open box `[minx,maxx) x [miny,maxy)`; after intersecting it with the
framebuffer, emit `DSTXY=(miny<<16)|minx` and
`LEN=((maxy-miny)<<16)|(maxx-minx)`.  Do not set DWGCTL bit 23 and do not emit
`0x1328/0x132c`.  The fill primitive is already rectangular, and this avoids
converting Gallium's exclusive maxima to the 2D scissor's inclusive maxima.
It also makes the packet independent of stale scissor state.  Skip an empty
intersection.

The batch entry point should take allocation bounds separately from the
rectangle, for example:

```
parhelia_batch_clear_rect_surface(batch, handle,
    surface_width, surface_alloc_height, pitch, format,
    value, write_mask, x, y, width, height)
```

Validate nonzero handle/extent/mask, `surface_width <= pitch`, 64-pixel pitch
alignment, format `0x32` or `0x36`, and the overflow-safe relations
`x <= surface_width`, `width <= surface_width-x`, `y <= surface_alloc_height`,
and `height <= surface_alloc_height-y`.  The existing full-clear entry point
can remain as an all-ones-mask wrapper.  Do not require a rectangular clear's
height to be a multiple of 32; only the backing allocation height has that
constraint.  The engine handles rectangles that begin or end inside a tile.

## Mask construction

`PLNWT` selects physical bits in the 32-bit destination word.  The following
masks apply to the current little-endian render formats.

For B8G8R8A8/B8G8R8X8, Gallium's four-bit mask for colour buffer zero maps as:

```
PIPE_MASK_R -> 0x00ff0000
PIPE_MASK_G -> 0x0000ff00
PIPE_MASK_B -> 0x000000ff
PIPE_MASK_A -> 0xff000000
```

`color_clear_mask` contains four bits per render target.  The current hardware
path has only colour buffer zero, so use `color_clear_mask & PIPE_MASK_RGBA` and
ignore the higher groups only when their corresponding `PIPE_CLEAR_COLORn` is
absent.  A requested colour clear with a zero effective mask is a no-op.  Pack
the value with `util_pack_color()` exactly as today.

For the native `PIPE_FORMAT_Z24_UNORM_S8_UINT`/format-`0x36` word, depth is
bits `[23:0]` and stencil is bits `[31:24]`.  Build one combined fill mask:

```
uint32_t mask = 0;
if (buffers & PIPE_CLEAR_DEPTH)
        mask |= 0x00ffffff;
if (buffers & PIPE_CLEAR_STENCIL)
        mask |= (uint32_t)stencil_clear_mask << 24;
```

Pack `value` with `util_pack_z_stencil()`.  `PLNWT` makes unused fields in that
packed value harmless.  Examples, all in one GPU fill:

* depth only: `write_mask=0x00ffffff`; the existing stencil byte survives;
* full stencil only: `write_mask=0xff000000`; depth survives;
* stencil write mask `0x5a`: `write_mask=0x5a000000`; even the six unselected
  stencil bits survive;
* depth plus stencil mask `0x5a`: `write_mask=0x5affffff`.

This is the same destination-side bit merge as
`new = (old & ~write_mask) | (value & write_mask)`, performed by the GPU raster
engine at each covered pixel.  No CPU copy of the old S8Z24 contents is needed.

Although `ph_format_supported()` currently advertises
`PIPE_FORMAT_S8_UINT_Z24_UNORM`, native draws accept only
`PIPE_FORMAT_Z24_UNORM_S8_UINT` and format `0x36` has been proved with depth in
the low 24 bits.  Do not silently use the reversed masks (`0xffffff00` and
`0x000000ff`) with `0x36`: that would preserve bytes correctly but make the
depth unit interpret them incorrectly.  Reject the reversed format for native
depth/stencil binding until a matching surface descriptor is recovered.

## Gallium integration and resource ownership

`ph_clear()` should continue to call `ph_submit_vertices()` before emitting any
clear.  Compute one clamped rectangle from its optional `scissor_state` and use
it for both attachments.  Test `PIPE_CLEAR_DEPTH` and `PIPE_CLEAR_STENCIL`
separately; the current `buffers & PIPE_CLEAR_DEPTHSTENCIL` test followed by a
full-word fill is incorrect.

Factor the context helper around `(x,y,width,height,value,write_mask)` and use it
from three callbacks:

* `clear`: masked colour and masked depth/stencil over the optional scissor;
* `clear_render_target`: all-ones mask over its explicit rectangle;
* `clear_depth_stencil`: depth mask and/or full `0xff` stencil mask over its
  explicit rectangle.

Bind the latter two callbacks only for the same level-zero, layer-zero native
surfaces accepted by the draw path.  This stack has no conditional-render
state, so `render_condition_enabled` cannot name an active hardware condition.
Mipmap/layer clears need base-delta relocations and are outside this packet.

A full all-plane clear over the complete logical surface may retain the
current optimization of extending height to `ALIGN_POT(height,32)` and can
discard an older linear representation: every visible bit is replaced.  A
rectangular or masked clear preserves some existing tiled contents.  If
`r->linear_dirty` says an imported/linear representation is authoritative,
first call the existing `ph_resource_upload_tiled(r)` and require it to clear
that flag before submitting the fill.  This is an ownership transfer, not a
read-modify-write shadow.  Normal masked clears then read and merge only in
VRAM.  Never clear `linear_dirty` without that upload unless the operation is a
complete all-plane overwrite.

After a successful submission, the existing bookkeeping remains appropriate:
set `tiled_dirty=true`, `linear_dirty=false`, set mip-defined bit zero, and
increment `content_generation`.  Submit colour and Z/S packets through the
existing ordered async queue.  Each clear packet overwrites surface slots 0 and
1, but subsequent hardware draws emit complete target state, as they already
must after the current full clear.

`PARHELIA_CPU_CLEAR` is a diagnostic path rather than the production
implementation.  To keep it semantically useful, update its loops to cover
only the clamped rectangle and perform the explicit masked merge shown above.
It must still wait for async work before touching the mapping.

## Validation plan

Add an offline batch test first.  For a 256-pixel pitch, format `0x32`, value
`0xa1b2c3d4`, mask `0x00ff00ff`, and rectangle `(7,11) 193x101`, assert the
complete 15-dword packet above, including:

```
surface_cfg = 0x32020100
DSTXY       = 0x000b0007
LEN         = 0x006500c1
```

Assert two same-handle VRAM relocations at dwords 2 and 4.  Add negative cases
for zero mask/extent, arithmetic overflow, a rectangle beyond the logical
allocation, nonaligned pitch, and unsupported format.  Also assert that a
height of 1 is accepted for a rectangle while the allocation remains 32-row
aligned.

Then add a P690 hardware mode to `parhelia_drm_test` which uses no CPU
read-modify-write:

1. GPU-clear a 256x256 format-`0x32` BO to `0x11223344`.  Mask-clear a rectangle
   crossing both `x=63/64` and `y=31/32` tile boundaries with value
   `0xa1b2c3d4` and mask `0x00ff00ff`.  GPU-blit to the linear readback BO,
   fence, and verify `0x11b233d4` at every inside sample and `0x11223344` just
   outside all four edges.  Include a 1x1 rectangle and an unaligned width.
2. GPU-clear format `0x36` to raw `0x96123456`.  A depth-only rectangular clear
   to raw `0x3cc0ffee` must produce `0x96c0ffee`.  A stencil-only clear with
   mask `0x5a` must produce `0x9c123456`.  A combined depth plus masked-stencil
   clear must produce `0x9cc0ffee`.  A stencil mask of zero must emit no packet
   and preserve `0x96123456`.
3. Repeat step 2 with a depth-tested draw after the clear, then use stencil
   EQUAL against the merged value.  This verifies that raw readback and both
   fragment units agree on the word layout.
4. Queue draw, masked clear, and draw without intermediate CPU waits, then wait
   only for the terminal fence.  Verify the regions and run with kernel state
   shadow caching both disabled and enabled.  This checks submit ordering and
   proves that the relocated clear packet is never incorrectly elided.
5. Exercise an imported resource beginning with `linear_dirty=true`; verify
   that the pre-clear upload preserves pixels and bit planes outside the clear.
   Repeat a complete all-plane clear and verify that it safely skips the
   upload.

Capture `PARHELIA_TRACE` plus the kernel packet trace for the first successful
run.  The decisive trace is one 15-dword register list with `0x1324` preceding
the firing `0x138c`, followed by GPU readback values matching the equations
above.  No depth compression or hierarchical-clear state exists in the open
stream, so there is no metadata packet to invalidate; the functional depth and
stencil draw in step 3 is the hardware check for that assumption.
