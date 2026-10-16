# Matrox M9 draw and vertex frontend

This note records the exact Direct3D draw callback, topology, stream/index,
inline-vertex frontend, and draw-launch records recovered from saved Windows
`MxMD.dll`.  The context draw methods are now joined to the final command-buffer
writes.  `[BIN]` denotes direct binary data flow and `[DER]` a deterministic
decode.

## Real D3D callback path

`DdGetDriverInfo` at `0x3db50` returns the `D3DCallbacks3` GUID
`{DDF41230-EC0A-11d0-A9B6-00AA00C0993E}` from callback blob `+0xa8` [BIN].
Blob builder `0x77130` writes size `0x18`, flags `0x0c`,
`D3dDrawPrimitives2=0xbeeb0`, and `ValidateTSS=0xbf030`.  The draw path is:

```text
0xbeeb0 -> 0xddf20 setup -> 0xde020 parser -> 0xde200 finalizer
```

The real 256-operation dispatch table is at `0x20c9a0`, initialized by
`0xdd8e0/0xddc00`; default handler is `0xe0a80`.  Older M9 notes named
`0xffd20`, `0x87a70`, and `0x1efff0`, but in this exact binary those are an
unrelated constructor, temporary helper, and predicate data.

## Primitive topology

Modern draw handlers `0xe02d0`, `0xe04f0`, and `0xe0610` map the D3D primitive
enum through table `0x1ef8c4` [BIN]:

```text
input 0..15 -> 0,1,2,3,5,6,7,0,2,3,4,5,6,7,9,10
```

Helper `0x108780` writes the internal selector to context state as follows:

| destination | field | dirty mask |
|---|---|---:|
| `ctx+0x2528` | `+0x48[3:0]` | `+0x00 |= 1` |
| `ctx+0x252c` | `+0x04/+0x08` | `+0x00 |= 0x20000` |
| `ctx+0x2514` | `+0x0c[14:11]` | `+0x04 |= 0x02000000` |
| `ctx+0x2520` | `+0xc2` | `+0x00 |= 0x00800000` |

Legacy operations expose these selector meanings directly [BIN]:

| operation | selector |
|---|---:|
| POINTS | `0` |
| LINELIST / INDEXEDLINELIST2 | `2` |
| LINESTRIP / INDEXEDLINESTRIP | `3` |
| TRIANGLELIST / INDEXEDTRIANGLELIST2 | `5` |
| TRIANGLESTRIP / INDEXEDTRIANGLESTRIP | `6` |
| TRIANGLEFAN / INDEXEDTRIANGLEFAN | `7` |
| TRIANGLEFAN_IMM | `8` |
| INDEXEDLINELIST | `0xb` |
| INDEXEDTRIANGLELIST | `0xc` |

Selectors `0xb/0xc` are special old explicit-index record forms.

Helper `0x108c50` converts primitive count `N` into a derived count stored at
`ctx+0x252c+0x244`:

```text
selector 0..12:
N, N, 4N, N ? N+1 : 0, N ? N+1 : 0, 3N,
N ? N+2 : 0, N ? N+2 : 0, N ? N+2 : 0,
4N, N ? 2N+2 : 0, 4N, 3N
```

Selector 2 producing `4N` is surprising for D3D LINELIST, so this is a proven
derived count field but cannot yet be named consumed-vertex count.

## Stream and index state

Dispatch operation 49 calls `0xe0ea0` for 12-byte SETSTREAMSOURCE records
`{stream,handle,stride}` [BIN].  For stream below 16 it updates a 24-byte
descriptor at `state+0x10+stream*24`:

| field | resource form |
|---:|---|
| `+0x00` | zero, resource mode |
| `+0x04` | resolved resource wrapper `+0x08` |
| `+0x08` | `min(stride,0xfffe)` |
| `+0x10` | zero |

Handle zero clears `+0x00/+0x08/+0x10`.  A 16-bit mask at state `+0x00` marks
dirty streams, and context tables mirror the handle/resource.

Operation 50 calls `0xe01c0` for 8-byte SETSTREAMSOURCEUM records
`{stream,stride}`.  It sets descriptor `+0x00=1` for user-memory mode,
`+0x04=parser+0x30` vertex base, `+0x08=stride`, and
`+0x0c=[parser-result+0x20]`, with the same dirty mask.

