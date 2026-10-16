# M9 captured 3D state and two-pass draw pipeline

This note turns the September 14, 2026 M9148 vendor captures into an
implementation-oriented state map.  It covers the settled flat triangle in
`captures/m9x-linux-gltri-20260914` and one steady glxgears cycle in
`captures/m9x-linux-glxgears-20260914`.  The envelope, packet framing, and
capture method are described in
[parhelia-m9-linux-vendor-capture.md](parhelia-m9-linux-vendor-capture.md).

The accompanying `tools/m9-capture-state-summary.py` selects the current
ping-pong command buffer, summarizes every submission, reconstructs indexed
store images, and verifies the exact address join between the two draw passes.
Run:

```sh
python3 tools/m9-capture-state-summary.py --self-test
python3 tools/m9-capture-state-summary.py
python3 tools/m9-capture-state-summary.py --triangle-diff
```

The labels below distinguish facts read directly from the capture (**CAP**),
joins to the saved binary analysis (**BIN**), and conclusions from those facts
(**DER**).  Unknown numeric indices are deliberately left unnamed.

## Capture selection and record framing

The vendor alternates two 32 KiB bus-master mappings.  For the gltri fixture,
even ExecuteDma sequences use `bm-NN.bin` and odd sequences use
`bm1-NN.bin`.  The same-name mapping on the opposite parity is the preceding
submission and contains stale state.  The first glxgears tracer saved only the
even/current mapping, so sequences 19, 21, 23, and 25 cannot be decoded as
current submissions.  Sequences 18, 20, 22, and 24 are valid.  **CAP**

An ExecuteDma descriptor selects logical byte interval `[start,end)`, which is
physical bus-master interval `[8+start,8+end)`.  All observed descriptor flags
are zero.  An ordinary outer record is `u32 count` plus `count+1` raw dwords;
one raw block may contain several four-index packets.  Numeric draw triples are
three uncounted dwords.  The extent is eight plus the last descriptor's end.
**CAP**

The most useful submissions are:

| fixture/sequence | extent | ranges | counted records | packets | raw draws | role |
|---|---:|---:|---:|---:|---:|---|
| gltri 0 | `0xfa0` | 3 | 220 | 226 | 0 | startup state and invariant store image |
| gltri 8 | `0x874` | 2 | 113 | 114 | 0 | invariant store image |
| gltri 10 | `0x8cc` | 2 | 117 | 118 | 0 | invariant store image |
| gltri 12 | `0x4f0` | 3 | 75 | 78 | 2 | four-element preprocess, three-element raster, completion |
| gltri 14 | `0x8cc` | 2 | 117 | 118 | 0 | texture rectangle/present and invariant store image |
| gears 18 | `0x1010` | 3 | 223 | 239 | 10 | startup frame: five preprocess and five raster draws |
| gears 20/22 | `0x5e0` | 3 | 79 each | 94 each | 10 each | steady frames: five preprocess and five raster draws |
| gears 24 | `0x8cc` | 2 | 117 | 118 | 0 | texture rectangle/present and invariant store image |

## A draw is a preprocessing pass followed by a raster pass

Gltri sequence 12 has three ExecuteDma descriptors.  Range 0 is logical
`[0,0x268)`, physical `[8,0x270)`, and contains 38 decoded packets followed by
the raw numeric draw `[0,4)` at physical offset `0x264`.  Range 1 is logical
`[0x268,0x4bc)`, physical `[0x270,0x4c4)`, and contains 37 decoded packets plus
the application's `[0,3)` draw at `0x4b8`.  Range 2 is the 3-packet completion
footer.  **CAP**

The first pass writes four resources and the second pass consumes the exact
same addresses:

| preprocess output index | address | raster input index | address |
|---:|---:|---:|---:|
| `b0` | `0x113c1000` | `00` | `0x113c1000` |
| `b4` | `0x113ee000` | `01` | `0x113ee000` |
| `b8` | `0x11404800` | `02` | `0x11404800` |
| `b2` | `0x113d7800` | `03` | `0x113d7800` |

