# M9 vertex input and primitive topology capture

This note records controlled live differentials from the M9148 vendor Linux
OpenGL stack.  It closes the ordinary GL1 primitive selector, draw family, and
canonical two-pass address joins.  **CAP** means directly observed in the
saved capture, **BIN** means statically recovered from the saved vendor binary,
and **DER** is a deterministic inference from those facts.

The complete fixtures are in
`captures/m9x-linux-vertex-topology-20260914`.  Run
`python3 tools/m9-vertex-topology-evidence.py --self-test` to decode all twenty
pairs and check every value below.

## Raster topology words

For ordinary non-point raster passes, the hardware selector `t` appears
identically in the low nibble of both state words: **CAP**

```c
index_22 = 0x00002400 | t;
index_d7 = 0x200058a0 | t;
```

| GL input | `t` | index `22` | index `d7` | raster elements |
|---|---:|---:|---:|---:|
| `GL_LINES` | `1` | `00002401` | `200058a1` | 4 |
| `GL_LINE_STRIP` | `2` | `00002402` | `200058a2` | 4 |
| `GL_LINE_LOOP` | `2` | `00002402` | `200058a2` | 5, closing vertex inserted |
| `GL_TRIANGLES` | `3` | `00002403` | `200058a3` | 6 |
| `GL_TRIANGLE_STRIP` | `4` | `00002404` | `200058a4` | 4 |
| `GL_TRIANGLE_FAN` | `5` | `00002405` | `200058a5` | 4 |
| `GL_QUADS` | `6` | `00002406` | `200058a6` | 8 |
| `GL_QUAD_STRIP` | `7` | `00002407` | `200058a7` | 6 |
| `GL_POLYGON` | `d` | `0000240d` | `200058ad` | 4 |

Points take the special raster state `22=00006500`, `d7=200058a0`.  This
agrees on selector zero but changes index `22` bits `14` and `8` relative to
the ordinary `0x2400` base.  It is a distinct expansion path rather than an
ordinary one-element launch.  **CAP/DER**

The captured preprocessing state is invariant at `d7=000050ae`.  It uses
`22=0008040e` for ordinary paths and `22=0008000e` for points.  The topology
therefore belongs to the raster pass; selector `e` is the fixed preprocessing
operation.  **CAP**

The saved Windows `MxMD.dll` independently maps its API primitive enum through
`0x1ef8c4`, and helper `0x108780` packs its normalized selector into four
internal shadows.  The final Linux emitter `m9x21922` at `0x499da0` copies its
state `+0x38` to hardware index `22`, and copies state `+0x20/+0x24/+0x28` to
`ef/d7/f0`.  Those static paths confirm these are compiled state fields rather
than words embedded by the capture tool.  **BIN**

## Draw counts and software expansion

The preprocessing pass rounds ordinary work to four-element groups.  Lines,
line strip, triangle strip, triangle fan, and polygon use four elements;
six independent triangle vertices, the five-vertex closed line loop, eight
quad vertices, and six quad-strip vertices use eight.  The raster pass receives
the exact post-expansion API element count shown in the table.  **CAP/DER**

Points are more involved.  Three API points produce three preprocessing H03
draws of four elements each and one three-element raster H03 draw.  The vendor
allocates a new 36 KiB BM buffer after its first 32 KiB allocation is refused
for this larger `0xa3c` submission.  A new driver must budget the point
expansion path accordingly and must not model it as a single four-element
preprocess.  **CAP**

All nonindexed cases use the raw range record `23000001` (H03 plus launch bit
`0x20000000`).  No controlled case sets optional launch bit `0x40000000`.
Indexed draws select their range family by the post-conversion index width:
**CAP**

| API indices | draw header | final representation |
|---|---:|---|
| `GL_UNSIGNED_BYTE` | `26000001` | H06 packed 16-bit, promoted by the ICD |
| `GL_UNSIGNED_SHORT` | `26000001` | H06 packed 16-bit |
| `GL_UNSIGNED_INT` | `27000001` | H07 32-bit |

