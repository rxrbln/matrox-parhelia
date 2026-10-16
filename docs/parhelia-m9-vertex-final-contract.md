# M9 vertex preprocessing, transient streams, and draw batching closeout

This note audits the saved M9148 Linux captures and Windows/Linux vendor
binaries for the remaining vertex-front-end contracts needed by a new driver.
It covers the settled `gltri` preprocessing pass, transient resources consumed
at indices `00..04`, source element layouts, primitive strips/fans, and draw
batching.  It does not use a live target.

Run the deterministic verifier from the repository root:

```sh
python3 tools/m9-vertex-final-contract-evidence.py --self-test
```

The verifier checks the complete twenty-case topology manifest, hash-pins the
untextured and textured `gltri` pairs, hash-pins `MxMD.dll` and the Linux
`m9x_drv.so`, decodes every command stream, and checks instruction-aligned
binary slices for declaration lowering, primitive counts, batching, and the
three launch families.  **CAP** below means a saved command capture, **BIN** a
hash-pinned vendor binary, and **DER** an exact deduction from those facts.

## The settled untextured `gltri` pipeline

Sequence 12 in `captures/m9x-linux-gltri-20260914` has these three contiguous
logical ExecuteDma ranges: **CAP**

```text
preprocess  [0x000,0x268)  physical BM [0x008,0x270)
raster      [0x268,0x4bc)  physical BM [0x270,0x4c4)
completion  [0x4bc,0x4e8)  physical BM [0x4c4,0x4f0)
```

The H03 range records are at physical offsets `0x264` and `0x4b8`.  The first
draws `[0,4)` and the second draws `[0,3)`.  This is one application triangle:
the four-element draw generates raster inputs and the three-element draw
rasterizes them.  Dropping the first record is not a batching optimization.
**CAP/DER**

The producer/consumer address join is exact:

| preprocess target | address | raster input | address |
|---:|---:|---:|---:|
| `b0` | `113c1000` | `00` | `113c1000` |
| `b4` | `113ee000` | `01` | `113ee000` |
| `b8` | `11404800` | `02` | `11404800` |
| `b2` | `113d7800` | `03` | `113d7800` |

Thus the invariant routing is `[b0,b4,b8,b2] -> [00,01,02,03]`.  Output
descriptors are `b1=b3=b5=68002000` and `b9=b8002000`.  Raster input controls
are `10=00100010`, `11=00100006`; its eight index-`23` port values are:

```text
80100d88 801006b6 00010688 00c231b6
00d30688 00000000 00000000 00000000
```

The topology corpus carries the same words with bit 31 clear in the first two
entries.  Their low 31 bits and every remaining entry match.  Bit 31 and the
simultaneous `22` base change (`00006403` versus `00002403`) must remain
capture-template state until isolated; neither bit changes the address join.
**CAP**

In all nineteen ordinary topology/source cases the four output bases are
`113c1000`, `113d7800`, `113ee000`, `11404800`, in target order
`b0,b2,b4,b8`.  Consecutive observed bases differ by `0x16800`.  That is an
address-placement observation, not proof that `0x16800` is the BO size,
alignment, or usable payload extent.  The captures contain no allocation
records or contents snapshots for these four addresses.  **CAP**

## Five-stream routing is shared by points and textured triangles

The point capture and the saved one-level textured `gltri` capture both use
three four-element preprocessing draws.  Their target state is byte-identical
apart from the first-stage control and source declaration: **CAP**

| stage | `b0/b1` | `b2/b3` | `b4/b5` | `b8/b9` |
|---:|---|---|---|---|
| 0 | `10ec1000/ee19000c` | `10ec1000/ee19040c` | `10ec1000/ee19080c` | `0/b8010001` |
| 1 | `113c1000/68002000` | `113d3400/68002000` | retained stage-0 view | `1140a000/b8002000` |
| 2 | `113e5800/68002000` | `113f7c00/68002000` | retained stage-0 view | `1140a000/b8002000` |

Stage 0 writes three views of one scratch resource at `10ec1000`; descriptor
values distinguish the views.  Stages 1 and 2 produce four private addresses
and share `1140a000`.  Calling `0x12400`, the step between the five final
addresses in allocation order, a size or alignment would exceed the evidence.

The point first stage uses `22=0008040e`, while the textured first stage uses
`22=0008060e`.  Both later stages use `22=0008000e`.  Their final consumers use
the same controls:

```text
10=00100010  11=00060010  12=00000010
```

They do not use the same routing or declaration:

