# M9 color/depth surfaces, clear, viewport, and scissor

This note closes the static frontend-to-packet joins for color render targets,
the depth attachment, and viewport/scissor state in the saved M-Series Windows
driver.  It also identifies the Direct3D clear ABI, its dispatch chain, and the
three concrete clear implementations selected at the bottom of that chain.

Confidence labels match `parhelia-m9-3d-static-re.md`: **[BIN]** is directly
visible in the binary, **[DER]** is deterministic data-flow derived from those
instructions, and **[INF]** still needs a differential capture.  Addresses are
section-relative virtual addresses in `MSerX/MxMD.dll`; an indexed state write
uses MMIO offset `0x1000 + index * 4`.

## Color render-target join

The draw lowerer at `0x181fa0` receives a state object whose `+0x24` member
selects the batch surface shadow (`object + 0x3e8`) and whose `+0x20` member is
the nested compiled resource state [BIN].  The fragment-output attachment list
has a byte count at `[object+0x28]+0x361` and four-byte declarations beginning
at `+0x362`.  For every declaration [BIN]:

* bits `15:0` select a compiled resource entry;
* `((declaration >> 16) & 0x3f) - 0x10`, divided by four, selects color slot
  `0..3`;
* declaration value `0xfffe` is the unbound sentinel.

The copy at `0x182096..0x1820f3` is exact [BIN]:

| compiled resource field | surface-shadow destination | final index | role |
|---:|---:|---:|---|
| `+0x1bc + 8*r` | `+0x38 + 8*s` | `b0,b2,b4,b6` for `s=0..3` | tagged address word |
| `+0x1c0 + 8*r` | `+0x3c + 8*s` | relocation paired with the preceding index | relocation backing |
| `+0x1fc + 4*r` | `+0x58 + 4*s` | `b1,b3,b5,b7` for `s=0..3` | surface descriptor |

Here `r` is the declaration's resource index and `s` is its output slot.  The
address copy preserves the high three tag bits and replaces the low 29 bits;
the exact XOR-mask sequence is at `0x1820a1..0x1820d9`.  A private override at
`0x1820f6..0x18211c` can replace address bits `28:16` while retaining those
high tag bits.  The lowerer grows the live attachment count at surface shadow
`+0x34` to at least `s+1`, sets dirty bit `1 << s`, and emitter `0xfb920`
relocates each address using selector `s+2` before issuing the pair [BIN].

The final hardware map is therefore [BIN][DER]:

| slot | indices | MMIO offsets |
|---:|---|---|
| 0 | `b0,b1` | `0x12c0,0x12c4` |
| 1 | `b2,b3` | `0x12c8,0x12cc` |
| 2 | `b4,b5` | `0x12d0,0x12d4` |
| 3 | `b6,b7` | `0x12d8,0x12dc` |

The descriptor's seven-bit format code is exactly bits `31:25`.  At
`0x182120..0x18215c`, the lowerer computes `(descriptor >> 25) - 0x30`, bounds
it by `0x4a`, and feeds that value plus descriptor bits `20:19` to `0x181830`.
The helper's three-bit result is packed at `3*s` into per-batch common-state
shadow `+0x14`, emitted by `0xf4f50` as final index `89` (MMIO `0x1224`).  An
out-of-range format produces value seven [BIN].  This independently agrees
with the format-code family recovered for texture descriptor index `78`,
including `A8R8G8B8/X8R8G8B8/R8G8B8=0x66`, `R5G6B5=0x4e`,
`A2B10G10R10=0x68`, and `A2R10G10B10=0x6a` [DER].

The common resource-view builder `0x467e0` closes the rest of the render
descriptor.  It caches 12-byte triples at
`layout + 0x08 + 12*selector`; each triple is address, descriptor word 0, and
descriptor word 1.  Helper `0x4d180` returns descriptor word 0 to the surface
binder.  The GPU clear path requests selector 8 at `0xb50f5`, and alternate
draw lowerer `0x182b40` requests the same view at `0x182bb3`, so the dword
emitted at `b1/b3/b5/b7` is the following selector-8 word [BIN][DER]:

| bits | ordinary source | meaning established by construction |
|---:|---:|---|
| `15:0` | layout `+0x184[15:0]` | aligned horizontal row span in format elements |
| `18:16` | layout `+0x138[2:0]` | allocation-layout selector |
| `20:19` | zero | selector 8 never copies storage class `+0x13c` here |
| `21` | layout flags dword 0 bit 3 | layout/address-offset mode |
| `22` | zero ordinarily; one in the special view below | blocked/special-view marker |
| `23` | layout `+0x14c` bit 2 | resource-layout flag |
| `24` | zero | not populated for selector 8 |
| `31:25` | helper-adjusted layout `+0x140[6:0]` | hardware format code |