Operation 51 calls `0xe0da0` for 8-byte SETINDICES records.  The index descriptor
at state `+0x190` becomes resource mode 0, resolved resource at `+0x194`, and
record dword 1 at `+0x198`; dirty bit is `0x10000`.  Legacy inline helper
`0x1086f0` proves the layout more clearly:

| field | inline form |
|---:|---|
| `+0x190` | 1, inline/user source mode |
| `+0x194` | payload pointer/offset |
| `+0x198` | 2, 16-bit index element size |
| `+0x19c` | bounded length/range |

Thus `+0x190` is source mode, `+0x194` base, `+0x198` element stride/size, and
`+0x19c` a bound.  Exact resource-form meaning of record dword 1 still needs the
API structure join.

## Vertex declaration lowering

DP2 operation 45 CREATEVERTEXSHADER uses handler `0xdfc20` and declaration
parser chain `0x10f060 -> 0x10b690` [BIN].  The latter parses classic D3DVSD
tokens by class bits `31:29`: NOP=0, STREAM=1, STREAMDATA=2, CONSTMEM=4; it
accepts `0xffff` or `0xffffffff` as end.  STREAM is low four bits and must be
below 16.  Normal STREAMDATA uses register bits `4:0` up to 16, type bits
`19:16` up to 7, requires dword-aligned offset, and permits at most 64 elements.

It appends internal two-dword descriptors through `0xa9820` at declaration
object `+0x28` [BIN][DER]:

```c
d0 = (reg & 0x3f) |
     ((byte_offset & 0xfc) << 4) |
     ((type_format[type] & 0x1f) << 12) |
     ((reg_map0[reg] & 0x1f) << 21) |
     ((reg_map1[reg] & 0x1f) << 26) |
     (old_d0 & 0x80000000);
d1 = 0x0000000f;
```

The legacy type table at `0x1ef84c` is:

| D3D type | format | bytes |
|---|---:|---:|
| FLOAT1 | 0 | 4 |
| FLOAT2 | 1 | 8 |
| FLOAT3 | 2 | 12 |
| FLOAT4 | 3 | 16 |
| D3DCOLOR | 4 | 4 |
| UBYTE4 | 5 | 4 |
| SHORT2 | 6 | 4 |
| SHORT4 | 7 | 8 |

The register mapping table at `0x1f66ac` gives these `(map0,map1)` pairs for
registers 0..16 [BIN]:

```text
(0,0) (1,0) (2,0) (3,0) (4,0) (10,0) (10,1)
(5,0) (5,1) (5,2) (5,3) (5,4) (5,5) (5,6) (5,7) (0,1) (3,1)
```

D3DVSD SKIP, selected by STREAMDATA bit 28, increments the per-stream running
offset by raw bits `19:16`.  The units are surprising and remain unresolved.

The D3D9-style parser `0x10b8b0` accepts 8-byte elements
`{u16 Stream,u16 Offset,u8 Type,u8 Method,u8 Usage,u8 UsageIndex}`, ending when
Stream is `0xff`.  It emits [BIN][DER]:

```c
d0 = (Stream & 0x3f) |
     ((Offset & 0xfc) << 4) |
     ((Type & 0x1f) << 12) |
     ((Method & 0xf) << 17) |
     ((Usage & 0x1f) << 21) |
     ((UsageIndex & 0x1f) << 26) |
     (old_d0 & 0x80000000);
d1 = 0x0000000f;
```

Type sizes at `0x1ef88c` for types 0..17 are
`4,8,12,16,4,4,4,8,4,4,8,4,8,4,4,4,8,0`, matching
`D3DDECLTYPE_FLOAT1..FLOAT16_4/UNUSED`; the same 64-element cap applies.

These are internal declaration IR records.  Their stream/register bits `5:0`
and dword-offset bits `11:6` differ from the old Parhelia final-hardware layout,
so they must pass through M9-specific lowering and must not be submitted as
packet payloads directly.

## Inline vertex staging

DP2 LINELIST_IMM handler `0xdf4c0` and TRIANGLEFAN_IMM handler `0xdf610` use
state `ctx+0x252c` and first stream descriptor `+0x10` [BIN]:

```text
+0x00 = 1                     user-memory source
+0x04 = align4(payload vertex bytes)
+0x08 = parser vertex stride
+0x0c = total bytes
```