Thus `[b0,b4,b8,b2] == [00,01,02,03]` byte for byte.  The internal four-
element pass generates three color-like streams plus a depth-like stream, and
the raster pass reads those four generated streams.  The exact varying type,
component packing, and stride fields behind indices `00..03` still need
controlled vertex-declaration captures.  Their producer/consumer role does
not depend on naming those fields.  **CAP/DER**

The new Mesa backend must implement or faithfully replay this preprocessing
stage before a raster draw.  Omitting it leaves the second pass reading
uninitialized resources.  This explains why the four-element draw cannot be
dropped as startup noise.  **DER**

Glxgears independently repeats this organization.  Sequence 20 range 0 emits
preprocess ranges `[0,44)`, `[44,84)`, `[84,128)`, `[128,168)`, and
`[168,252)`.  Range 1 emits corresponding raster ranges `[0,42)`, `[44,84)`,
`[84,126)`, `[128,168)`, and `[168,250)`.  Sequence 22 repeats those values.
The paired draws and exact output/input address joins recur for each batch.
**CAP**

## Exact gltri pass deltas

`--triangle-diff` prints every index whose complete write sequence differs
between ranges 0 and 1.  The implementation-relevant groups are:

| state | range 0: preprocessing | range 1: raster |
|---|---|---|
| numeric draw | `[0,4)` | `[0,3)` |
| `22` topology/control | `0x0008040e` | `0x00006403` |
| `00..03` input resources | `00=0x40f52001`, `01=0xf2fe014f` | generated addresses shown above |
| `10,11` input controls | `10=0x00000014` | `10=0x00100010`, `11=0x00100006` |
| `23` data-port sequence | `00100dac 00100236 02200d88 02200ab6 00004b08 0 0 0` | `80100d88 801006b6 00010688 00c231b6 00d30688 0 0 0` |
| render outputs | `b0/b2/b4/b8` generated streams | `b0=0x11ee9000`, `b8=0x11f39000`, `ba=0x11f89000` |
| output descriptors | `b1/b3/b5=0x68002000`, `b9=0xb8002000` | `b1=0xcc210140`, `b9=0xfa210140`, `bb=0xf0010140` |
| depth/stencil | `a8=0x0000001f`, `a9=aa=0` | `a8=0x0038038a`, `a9=0x08ffff00`, `aa=0x00ffff00` |
| clip max | `f6=c5=0x7fff7fff` | `f6=c5=0x00f00140` |
| raster controls | `f0=0xf0000400`, `ef=0x00288800`, `d7=0x000050ae` | `f0=0xc4008400`, `ef=0`, `d7=0x200058a3` |
| internal controls | `8c/8d/8e=00501600/40c3a011/40c3a0c1` | `00500200/40c3a001/40c3a011` |

The full differential also records `18/19`, `30`, `43`, `88/89`, `ab..af`,
`be`, `c3`, `e3/e6/eb`, `f1`, and `f4`.  These remain numeric until their
semantics are joined by a controlled state change.  A driver may replay their
captured values in the matching diagnostic path, but should not expose them as
raw userspace controls.  **CAP**

## Raster render, depth, clip, and program state

The settled gltri raster pass emits this directly actionable state:

| indices | captured value | role/confidence |
|---|---|---|
| `b0,b1` | `0x11ee9000, 0xcc210140` | color target 0 and descriptor (**CAP/BIN**) |
| `b8,b9` | `0x11f39000, 0xfa210140` | ordinary depth target and descriptor (**CAP/BIN**) |
| `ba,bb` | `0x11f89000, 0xf0010140` | internal auxiliary depth backing (**CAP/BIN**) |
| `a8,a9,aa` | `0x0038038a, 0x08ffff00, 0x00ffff00` | depth/stencil state (**CAP/BIN**) |
| `f5,f6` | `0, 0x00f00140` | clip `[0,0,320,240)` (**CAP/BIN**) |
| `c4,c5` | `0, 0x00f00140` | zero-origin transformed clip copy (**CAP/BIN**) |
| `70..73` | all zero | texture-unit selectors disabled (**CAP**) |
| `69` | `0x11e42000` | relocated external fixed-fragment-database binding; the old address-directed BAR bytes are an opaque alias and its translation is unresolved (**CAP/BIN/DER**) |
| `6a` | `0x25010000` | paired numeric control; exact encoding and stage meaning open (**CAP/BIN**) |
| `68,6b` | `0, 0x00001030` | program controls; exact subfields open (**CAP**) |
| `6c` | `0x40c3a301` | relocated selected linked-program-state record; A/B/A ownership closed by the GLSL capture (**CAP/BIN/DER**) |
| `c2,c3` | `0x11f91000, 0` | internal resource and control; exact role open (**CAP**) |