The unused second descriptor word is still built exactly: bits `15:0` are raw
vertical extent `+0x12c`, and bits `27:16` are `+0x130[11:0]` (or `+0x134`
for formats `0x3f/0x7f`).  Storage class `+0x13c == 1` replaces those high
extent bits with one [BIN].

Selector 8 has one exact blocked-layout override.  When storage class
`+0x13c == 2`, `0x46bea..0x46c3a` sets descriptor bit 22, puts
`+0x150[3:0]` in address bits `5:2`, copies `+0x184` bit 5 to address bit 6,
and either adds the aligned offset returned by `0x46e40` when layout flags
dword 0 bit 3 is set, or replaces descriptor bits `15:0` with descriptor-word-1
bits `27:16`.  Layout flag `+0x14c bit 6` clears descriptor bits `18:16` for
selector 8.  These are the complete pitch and layout transformations visible
at the packet boundary [BIN][DER].  The vendor binaries do not name the
physical in-block address permutation, so the numeric classes should remain
numeric until a coordinate-coded capture supplies that last semantic label.

## Depth/stencil attachment join

The lowerer tests `[object+0x28]+0x372` for a depth/stencil attachment at
`0x1821f4..0x182201`.  When present, it installs two relocation-backed
descriptor pairs [BIN]:

| nested compiled fields | surface-shadow fields | final indices | MMIO offsets | dirty bit / relocation selector |
|---|---|---|---|---|
| `+0x21c,+0x220,+0x224` | `+0x68,+0x6c,+0x70` | address/descriptor `b8,b9` | `0x12e0,0x12e4` | `0x10` / `6` |
| `+0x228,+0x22c,+0x230` | `+0x74,+0x78,+0x7c` | address/descriptor `ba,bb` | `0x12e8,0x12ec` | `0x20` / `7` |

The address fields use the same low-29-bit address/high-three-bit tag
preservation as the color targets.  The adjacent words `+0x220` and `+0x22c`
are relocation backing, while `+0x224` and `+0x230` are emitted verbatim as
descriptors.  The first install is at `0x182201..0x182291`; the repeated update
path is at `0x182549..0x182620` [BIN].  If a depth attachment is declared but
its compiled binding must be reset, the code substitutes the global resource
pair at `0x20d51c/0x20d520` while retaining the compiled descriptors [BIN].

The fixed depth/stencil controls are separate.  `0x181900` derives final
`a8,a9,aa`; their complete bit layout is documented in
`parhelia-m9-3d-static-re.md`.  Depth surface format codes occupy the same
seven-bit family used by the color and texture paths: the recovered table
includes `D16=0x5c`, `D15S1=0x5d`, `D32=0x7c`, and the D24 family at `0x7d`
[BIN][DER].  The two descriptors above remain distinct even when both refer to
one API depth/stencil attachment.

The identities are no longer ambiguous.  Default compiled-state constructor
`0x12e75a..0x12e7c9` initializes the first descriptor to `0xb8010001`; its
format field is `0x5c`, the independently joined `D16` hardware code.  It
initializes the second pair to the null resource and descriptor `0xf0010100`,
whose format field is `0x78`, the independently joined `G16R16` code rather
than a depth/stencil format [BIN][DER].  Alternate lowerer `0x182b40` provides
the dynamic join: it asks the bound resource for selector 9, installs that
address and descriptor at `+0x68/+0x70`, rewrites the format field to `0x5c`
or `0x75`, and dirties only `b8/b9` at `0x182eaa..0x182f76` [BIN].

Thus `b8/b9` is the ordinary Z/depth surface.  `ba/bb` is the optional
`HZFlags`-controlled hierarchical-Z metadata surface, not a separately
addressable API stencil plane [BIN][DER].  Allocator `0x54be0` creates its
format-`0x0b` storage at `ceil(width/8) x ceil(height/4)`, and binder `0xaaa7d`
joins selector 12 to the `+0x74/+0x78/+0x7c` shadow fields.  Stencil remains
packed into the depth format and is selected by
the `a8/a9/aa` controls.  Clear helper `0xb5630` corroborates the distinction:
for the flagged depth layout it obtains cached selector 9 at
`layout+0x74/+0x78/+0x7c`, temporarily rebinds its address and descriptor-word
0 as color slot 0, and issues the depth clear as a normal rectangle draw.  The
second `ba/bb` pair is not substituted for the Z surface on that path [BIN].