Both indexed passes carry relocated address ranges.  Their low address tag is
preserved while H06 uses bit 1 of the exclusive end to mark an odd final
halfword.  These observations match the exact H06/H07 endpoint formulas in
`parhelia-m9-draw-frontend.md`.  **CAP/BIN**

## Canonical vertex input

The vendor copies GL client arrays into a canonical preprocessing input.  The
isolated offset/stride tests and `glDrawArrays(first=2)` case all launch
`[0,4)`; their original byte offset, caller stride, and first value do not
survive.  The interleaved position/color case is a boundary exception: it has
canonical span `0x10` but launches `[1,5)`, so a one-unit alignment/base rebase
survives there even though its original 28-byte C stride does not.  Component
count and enabled color also change final input state. **CAP/DER**

| input case | preprocessing index `10` | index `23` distinguishing fields |
|---|---:|---|
| float2 position, current color | `00000014` | `00100dac ... 02200d88 02200ab6` |
| float3 position, current color | `00000018` | `00100daa ... 03200d88 03200ab6` |
| float2 position, ubyte4 color | `0000000c` | `00100dac ... 0220cd88 0220c6b6` |
| float3 position, ubyte4 color | `00000010` | `00100daa ... 0320cd88 0320c6b6` |

The float2 results are identical for tight layout and byte-offset-4/stride-16;
the float3 results are likewise identical for tight and offset/stride layouts.
Index `10` is therefore the canonical input record span, not the application's
original stride.  The low-bit differences in the index-`23` records encode the
position component count and packed color presence.  Their exact subfield
names remain unproved, so the driver should initially emit one of these four
captured canonical layouts rather than expose the words as a userspace ABI.
**CAP/DER**

The `m9x26001` Linux state emitter at `0x4ba3a0` proves that index `23` is a
variable-length state list: count at state `+0x10`, four dwords per record at
`+0x14`, emitted four values at a time under header `23232323`.  It also proves
this list is ordinary indexed hardware state and not inline vertex data.
**BIN**

## Generated raster inputs

Every ordinary case retains the exact producer-to-consumer permutation:
**CAP**

```text
preprocess [b0,b4,b8,b2] == raster [00,01,02,03]
```

The preprocessing output descriptors remain
`b1=b3=b5=68002000, b9=b8002000`.  The raster consumer controls are invariant
at `10=00100010, 11=00100006`, and its index-`23` list is invariant:

```text
00100d88 001006b6 00010688 00c231b6
00d30688 00000000 00000000 00000000
```

This is a complete replay contract for the four canonical generated streams:
four relocations, the two consumer controls, and the eight descriptor words.
It does not yet assign GL semantic names or scalar formats to each stream.
That naming requires a synchronized contents snapshot or a one-output-at-a-time
shader differential.  A new Mesa backend can proceed by producing and consuming
this canonical layout exactly; arbitrary direct-to-raster vertex layouts should
remain disabled until their semantic map is proved.  **CAP/DER**

Points use five generated raster inputs and a third consumer control:

```text
00=second-pass b2, 01=third-pass b2, 02=second-pass b0,
03=shared b8, 04=third-pass b0, 05=0
10=00100010, 11=00060010, 12=00000010
```

The precise point-expanded stream meanings remain internal, but this closes
their count, address join, consumer count, and exact captured descriptor state.

This five-stream shape is not point-only.  The saved one-level textured
`gltri` capture uses the same three-stage transient-output skeleton and the
same three consumer controls, but routes
`113c1000,113d3400,113f7c00,1140a000,113e5800,0` to `00..05` and uses a
different index-`23` list.  The exact two variants and their common transient
stages are pinned in `parhelia-m9-vertex-final-contract.md`. **CAP/DER**

## Implementation boundary

The topology packer, GL1 software-expansion decisions, nonindexed and indexed
launch families, canonical preprocessing input variants, ordinary four-stream
join, and five-stream point join are now directly implementable.  Remaining
vertex-front-end work is limited to semantic naming of generated components,
server-side VBO zero-copy layouts, optional `0x40000000` launch conditions, and
uncommon GL types.  None of those prevents a conformant first implementation
that repacks to the captured canonical layouts and lowers unsupported topology
in software.