| path | raster `00,01,02,03,04,05` |
|---|---|
| points | `113d3400,113f7c00,113c1000,1140a000,113e5800,0` |
| textured triangle | `113c1000,113d3400,113f7c00,1140a000,113e5800,0` |

Point raster index `23`:

```text
80100d88 801006b6 00010688 00e20db0
00c331b6 00d40688 00000000 00000000
```

Textured-triangle raster index `23`:

```text
80100d88 801006b6 00510688 00020688
00c331b6 00d40688 00000000 00000000
```

The five-stream template therefore cannot be selected merely by stream count
or treated as a point-expansion format.  Topology and active varying layout
select separate route/declaration templates over a shared three-stage
preprocessor skeleton.  **CAP/DER**

## Source element formats, spans, and semantic boundary

The Linux ICD copies or repacks the tested client arrays into four canonical
preprocessing source layouts: **CAP**

| source attributes | index `10` span | distinguishing index-`23` values |
|---|---:|---|
| float2 position, current color | `14` | `00100dac 00100236 02200d88 02200ab6` |
| float3 position, current color | `18` | `00100daa 00100236 03200d88 03200ab6` |
| float2 position, ubyte4 color | `0c` | `00100dac 00100236 0220cd88 0220c6b6` |
| float3 position, ubyte4 color | `10` | `00100daa 00100236 0320cd88 0320c6b6` |

All four end with `00004b08 0 0 0`.  These exact record deltas establish:

* float3 position adds four bytes over float2;
* an enabled packed ubyte4 color array reduces the canonical span by eight
  bytes relative to the tested current-color form;
* the active float2 texture coordinate in textured `gltri` adds eight bytes,
  making span `1c`, and adds descriptor pair `05300dac 05300236` before the
  terminator.

The float2 tight, offset-4/stride-16, and `first=2` cases produce the same
span/declaration and a four-element preprocessing draw.  Float3 tight and
offset-4/stride-16 do likewise.  Case 17 proves that API `first=2` is rebased
to H03 `[0,4)`.  Case 13 is a useful exception to an overly broad claim that
all offsets disappear: its interleaved position begins four bytes into a
28-byte C record, its final canonical span is `0x10`, and its preprocess H03
range is `[1,5)`.  The original 28-byte stride is absent, but an alignment or
base-rebase unit survives as numeric start 1.  **CAP/DER**

Cases 13 and 14 enabled a texture-coordinate client array while texturing was
disabled.  Their declarations contain position and color only.  The active
textured case adds the pair above.  Dead fixed-function inputs are therefore
eliminated before the hardware declaration is emitted.  **CAP/DER**

Saved Windows `MxMD.dll` independently closes the API-side declaration
contract.  It has 16 stream descriptors and accepts up to 64 declaration
elements.  Classic types map to `(format,bytes)` as follows: **BIN**

```text
FLOAT1=(0,4) FLOAT2=(1,8) FLOAT3=(2,12) FLOAT4=(3,16)
D3DCOLOR=(4,4) UBYTE4=(5,4) SHORT2=(6,4) SHORT4=(7,8)
```

The D3D9 type-size table for types 0 through 17 is:

```text
4 8 12 16 4 4 4 8 4 4 8 4 8 4 4 4 8 0
```

Its internal D3D9 declaration record preserves stream, offset, type, method,
usage, and usage index.  This closes source parsing and a suitable driver IR;
the two-dword record is not final packet payload.  Linux `m9x26001` at
`0x4ba3a0` closes the other end: it reads a count at state `+0x10`, reads
16-byte records from `+0x14`, and emits groups of four values under index header
`23232323`.  **BIN**

The semantic boundary is now precise.  API source roles are known for the
controlled position, color, and texture-coordinate cases.  The four/five
generated resources' component order, scalar representation, per-element byte
stride, interpolation role, and clip-space conventions are not recoverable
from these files.  The command captures contain their addresses, not their
contents.  Index `10` controls such as `00100010` must not be decoded as two
strides solely from their appearance.  **CAP/DER**

## Primitive strips, fans, and preprocessing counts

Ordinary raster topology is selected by the same nibble in `22` and `d7`:
**CAP**

| GL primitive | selector | raster elements | preprocessing elements |
|---|---:|---:|---:|
| lines | `1` | 4 | 4 |
| line strip | `2` | 4 | 4 |
| line loop | `2` | 5 | 8 |
| triangles | `3` | 6 | 8 |
| triangle strip | `4` | 4 | 4 |
| triangle fan | `5` | 4 | 4 |
| quads | `6` | 8 | 8 |
| quad strip | `7` | 6 | 8 |
| polygon | `d` | 4 | 4 |