The selector-9 descriptor uses the same table above.  Its blocked-layout
override is selected by layout flags dword 0 bit 2 rather than storage class 2,
and it does not apply selector 8's `+0x14c bit 6` clearing rule.  This resolves
the descriptor bit placement for the ordinary depth view as well as the color
view [BIN][DER].

## Viewport and scissor through final packets

Compiler `0x138c10` reads two rectangles [BIN]:

| rectangle | validator inputs | construction |
|---|---|---|
| viewport | `+0x108,+0x10c,+0x110,+0x114` | `x,y,x+width,y+height` |
| scissor | `+0x124,+0x128,+0x12c,+0x130` | `x,y,x+width,y+height` |

Helper `0x138cd0` tests validator `+0x8c bit 2`, already joined to
`SCISSORTESTENABLE`.  When enabled, helper `0xf1790` intersects the scissor and
viewport rectangles; when disabled, the viewport passes through.  An optional
subsurface/origin mode adds `+0x118/+0x11c`.  The helper then clamps negative
coordinates to zero and clips against the width/height reported by each of up
to eight attachments referenced from validator `+0x1cc` [BIN].

The result is packed as unsigned 16-bit coordinates with exclusive right and
bottom endpoints [BIN][DER]:

| validator output | nested offset | surface-shadow offset | final index / MMIO | value |
|---:|---:|---:|---|---|
| `+0x410` | `+0x258` | `+0xa4` | `f5 / 0x13d4` | `left | top << 16` |
| `+0x414` | `+0x25c` | `+0xa8` | `f6 / 0x13d8` | `right | bottom << 16` |
| `+0x418` | `+0x260` | `+0xac` | `c4 / 0x1310` | internal transformed left/top pair |
| `+0x41c` | `+0x264` | `+0xb0` | `c5 / 0x1314` | internal transformed right/bottom pair |

`0x138cb6` sets nested dirty bit `0x04000000`.  Lowerer `0x181fa0` tests it at
`0x18283e`, copies the four words at `0x182846..0x18286a`, and sets surface
dirty bit `0x400` at `0x182870`.  Emitter `0xfb920` writes header
`c5c4f6f5` and those four values [BIN].  In the ordinary zero-origin path,
`c4/c5` duplicate `f5/f6`; their different transformed values in the private
origin mode should be preserved until that mode is identified.

There is no minus-one adjustment: `0x138c10` adds width and height directly.
Consequently a Mesa scissor `(x,y,w,h)` must send `(x,y,x+w,y+h)`, after the
API-to-framebuffer Y conversion appropriate to the current framebuffer.

## Clear dispatch and payload

The DP2 dispatch table is initialized at `0xdd8e0`; table entry 42 at
`0x20ca48` selects `0xdf9d0`, the `D3DDP2OP_CLEAR` parser [BIN].  Its payload
contract is [BIN]:

| byte offset | type | meaning |
|---:|---|---|
| `+0x00` | `u32` | clear flags, low four bits consumed |
| `+0x04` | `u32` | packed color |
| `+0x08` | `float` | depth |
| `+0x0c` | `u32` | stencil |
| `+0x10` | array | 16-byte clear rectangles |

The rectangle count is at parser record `+0x14`, and the parser advances by
`0x10 + count*0x10`.  It calls vtable slot `+0x04` on `[parser+0x24]` [BIN].
The draw/context vtable at `0x1f4abc` joins that slot to clear routine
`0x13de20`; the neighboring entries are `0x13ca20`, draw `0x13e790`, and
indexed draw `0x13e7e0` [BIN].

`0x13de20` dispatches flags exactly as follows [BIN]:

| flag | D3D meaning | action |
|---:|---|---|
| `0x1` | `D3DCLEAR_TARGET` | call color clear `0x13e0f0` |
| `0x2` | `D3DCLEAR_ZBUFFER` | participate in depth/stencil clear `0x13e4c0` |
| `0x4` | `D3DCLEAR_STENCIL` | participate in depth/stencil clear `0x13e4c0` |
| `0x8` | private rectangle preprocessing | call `0x13def0` |