Descriptor bits `31:25` are the hardware format and bits `15:0` are the row
span.  Therefore `b1=0xcc210140` has format `0x66` and span 320,
`b9=0xfa210140` has format `0x7d` and span 320, and `bb=0xf0010140` has format
`0x78` and span 320.  The exact descriptor bit layout and format tables are in
[parhelia-m9-render-surfaces-clear.md](parhelia-m9-render-surfaces-clear.md).

The static depth lowering proves `a8` bit 0 is depth-test enable and bit 1 is
depth-write enable.  The live differential validates bit 0 independently:
gltri uses `a8=0x0038038a`, while the glxgears raster pass uses
`a8=0x0038038b`; all other `a8` bits are equal.  Glxgears has depth testing
enabled.  Consequently hardware depth testing without depth update is encoded
by setting bit 0 and clearing bit 1 while preserving the compare/stencil
fields.  It requires no CPU or linear depth shadow.  **BIN/CAP/DER**

Glxgears uses `f6=c5=0x012c012c`, the exclusive 300 by 300 endpoint.  Its
raster batches alternate index `22` values `0x00006406` and `0x00006407`, with
the low bit of `d7` changing in step.  This ties `22` to topology or a closely
coupled primitive submode, but the individual low-bit meanings are not closed.
Preserve the captured pair for diagnostic replay and obtain controlled points,
lines, independent triangles, strips, and fans before writing a general
topology packer.  **CAP/DER**

## The `8f/90..9f` indexed store

Gltri sequences 0, 8, 10, and 14, glxgears sequence 18, and glxgears sequence
24 contain the same 80 consecutive two-dword slots.  Selector `8f` starts at
`0x80000000` and advances by `0x40` per eight slots; paired data ports
`90/91` through `9e/9f` carry the slot dwords.  Reassembled in slot order, the
image is `0x280` bytes with SHA-256:

```text
78434eea53365d762f7eda163d5981ab765d754d1bed9e5377ed231e30bc34fb
```

Many slots have low patterns also seen in the recovered native M9 ISA, so the
image is safely described as a native-looking indexed-store program image.
It does not contain the separately recovered terminal slot
`fffc7e1d a2803f0f`.  It must therefore not be identified as the application's
vertex shader, fragment shader, or as the `68/60..67` inline-program class
without an isolated program differential.  The later controlled GLSL A/B/A
capture supplies that differential: neither a vertex-only nor fragment-only
switch uploads `90..9f`, while both switch the linked record selected through
`6c`.  The 80-slot image is therefore context/invariant state rather than the
selected application GLSL program.  **CAP/DER**

Static disassembly of the saved Linux Xorg 1.13 DDX
`m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/1_13_0/m9x_drv.so`
(SHA-256 `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`)
joins this transport to exported function `m9x30355` at virtual address
`0x4ae850`; `m9x23192` calls it at `0x4af2de`.  It consumes a count at object
`+0x5a0` and 16-byte records beginning at `+0x5a8`; `m9x29052` at `0x4acc20`
loads a record dword through its pointer.  The selector construction is at
`0x4aea15..0x4aea33` and the `90..9f` header construction is at
`0x4ae99f..0x4ae9be`.  A second record list at `+0x19c` selects the
`0xc0000000` class and uses helper `m9x15572` at `0x4acb70`.  The captured
stream shows adjacent records merged into packets such as `8f,90,91`.
**BIN/CAP**

