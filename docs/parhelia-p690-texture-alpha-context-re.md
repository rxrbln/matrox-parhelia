# P690 texture-alpha context route

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note isolates the remaining P690 texture-alpha question: which state is
present in the vendor context but absent from the direct Mesa sampler/combiner
packet.  It records a capture and probe plan only.  It does not license a
production Mesa change.

## Result so far

The direct P690 cutout probe has already established all of these facts:

* A8R8G8B8 storage, texture addressing, RGB sampling, the RGBA vertex fetch,
  the alpha comparator (`0x12e4`), and the alpha pipe work.
* With the captured `GL_RGBA` `MODULATE` combiner, an `A=0` texture still
  passes `GL_GREATER, 0.5`; setting vertex alpha to zero rejects the same
  primitive.  Therefore the direct packet reaches vertex alpha but not sampled
  alpha.
* `0x11e8=0x00500000` has no effect.  It belongs to the AA-gated sampler cache
  group and is not a supported alpha selector.
* `0x1200=0x00040103`, seen in a PCI-X 0528 capture, times out the P690.  The
  target-safe value is `0x00040101`; `03` must not be used as an alpha fix.

The remaining evidence-backed candidate is the vendor's stage-state context
path, key `0x0e`, which is processed before hardware state is derived.  This
is distinct from Mesa's per-draw `0x1200`/`0x11c0..0x11e8` sampler window and
its packed `0x1260..0x126c` combiner words.

## Working P690 context evidence

`captures/ioq3-p690-working-draw.txt` is a trace of a working vendor-ICD
ioquake3 session on the P690.  At stream offset `0x378` it contains:

```
000378  0000000e       key: per texture stage
00037c  00000070       112 eight-byte entries
```

112 is four copies of the 28-item stage image.  Each item is:

```
(u16 unit, u16 field, u32 value)
```

`field 0x0c` (`D3DTSS_ADDRESS`, an alias for both U and V) is omitted because
this image supplies the separate U/V fields.  The first P690 unit's
alpha-relevant prefix is:

| field | D3D texture-stage state | captured value | meaning |
|---:|---|---:|---|
| 1 | `COLOROP` | 4 | `D3DTOP_MODULATE` |
| 2 | `COLORARG1` | 2 | `D3DTA_TEXTURE` |
| 3 | `COLORARG2` | 0 | `D3DTA_DIFFUSE` |
| 4 | `ALPHAOP` | 2 | `D3DTOP_SELECTARG1` |
| 5 | `ALPHAARG1` | 2 | `D3DTA_TEXTURE` |
| 6 | `ALPHAARG2` | 0 | `D3DTA_DIFFUSE` |

The values are directly visible in the trace as `00010000=4`,
`00020000=2`, ..., `00060000=0`.  Field zero is an auxiliary stage record
handled separately by the Linux driver and has value zero for unit 0.  It is
not a `D3DTSS_*` token.

A scan of every saved textual stage-record capture finds unit-0 field 4 with
value 2 in all seven captures that contain the complete pair, and finds no
unit-0 `ALPHAOP=MODULATE(4)` pair.  The archive therefore has no recorded
key-`0x0e` delta for the required `SELECTARG1` to `MODULATE` transition.  In
particular, it does not contain a controlled RGBA plus alpha-test draw from
which a context delta could be replayed.

This proves that the working P690 context explicitly describes texture-alpha
selection, even though the final draw's raw alpha combiner packet is a separate
object.  It does **not** prove that this particular draw was an RGBA cutout:
the trace does not identify the texture format or alpha-test state for that
specific context transition.  It is useful because it provides the exact
context interface and establishes that fields 4--6 are live vendor inputs.

## Binary path and output boundary