Color clear `0x13e0f0` walks up to eight color attachments from the surface set
at `[arg+8]+0xf8` and `+0x18+4*slot`.  It clamps each rectangle with attachment
width/height methods `+0x24/+0x28`, obtains a color mask from `+0x130/+0x100`
or defaults to `0xf`, and calls attachment method `+0x8c` at
`0x13e436..0x13e470` with the rectangle and converted color metadata [BIN].
Attachment kinds returned by method `+0x3c` select conversion method `+0x7c`
for kind 2, `+0x88` for kind 3, and `+0x80` for kind 5.  The first render
target may apply sRGB conversion when surface-set `+0xf4 bit 0x800` is set
[BIN].

Depth/stencil clear `0x13e4c0` takes the attachment at surface-set `+0x3c`,
uses method `+0x58` to form format-specific depth/stencil metadata from flags,
depth, and stencil, then calls attachment method `+0x64` for every clipped
rectangle at `0x13e73e..0x13e74a` [BIN].

These are resource virtual calls, not raw indexed command writes.  The common
resource constructor `0xa32a0` installs interface vtable `0x1f3ba0` at resource
subobject `+0x18`; that is the exact subobject passed to the render-target
binder at `0x2d6d0..0x2d721` [BIN].  The vtable therefore closes the next clear
dispatch level [BIN][DER]:

| attachment vtable slot | concrete function | observed action |
|---:|---:|---|
| `+0x64` | `0xa6c10` | construct depth/stencil clear state and call `0xb4fe0` |
| `+0x8c` | `0xa7b70` | color clear/lazy-clear selection, falling through to `0xa7ee0` |

Color method `0xa7b70` recognizes a whole-surface rectangle when width times
height exceeds `0x280000` pixels.  That path flushes incompatible prior state,
then caches the four converted clear words at subobject `+0x318..+0x324`, the
rectangle at `+0x328..+0x334`, the color mask at `+0x338`, and sets pending bit
`+0x34c bit 0` [BIN].  Other clears flush pending state through `0xa7ce0` and
call `0xa7ee0`.  `0xa7ee0` routes ordinary matching-size clears through
`0xa48b0`; a size-mismatch branch constructs a conversion operation through
`0xb38d0`, `0xb4690`, and `0xab620` [BIN].

This vtable join proves that the vendor batches some full color clears lazily.
The leaves can now be flattened.  Color leaf `0xa48b0` builds one common
operation record: resource at `+0x04`, exclusive rectangle at `+0x08..+0x14`,
mode flags at `+0x18`, component masks at `+0x1c..+0x28`, and four converted
clear words at `+0x3c..+0x48`.  Depth/stencil leaf `0xb4fe0` receives the same
record already constructed by `0xa6c10` [BIN].  Both then use exactly this
ordered executor [BIN]:

```text
if (resource+0x3d0 != 0 && b5e70(op)) return success;
if (b5030(op))                         return success;
return b5a80(op);
```

### Whole-allocation fast clear at `0xb5e70`

The first path is a direct command-stream clear, not a draw.  It accepts only
a rectangle beginning at `(0,0)` and covering at least resource dimensions
`+0x240/+0x244`, mode `op+0x18 & 3 == 0`, page-aligned allocation address
`resource+0x198`, and element widths `0x10`, `0x20`, `0x40`, or `0x80` bits at
`resource+0x1a0`.  A 64-bit element requires equal clear words 0 and 1; a
128-bit element requires all four words equal.  The device capability call at
`0xb5e87..0xb5e9f` must also leave bit 1 clear [BIN].

After normal command-buffer entry and state preparation, it emits these exact
indexed records [BIN]:

```text
header e6,ac,45,45       e6 = 00000002
                        ac = clear_word

header 2a,2b,e6,45       2a = 454545e6
                        2b = 0000ffff
                        e6 = 20000000 | ((run_words - 1) & ffff)
                        inline payload = allocation page-run words...
```

For 16-bit elements `clear_word` duplicates the low halfword with multiplier
`0x00010001`; wider accepted elements use `op+0x3c` unchanged.  The
`0x454545e6` value is the inline layout word, as in the independently
identified `0xf9c70` inline-data path.  The page-run payload is derived from
`base_page = resource+0x198 >> 12` and
`page_count = ceil(resource+0x1bc / 4096)`.  It covers the unaligned leading
pages and then groups the rest in runs of eight.  Each payload starts from
`(((base_page & 0xffff8) | 0xd00000) << 5)`; its low byte is the page mask,
and bits above that mask advance by `0x100` per eight-page group.  The tail
mask and the optional `0/1` padding words are selected at
`0xb60c4..0xb61b0` from the device mode [BIN][DER].