Keep this store separate from program binding through `69/6a/6b/6c` and from
the statically recovered inline `68/60..67` uploader.  The controlled result
and allocation join are documented in `parhelia-m9-glsl-bind-capture.md`.

## Render-target-to-texture rectangle pass

Gltri sequence 14 has no raw numeric draw record.  It binds the previous
triangle's color target as a texture and launches a rectangle through `fc`:

| indices | value | role |
|---|---|---|
| `f0,ef,d7` | `0x080c040c, 0x000000aa, 0` | rectangle/present controls |
| `f2` | `0` | source origin |
| `f3` | `0x00160001` | destination `(1,22)` |
| `f4,fc` | `0x00f00140` | 320 by 240 extent and launch |
| `b0,b1` | `0x11521000, 0xf0210780` | destination and 1920-element span descriptor |
| `70` | `0` | texture unit 0 selector |
| `78` | `0x11ee9000` | texture address, exactly prior raster `b0` |
| `79` | `0xf0210140` | texture descriptor word 0 |
| `7a` | `0x000100f0` | texture descriptor word 1: height 240, depth/layers 1 |
| `7c,7d,87` | `0, 0x0ff80000, 0` | sampler control, LOD bounds, auxiliary control |
| `7e,7f,80,81` | `0,0,0xffffffff,0` | border/remaining sampler state |
| `82..85` | all zero | bump/texture matrix block |

This closes an important packet-boundary ambiguity: final index `78` receives
the relocated base address, `79` descriptor word 0, and `7a` descriptor word
1.  Some static helper notes call their output variables `idx78` and `idx79`
for the two descriptor words; those variable names do not mean the final
packet omits the address.  The emitted packet is the three-dword resource
triple above.  **CAP/BIN**

Only one root address is emitted, followed by the compact descriptor and LOD
bounds.  This supports the recovered packed mip-root/tail model and disproves
a per-level-address packet for this path.  The capture does not by itself map
the complete filter, wrap, anisotropy, compare, or LOD bitfields.  Those need
one-state-at-a-time sampler captures.  **CAP/DER**

## Completion and dependency ordering

The final range has this shape:

```text
index 1b = second fence writeback address
e6 = 0x00000003
e6 = 0x00000002
e3 = target | 1
index 43 = target
```

For gltri sequence 12, `e3=0x000002c1` and `43=0x000002c0`.  Static kernel
initialization maps MMIO `0x106c` to index `1b`, proving the second completion
writeback address.  The low-bit-set `e3` value and low-bit-clear `43` value
carry the same target.  Index `43` also appears before dependent sampling and
is the exact producer dependency/order value.  Completion is a GPU memory
writeback; it is neither an IRQ event nor the `0x8f0` idle indication.
**CAP/BIN/DER**

The kernel must append and validate completion itself, keep referenced BOs
resident through retirement, and treat `43` as an ordering dependency rather
than a user-provided arbitrary address or tag.

## Driver-ready facts and remaining capture blockers

The current evidence is sufficient to build a diagnostic M9148 path that
replays the captured fixed program/state, preprocesses vertices, rasterizes an
untextured triangle or the gears strips, binds color/depth/auxiliary targets,
uses hardware depth test with writes on or off, clips to exclusive endpoints,
performs the captured texture rectangle, and retires through a memory fence.

A general Mesa driver still needs controlled differentials for:

1. the exact format, stride, and component interpretation of generated input
   resources `00..03`, plus the general vertex declaration lowering;
2. index `22` topology subfields and the correlated `d7` bit;
3. native instruction storage referenced by the now-identified `6c` linked
   record, its internal vertex/fragment entry fields, and the mode-2 constant
   backing layout;
4. sampler bitfields in `7c/7d/7e/7f/80/81/87` and controlled mip transitions;
5. blend, alpha-test, stencil, cull/fill, and color-mask live differentials,
   even where static packers already exist.

These gaps constrain state compilation; they do not reopen the ExecuteDma
framing, render/depth attachment indices, exclusive clip convention,
two-pass address join, texture resource triple, or completion footer.