LINELIST_IMM uses `2 * primitiveCount * stride`; TRIANGLEFAN_IMM uses
`(primitiveCount + 2) * stride`.  Both set the low stream dirty bit.  This is
frontend staging and does not imply a hardware inline-vertex packet.

## Modern draw records

Operation 52 handler `0xe02d0` consumes 12-byte records
`{primitiveType,startVertex,primitiveCount}`.  It maps the type, sends
`startVertex` through state helper `0x1088d0`, converts count through `0x108c50`,
then invokes context `+0x30` vtable `+0x08`.  A batching path coalesces adjacent
same-type records into start/end pairs before one call.

Operation 53 handler `0xe0ad0` consumes the public 24-byte
`D3DHAL_DP2DRAWINDEXEDPRIMITIVE` layout [BIN]:

```text
+0x00 PrimitiveType       +0x0c NumVertices
+0x04 BaseVertexOffset    +0x10 StartIndex
+0x08 MinIndex            +0x14 PrimitiveCount
```

It maps the type, passes `BaseVertexOffset`, `MinIndex`, and `StartIndex` through
`0x1088d0`, converts `PrimitiveCount` through `0x108c50`, then invokes context
vtable `+0x0c`.  This handler does not copy `NumVertices` through `0x1088d0`.
Operation 59 handler `0xe04f0` also consumes 12-byte records and passes its
`+0x04` field through `0x1088d0`; the post-D3D8 operation-59 name remains
unresolved.

Helper `0x1088d0` writes state exactly:

| field | input register/argument |
|---:|---|
| `+0x228` | ESI |
| `+0x22c` | first stack argument |
| `+0x230` | EDX |
| `+0x23c` | EDI |
| `+0x240` | second stack argument |

It sets dirty bit `0x100000`.  Operation 52 supplies EDX=`startVertex` and zero
for the other four fields.  Operation 59 supplies the record `+0x04` value as
first stack argument and zero elsewhere.  Operation 53 therefore puts
`BaseVertexOffset` at `+0x230`, `MinIndex` at `+0x228`, and `StartIndex` at
`+0x240`, with `+0x22c/+0x23c` zero.  `0x13ee10` later folds
`BaseVertexOffset` into stream byte-address calculations such as
`(BaseVertexOffset + index) * stride + stream offset/base`; it is not a separate
dword in the final three-dword launch record [BIN].

## Context-object and draw-backend join

Context creation reaches `0x751f0 -> 0x753e0`.  The outer object has vtable
`0x1f4a5c`; its `+0x04` method at `0x11c4f0` returns the inner object stored at
main context `+0x30`.  The inner constructor `0x13c910` installs vtable
`0x1f4abc`, whose draw entries are [BIN]:

| slot | function | frontend |
|---:|---:|---|
| `+0x08` | `0x13e790` | operation 52, nonindexed |
| `+0x0c` | `0x13e7e0` | operation 53, indexed |

The nonindexed path is `0x13e790 -> 0x13e850 -> 0x13cd00 -> 0x13ee10`; the
indexed path is `0x13e7e0 -> 0x13ebe0 -> 0x13cd00 -> 0x13ee10`.  Both reach the
same draw object and call its executor `0x176760` at `0x13fc80`.  This disproves
the earlier boundary at the two indirect slots.

The draw object is `0x1580` bytes, initialized by `0x176570`, with working state
starting at object `+0x20`.  Its command-buffer object is at `+0x10`.  Before
each batch, `0x179940` calls command-buffer vtable `+0x58` to reserve/check
space, invokes per-batch state emission including `0x13d100`, and calls the
selected launcher at draw object `+0x150` with [BIN][DER]:

```c
launcher(draw,
         0,
         batch_index,
         batch_index == batch_count - 1,
         draw->batch_span,
         launch_flags);
```

Here `draw->batch_span` is `+0x1550`; `0x17a590` selects it from either the
device engine `+0x47dc` or draw `+0xe8`.  Thus it is a scheduler-selected
per-batch span, not a field with one fixed source.  `launch_flags` is
`0x20000000`, or `0x60000000` when draw-state bit 12 is set.

## Final draw-launch records

The simplest selected launcher, `0x17e480`, writes a three-dword contiguous
range record directly at `[draw+0x10]+0x04`, stores that packet address at
command buffer `+0x18`, and advances the write pointer by 12 bytes [BIN]:

```c
packet[0] = launch_flags | 0x03000001; /* 0x23000001 or 0x63000001 */
packet[1] = draw->range_cursor;        /* draw+0xc4 */
packet[2] = draw->range_cursor + batch_span;
if (last_batch)
    draw->range_cursor = 0;
```

The arithmetic makes dword 2 an exclusive end.  Primitive topology is not a
fourth packet dword: it has already been normalized and emitted as state before
the launcher.  `0x176760` classifies topology selectors with the table
`{0,0,1,1,1,2,2,2,2,2,2,1,2,2,1}` for selectors 0..14; classes 0, 1, and 2
take the one-, two-, and three-element minimum paths respectively.  It adjusts
its batch bookkeeping before `0x179940` [BIN].

The explicit-range launcher `0x17d900` emits the same header and one
`{header,start,end}` record per eight-byte range at draw `+0xa8`, iterating
through `draw+0xec` inclusive [BIN].  `0x176760` sets `+0xec` to
`range_count ? range_count - 1 : 0`.  On the last batch, `0x17d900` consumes
the emitted entries by updating `+0xc0/+0xa8/+0xb0/+0xf4/+0xa0`.  Therefore
`+0xc0` is an active explicit-range count/status field; it is not an indexed
draw marker.  The nonindexed shaper can set it, while the indexed shaper can
leave it zero.

Two relocation-backed launchers encode address ranges [BIN][DER]:

| function | header base | start | exclusive end |
|---:|---:|---|---|
| `0x17a960` | `0x07000001` | `A=draw+0x114` | `(A & ~3) + 4*N`, preserving address-class high bits |
| `0x17b620` | `0x06000001` | `A=draw+0x114` | `((A & ~3) + 4*ceil(N/2)) | ((N&1)*2)`, preserving high bits |

ORing the same launch flags produces `0x27000001/0x67000001` and
`0x26000001/0x66000001`.  The first formula is a range of 32-bit units; the
second packs 16-bit units and tags an odd terminal halfword.  Both call draw
resource vtable `+0x30` with `(A, draw+0x118, 1)`, then command-buffer relocation
vtable `+0x54` for payload dwords 1 and 2 with selectors `0x2c` and `0x2d`.
Those selectors are relocation selector arguments, not direct indexed-register
writes.

Launcher selection at `0x177970` also names alignment and topology variants
`0x17cfa0`, `0x17d1c0`, `0x17d620`, `0x17d9c0`, and `0x17dc20`.  They remain in
the same `0x03000001` numeric-range family, with endpoint merging or padding,
but their individual policy meanings are not yet sufficiently constrained.
The exact common record formats above do not require those names.

Functions `0x118740/0x118850` independently lower the 16 stream shadows and
index shadow into deeper resource descriptors.  In `0x118850`, source mode 1
passes index base/bound and element size to `0x119310`; resource mode 0 passes
the resource, element size, and bound through `0x119370`.  This is descriptor
lowering before launch and not another draw packet.

## Disproved packet candidates

The task-type switch at table `0xf8fac` is exact but does not participate in the
draw join.
Types 9/10 emit only `0x45fcf3ec` (`ec=0`, `f3/fc` rectangle bounds), and task 8
includes `0x7a7978f0`, `0xf3f0b1b0`, `e6/8f/90/92`, and `ef/fc`; caller context
does not tie these paths to DP2 triangles.  They remain rectangle/clip/task
records rather than a claimed 3D launch.

The following earlier hypotheses are disproved by caller context:

* `0xf9c70` and `2b,2a,e6` are 2D rectangle/stretch or mask/pixel inline data,
  not generic 3D primitives.
* `0xfc620`, `0xfcb60`, and `0xfc8f0` are cache/TLB/resource transitions, not
  vertex descriptors.
* `0x13fd70` is a resource-watermark scan, not the final draw launch.
* `0x1190e0` and the adjacent stream/index helpers lower state; they do not emit
  the three-dword launch record.
* `0x03000001` is a draw-record header in the command buffer.  Decoding its
  bytes as old indexed-register tags is unsupported.
* Similar-looking old Parhelia packet words do not establish M9 compatibility;
  these M9 formats must stand on their own recovered call chain.