The saved Linux driver decodes key `0x0e` at `mtx558+0xaec` (`0x459dc`);
its eight-byte item loop begins at `mtx558+0xb1b` (`0x45a0b`).  For each item
it computes a 130-byte per-stage descriptor at `device+0xdd7+0x82*unit`.
Field zero updates that descriptor's auxiliary word.  Fields 1 through 28 call
`mtx673` at `0x17260`.

`mtx673` has a 29-entry dispatch table and stores `COLOROP`/`COLORARG*` at
`+0x2c..+0x38`, `ALPHAOP`/`ALPHAARG*`/`RESULTARG` at `+0x3c..+0x4c`, then
sets descriptor dirty bit `+0x1:0x10`.  The same field layout and
operation/argument enums are independently established from the Windows D3D
driver.

The immediate output staging is now located.  `mtx3669` calls `mtx2223`
(`0x182b3`), which copies all ten raw 0x82-byte descriptors into ten
0xa0-byte records at `device+0x434+0xa0*stage`.  The alpha fields have this
exact output map:

| raw descriptor | output record | state |
|---:|---:|---|
| `+0x3c` | `+0x10` | `ALPHAOP` |
| `+0x40` | `+0x6c` | `ALPHAARG0` |
| `+0x44` | `+0x14` | `ALPHAARG1` |
| `+0x48` | `+0x18` | `ALPHAARG2` |
| `+0x4c` | `+0x70` | `RESULTARG` |

`mtx580` calls the same copier and returns the selected
`device+0x434+0xa0*stage` record.  This proves a concrete context output for
fields 4--6.

The later reader is now identified, but its destinations remain opaque.
`mtx3828` allocates a 0x858-byte, variant-4 context and initializes it through
`mtx1631`.  Its static input-ID table at `.data+0x8a0` is exactly
`1..11, 13..28`: 27 IDs; neither zero nor 12 is registered.  `mtx3136`
obtains the stage-zero record through `mtx580` and calls `mtx950`.  `mtx950`
invokes `mtx713` eight times for the 0x60-byte map containers at
`context+0x78+0x60*i`, advancing its source by 0x74 bytes each time.

The containers are in the 0x858 allocation, but their values are not fixed
fields in that allocation.  `mtx3772`, which builds each map entry, allocates
a separate four-byte object; `mtx713` copies the dword from `source + 4*ID`
to that heap pointer at the map node's `+0x28`.  Consequently the first source
slice demonstrably routes output `+0x10/+0x14/+0x18` (IDs 4/5/6) into the
heap-backed values for its first submap.  It does not establish fixed
destination offsets in the 0x858 context.  The other seven maps consume later
opaque 0x74-byte source slices and must not be treated as additional stage
records.

This is a stronger output checkpoint, but it is still not a hardware-register
packet.  The copier's source advances by 0x74 while `mtx2223` reserves 0xa0
per stage; a live object dump is required to resolve the other seven slices
and all dynamic destination pointers.  Neither `mtx950` nor `mtx713` emits the
`0x1200` sampler window, a fragment-register packet, or a key-`0x46` combiner
object.  `mtx3669` calls `mtx1900` immediately before `mtx2223`; `mtx1900`
constructs its unrelated `+0x2a0` context from `device+0xc20`-class fields and
does not read the `+0xdd7` alpha-operation words.  The compact WARP
texture-coordinate context is therefore a parallel consumer, not the final
destination of fields 4--6.

No later reader from this context path to the final packed combiner or
sampler-alpha route was found.  The evidence rules out treating
`device+0x434` as an additional MMIO state block.  It also rules out deriving
a replayable 0x858 offset table from the static binary: that table would need
the live heap-map pointers or their later consumers.  It remains a useful
capture checkpoint, while the final alpha route must be established by
correlated vendor command capture.

Complete key-`0x46` objects are saved for ordinary textured `gltri` draws
(for example, the 414-dword object at dword `0x18` in the final
`gltri-textured-0528-trace.txt` state image).  They do not coincide with an
alpha-test capture, and no saved key-`0x46` object is correlated with a
key-`0x0e` fields-4--6 transition.  Key `0x46` is therefore evidence for the
separate combiner/texture-address object only; it supplies no missing
sampler-alpha or context programming value.