The path closes the inline run through command-buffer method `+0x68`.  It then
emits either `e6,e6,45,45` with payloads `0,1`, or `e6,45,45,45` with payload
`1`, tracks the allocation through resource method `+0x1e4`, sets surface
dirty bit `0x200` so indices `ab/ac/ae` are recommitted, and leaves the command
transition through `0x55370(0)` (`0xb61b3..0xb6327`) [BIN].

### Rectangle-draw clear at `0xb5030`

When the direct path is unavailable, `0xb5030` performs an ordinary GPU
rectangle draw.  It requires a supported resource kind, device capability bit
1 clear, a valid layout object, and selector-8 capability flag
`layout+0x126 bit 0`.  It obtains or builds cached selector 8, copies its
address/relocation and descriptor word 0 to color slot 0, writes the replicated
clear value to common-state `+0x3a8`, installs the exclusive rectangle, and
marks the normal raster and surface groups dirty [BIN].  Calls through the
task interface at `0xb54ee` and `0xb54fd` run the standard state emitter and
draw launcher, after which the allocation is tracked and `0x55370(0)` closes
the transition.  There is no clear-specific launch opcode on this path [BIN][DER].

One depth layout takes the specialized branch `0xb5630`.  It aligns the left
edge down and the width up to eight elements, binds selector-9 depth storage as
temporary color slot 0, emits the two rectangle vertex records beginning with
literal headers `0x504e4c48` and `0x5e4f4c48`, and submits through the same
task/draw methods.  The saved Linux 1.15 vendor build independently contains
the same two record literals at file offsets `0x3a88df/0x3a893c`; it also
contains the default depth/auxiliary descriptors `0xb8010001/0xf0010100` in
the same constructor at `0x3bf008/0x3bf09d` [BIN].

### CPU fallback at `0xb5a80`

The final path maps the allocation through `0x2cb40` (or the resource-specific
map helpers), computes row and element offsets from the resource layout, and
updates 8-, 16-, 32-, 64-, or 128-bit elements with the operation's masks and
clear words in nested rectangle loops.  It unmaps through `0x56130`, `0x56430`,
`0x58080`, or `0x2c840` and closes with `0x55370(2)` [BIN].  For source layout
enum 1 / descriptor class 3, constructor flag `+0x2b8` makes this loop operate
on a separate linear staging allocation with byte pitch `+0x2b0`; unlock
submits the staging record for GPU conversion.  The exact row-major formula
and transfer join are in `parhelia-m9-blocked-layout-boundary.md`.  This is a
CPU fallback and emits no pixel-fill packet.  The three paths together are the
complete concrete implementation below both attachment leaves.

## Driver-ready fields and remaining blockers

The following can be implemented from static evidence [BIN][DER]:

* four color attachment packet pairs `b0/b1` through `b6/b7`, their relocation
  selectors, dirty bits, slot declaration mapping, and descriptor format bits;
* ordinary depth packet pair `b8/b9`, hierarchical-Z metadata pair `ba/bb`,
  their source fields, relocation selectors, dirty bits, and identities;
* effective viewport/scissor calculation, exclusive endpoints, dirty-state
  propagation, and final `f5/f6/c4/c5` packet indices;
* clear API parsing, clipping and attachment iteration, plus direct page-run,
  GPU rectangle-draw, and CPU fallback implementations;
* selector-8/9 render descriptor row-span, layout, format and flag fields,
  including the blocked-layout address and descriptor overrides.

The render-surface blockers named above are closed.  The remaining questions
are narrower semantic or uncommon-mode issues:

1. The physical address permutation inside the numeric blocked layout classes.
   It is not needed to encode an already constructed vendor-style descriptor,
   but a new allocator needs a coordinate-coded capture before manufacturing
   that layout independently.
2. The private transformed meaning of `c4/c5` when the nonzero-origin path is
   active.  Ordinary framebuffer viewport/scissor operation does not depend on
   this gap because the zero-origin path duplicates `f5/f6`.
3. Hierarchical-Z metadata bit meaning and dynamic rejection behavior.  Static
   data flow now closes initialization word `0x007ff000`, full-clear seeding,
   whole-plane partial-clear invalidation, dirty refresh after uncoupled
   writes, owner/alias lifetime, joint resource tracking, selector 12, and
   final `ba/bb` binding.  A bounded hardware test must still prove that the
   reproduced metadata never rejects visible fragments before default enablement.