For these cases `22=00002400|selector` and
`d7=200058a0|selector`.  Triangle strips and fans are native raster modes in
the captured range; they are not expanded into triangle lists.  Line loop
shares line-strip selector 2 and inserts one closing vertex before raster.
The ordinary preprocessing count is exactly `round_up(raster_count,4)` for the
tested sizes.  Points and active-texture preprocessing use their three-stage
templates instead.  **CAP/DER**

All nonindexed cases use H03 `23000001`.  Unsigned-byte indices are promoted
to the same H06 packed-16 family `26000001` as unsigned short.  Unsigned int
uses H07 `27000001`.  Both preprocessing and raster passes use the selected
index family.  The optional launch bit `0x40000000` is clear in every controlled
case.  **CAP**

## Windows batching contract

The Windows operation-52 callback consumes 12-byte records
`{public_primitive_type,start_vertex,primitive_count}`.  Its normalized
primitive table is: **BIN**

```text
input 0..15 -> 0,1,2,3,5,6,7,0,2,3,4,5,6,7,9,10
```

For normalized selector `s`, helper `0x108c50` produces the following derived
count from primitive count `N`: **BIN/DER**

| selector | derived count |
|---|---|
| `0,1` | `N` |
| `2,9,11` | `4*N` |
| `3,4` | `N ? N+1 : 0` |
| `5,12` | `3*N` |
| `6,7,8` | `N ? N+2 : 0` |
| `10` | `N ? 2*N+2 : 0` |

When its temporary batching path is available, operation 52 groups consecutive
records with the same *public* primitive type.  For every input record it
retains a separate exclusive pair
`{start_vertex,start_vertex+derived_count}` and submits the array of pairs in
one backend call.  It does not merge adjacent or overlapping pairs into one
range.  A primitive-type change ends the group.  The non-batching path submits
the records one at a time through the same state helpers.  **BIN/DER**

The backend then selects a launch shaper.  The completely recovered terminal
records are: **BIN**

```text
H03: {launch_flags|03000001, range_cursor, range_cursor+batch_span}
H07: {launch_flags|07000001, relocated 32-bit-index start, exclusive end}
H06: {launch_flags|06000001, relocated packed-16 start, tagged exclusive end}
```

H07 advances by `4*N`.  H06 advances by `4*ceil(N/2)` and uses end bit 1 for
an odd terminal halfword.  Launch flags are `20000000`, or `60000000` when
draw-state bit 12 is set.  The scheduler passes its chosen `draw+0x1550`
batch span to the launcher; that span is not the API primitive count.  **BIN**

This establishes record coalescing and terminal encoding.  It does not close
the scheduler's maximum batch, the source of every `+0x1550` choice, or the
vertex overlap/carry policy when a long line/triangle strip or fan must be
split.  The four-element strip/fan captures never cross a batch boundary.

## Driver-ready result and remaining gates

The following pieces are ready to implement without further reverse
engineering:

1. exact two-pass untextured triangle replay;
2. four canonical position/color source layouts and active float2 texcoord
   extension, including dead-input elimination;
3. ordinary four-stream and point/textured five-stream relocation routing;
4. ordinary GL1 topology words, line-loop closure, and four-element
   preprocessing roundup for the captured sizes;
5. H03 nonindexed, H06 packed-16, H07 32-bit launches, including u8 promotion;
6. consecutive-type draw batching while retaining one exclusive range per
   application record.

A general from-scratch OpenGL 2 vertex path still has these concrete gates:

* capture or recover the generated transient contents to name the scalar
  formats, component order, byte stride, clip-space fields, and interpolation
  roles behind `00..04`;
* recover transient BO allocation size/alignment, lifetime, reuse, and the
  producer-to-consumer cache/dependency rule rather than copying observed GPU
  addresses or address steps;
* establish arbitrary generic vertex attributes, normals, secondary color,
  fog coordinate, uncommon source types, and larger varying sets;
* determine long-draw split limits and strip/fan carry vertices;
* isolate the `0x40000000` launch flag and the contextual high bits in index
  `22` and the first two raster declaration values.

An initial driver can execute the captured preprocessing templates and keep
the generated layout opaque.  It cannot safely synthesize direct raster input
buffers or advertise arbitrary GL2 vertex/varying layouts until the first two
gates are closed.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