Consequently, the following is the narrow hypothesis, not a demonstrated
hardware programming sequence:

```
RGBA + GL_MODULATE needs a context whose unit-0 alpha stage is
  { ALPHAOP=MODULATE(4), ALPHAARG1=TEXTURE(2), ALPHAARG2=DIFFUSE(0) }
while RGB remains
  { COLOROP=MODULATE(4), COLORARG1=TEXTURE(2), COLORARG2=DIFFUSE(0) }.
```

This differs only in field 4 from the saved unit-0 context prefix above.  The
open driver's raw combiner already encodes the requested alpha multiplication;
the candidate is that the vendor context compiler additionally selects the
sampler-to-alpha delivery path or a matching WARP/context variant.  No such
causal link has been demonstrated, so direct writes of this record or blind
WARP substitutions are unsafe.

## Required vendor comparison

Capture four otherwise identical one-texture draws through the vendor ICD on
the P690, retaining the complete key-`0x0e` record and the resulting context
object, not only changed ring dwords:

| case | texture | alpha test | expected sample |
|---|---|---|---|
| RGB control | RGB or RGBA with A=255 | disabled | painted RGB |
| RGBA opaque | A8R8G8B8, A=255 | `GREATER, 0.5` | painted RGB |
| RGBA cutout | same RGB, A=0 | `GREATER, 0.5` | unchanged background |
| vertex control | A=255 texture, vertex A=0 | `GREATER, 0.5` | unchanged background |

Use `GL_MODULATE`, unit 0 only, vertex alpha one except for the final control,
blending and depth disabled, and a fixed clear colour.  This separates sampled
alpha from depth, blend, interpolation, and RGB addressing.

For each case:

1. Decode all 112 entries as `(unit, field, value)`, preserving unchanged
   entries from the prior context image.
2. Diff unit 0 fields 1--6 first, then the auxiliary field zero and all
   texture-coordinate/transform fields.  The decisive expected delta is field
   4, `SELECTARG1(2)` versus `MODULATE(4)`, with fields 5/6 retained as
   `TEXTURE(2)`/`DIFFUSE(0)`.
3. Diff the compiler-produced `device+0x434+0xa0*unit` record first: fields
   4--6 must appear at output `+0x10/+0x14/+0x18`.  Also snapshot the
   post-`mtx950` 0x858 context and its dynamic map entries for IDs 4, 5, and
   6.  This resolves the compiler's opaque output slots without assuming that
   `+0x434`, its 0xa0 stride, or the mapped slots are MMIO/WARP state.
4. Capture key `0x46` (the fragment/combiner object), the final sampler window,
   and the alpha-test packet in the same submit.  If cutout and opaque draws
   differ in key `0x0e` but their final `0x46`, sampler, and `0x12e4` images
   are identical, that is positive evidence for a hidden context side
   condition.  If key `0x46` differs, decode that delta before testing any
   context change.
5. Inspect the compact WARP texture-coordinate block and WARP bind/program
   only as parallel consumers.  A format change from A8R8G8B8 (`0x32`) makes
   this a different question.

Only after this vendor delta is captured should a private,
non-Gallium-selected DRM diagnostic replay the *entire observed delta* beside
the already-proven direct packet.  It must require opaque paint, A=0 rejection,
vertex-A=0 rejection, and fence retirement before any broader test.  Do not
install a module into `/lib/modules`; use the existing temporary-module test
path for any subsequent target probe.

## Explicit exclusions

Do not perform a register sweep, change A8R8G8B8 to XRGB, retry `0x1200=03`,
or add `0x11e8` to normal packets.  Each either already failed on the P690 or
cannot distinguish the alpha-route hypothesis from an unrelated sampler
failure.
