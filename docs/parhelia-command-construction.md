# Parhelia command construction — captured from live hardware

How the vendor OpenGL ICD actually builds and submits work. Captured 2026-09-05
on a real Parhelia 0x0528 (`Subsystem: Matrox Parhelia PCI 256MB`, PCI/PCI-X, no
AGP or PCIe capability) running the vendor 2009 mtx stack on kernel 2.6.30.

Instrument: `tools/parhelia-glsweep.c` — a GLX client that snapshots every
`/dev/matrox/mtx0` mapping of its own process before and after each GL
operation and records the changed dwords. This supersedes the earlier
gdb PRE/POST method: no breakpoints, one run captures the whole feature sweep.
Raw capture: `captures/glsweep-0528-raw.txt`.

Method note: the snapshot records the **whole changed span**, not only the
dwords that differ. A dword rewritten with the value it already held is still
part of the packet stream, and filtering to differences alone corrupts any
attempt to frame the records.

## The mapping set

The ICD maps `/dev/matrox/mtx0` seven times. Observed in one client:

| Region | Size | Role |
|---|---|---|
| 0, 1, 3 | 4096 each | small control/status pages |
| **2** | 139264 (136K) | **submit-request buffer** — the 0x28-byte records the kernel consumes |
| **4** | 3276800 (3M) | **vertex ring** (`0x320000`) |
| **5** | 688128 (672K) | **command ring** (`0xa8000`) |
| 6 | 4194304 (4M) | third heap (`0x400000`), not used per draw |

The ring sizes match the ICD's own allocation constants exactly, which is what
identifies each region. Regions 4 and 5 are addressed in lockstep: draw *n* uses
slot `n * 0x2000` in both — command packets at `0x2000*n` in region 5, vertex
data at `align64(0x2000*n) + 0x40` in region 4.

## Per-draw work — it is remarkably small

Once state is established, an entire triangle costs **three dword updates plus
the vertex data**. For `tri_flat` (the second draw in the sweep):

```
region 2  0x18 = 0x00002000     slot start
          0x20 = 0x0000201d     slot start + 0x1d
          0x28 = 0x00002040     slot start + 0x40   (= vertex data base)
region 5  0x2000 .. 0x2018      the 7-dword descriptor (below)
region 4  0x2040 .. 0x2074      3 vertices x 20 bytes
```

The next draw (`tri_gouraud`) writes the identical shape at `0x4000` and
`0x4040`, and only regions 2/4/5 change. So region 2 offsets 0x18/0x20/0x28 are
the **submission head/tail** the kernel reads.

## The per-draw command packets (region 5) — CAPTURED + CONFIRMED in the ICD

What the capture showed as "seven dwords" is **two concatenated command
packets**, confirmed against the ICD disassembly (the 64-bit `libGL.so.1.3.0`
under `opengl6/lib64/` — `opengl6/libGL.so.1.3.0` itself is the 32-bit build).

Generic packet form: `[header][payload...]`, header = `(opcode << 24) | count`.

```
+0x00  0x8f000001   DRAW packet: opcode 0x8f, count 1
+0x04  first        first vertex index in this slot's vertex array
+0x08  last         one-past-end vertex index
---- second packet ----
+0x0c  0x00000002   REGISTER-WRITE list, 2 registers
+0x10  0x21212425   index bytes 0x25, 0x24, pad, pad
+0x14  fence value  -> reg 0x1094
+0x18  fence addr   -> reg 0x1090
```

**Count semantics differ per opcode (CONFIRMED):** a draw header's low 16 bits
accumulate +2 per appended range and are decremented once at flush, so on the
wire `count = payload dwords - 1`; one range gives exactly `0x8f000001`.
Register-list counts are *not* decremented — count = number of registers.

Draw header bits: `0x03000000 | ctx[0xa040] | ctx[0xa044] | count`, where
`ctx[0xa040]` bit27 set means **sequential vertex ranges** (payload = `first,
last` pairs) and clear means a **packed 16-bit index list**. So `0x8f000000` =
"draw sequential ranges", `0x07000000` = "draw packed index pairs".

### Corrections to my first reading of this capture

Three things inferred from the capture alone were wrong; the disassembly settles
them. Recorded because the wrong readings were plausible and someone re-deriving
this from the raw capture would make them again:

| First reading | Actually |
|---|---|
| `+0x04` = a GL_POINTS flag | **first vertex index** — it was 1 because that batch had already accumulated one vertex |
| `+0x08` = vertex count | **one-past-end vertex index**; count = last - first |
| `+0x18` = vertex buffer bus address | **fence write-back address** (reg 0x1090); `+0x14` is the fence value (reg 0x1094) |

That also disposes of the "stale leading record" theory: points, strips and fans
read one higher simply because `first` was 1, not because a stale vertex was
present. The earlier guess that `+0x14`/`+0x18` are the two payload dwords for
regs 0x1094/0x1090 was correct.

`0x1090` holds the 32-bit bus address of a 4-byte completion word and `0x1094`
the value the engine writes there; the ring allocator spins on that word. Both
are per-context constants, which is why `+0x18` never varied between draws.

## Primitive type — SOLVED: register 0x10b0 (index 0x2c)

Not in the descriptor. It is emitted as its own 3-dword register packet:

```
0x10b0 = 0x00040000 | ((B[p] & 7) << 12) | (A[p] & 0x7f)
```

| primitive | p | reg 0x10b0 |
|---|---|---|
| GL_POINTS | 0 | 0x00040020 |
| GL_LINES | 1 | 0x0004103d |
| GL_LINE_STRIP | 2 | 0x0004203d |
| GL_LINE_LOOP | 3 | 0x0004203d |
| GL_TRIANGLES | 4 | 0x0004305c |
| GL_TRIANGLE_STRIP | 5 | 0x0004405c |
| GL_TRIANGLE_FAN / GL_POLYGON | 6 | 0x0004505c |
| GL_QUADS | 7 | 0x0004605c |
| GL_QUAD_STRIP | 8 | 0x0004705c |

INFERRED: bits[6:0] take only three values (0x20 point / 0x3d line / 0x5c
triangle) and look like a WARP setup-program entry address; bits[14:12] are the
topology selector; bit18 is always set.

**Why the sweep never showed a 0x10b0 write:** the ICD emits it only when the
primitive code changes *and* old and new fall in the same equivalence class
(`{0}{1,2}{3}{4,5,6}{7,8}`). Across a class boundary it emits nothing here and
the change is handled by state validation on the server side. A run of
same-primitive draws therefore carries no primitive packet at all.

GL_QUADS and GL_QUAD_STRIP are **decomposed on the CPU** into 16-bit index
lists. GL_LINE_LOOP emits the range packet plus a `0x07000001` packet carrying
`(first<<16)|(last-1)` as the closing segment.

### The 0x008 lead, resolved

Region 2 offset 0x008 was observed carrying what looked like the GL primitive
enum (points 0, lines 1, triangles 4, strip 5, fan 6), then refuted as a
dedicated field because state-only operations wrote unrelated values there.
Both halves were right. Those values are the ICD's internal **primitive code**
`p` — the same `p` that indexes the table above — which the ICD stores at
`ctx+0xa080` and, per the disassembly, never reads back itself. It reaches the
kernel through this shared region. Offsets 0x000-0x030 of region 2 are a
**submit record** whose fields are per-record-type, which is why non-draw
records put other things at 0x008.

## The submit record (region 2) — capture and disassembly agree

The ICD builds 0x28-byte submit records and hands them over with request opcode
0x1b. Aligning the disassembled layout against the captured dwords, the record
in the capture begins at region 2 offset **0x008**:

| Record field | Region 2 offset | Captured (tri_flat) |
|---|---|---|
| +0x00 | 0x008 | 4 — the primitive code |
| +0x08 u64 device/context handle | 0x010/0x014 | 0xffffc200_009b8004 (kernel pointer) |
| +0x10 START | 0x018 | 0x00002000 — chunk start |
| +0x18 END\|1 | 0x020 | 0x0000201d — note the low bit set |
| +0x20 | 0x028 | 0x00002040 |

START and `END|1` matching the captured 0x2000 / 0x201d is what pins the
alignment. The `(START, END|1)` pair is what the server/kernel side turns into
`0x404` PRIMADDRESS and `0x408` PRIMEND with its DMA-mode bits.

Discrepancy to resolve: the disassembly reads record +0x00 as a type field
(`ctx[0xa050]` = 0xd) while the capture shows the primitive code there. Either
the field is overloaded or the record base is off by one field; the START/END
match makes the rest of the alignment solid.

## Vertex format — how the stride is built (CONFIRMED)

A prefix sum over per-attribute sizes with a **fixed attribute order**; the
total is the stride. Position is always at offset 0.

| Attribute | Selector | Sizes |
|---|---|---|
| position | fmt[1:0] | {8, 12, 16, 8} (xy / xyz / xyzw) |
| diffuse colour | fmt[3:2] | {0, 12, 0, 8} |
| specular colour | fmt[7] | {0, 12} |
| texcoord 0 | fmt[6:4] | {0, 12, 4, 8, 8, 16, 0, 0} |
| texcoord 1 | fmt[10:8] | same table |
| fog / point size | fmt[30:29] | {0, 4} |
| extra 0..3 | fmt>>{16,19,22,25} & 7 | {0, 4, 8, 12, 16, 0, 0, 0} |

The captured `[x,y,r,g,b]` stride 20 is exactly `fmt[1:0]=0` (8 bytes: x,y) plus
`fmt[3:2]=1` (12 bytes: r,g,b), everything else zero. **Adding a 2-float
texcoord** sets `fmt[6:4]=3`: +8 bytes, stride 28, texcoord at offset 20.
**Adding a secondary colour** sets `fmt[7]`: +12 bytes at offset 20, pushing
texcoord 0 to 32. The stride grows; the order never changes.

## The two rings

| Ring | Size | Region |
|---|---|---|
| vertex | 0x320000 (3276800) | region 4 |
| command | 0xa8000 (688128) | region 5 |
| third heap | 0x400000 | region 6, unused per draw |

Slot size is hard-coded **0x2000**, which is the per-draw stride the capture
shows. The ring base is 16-byte aligned and each flush aligns the write cursor
**up to 64** before writing — the source of the observed `+0x40` vertex offset.
Vertex *k* lives at `align64(slotStart) + k*stride`, and the draw packet's
`first`/`last` index into that array.

Batch capacity is chosen per `(primitive, stride)` so `maxVerts*stride` fits
0x2000 and is a multiple of the primitive size — e.g. (TRIANGLES, 24) -> 339,
(LINES, 20) -> 408, (TRIANGLES, 32) -> 255.

## Scissor

If the scissor is dirty the chunk is prefixed with
`[2][0x2121a1a0][(f3<<16)|f0][(f1<<16)|f2]` -> regs **0x1280 / 0x1284**, values
clamped to +/-1024. This confirms the documented guard-band scissor encoding.

## Still open in the ICD

- Draw-header bit30 (0x40000000), gated on `ctx+0xa4f2`.
- Register 0x1104 (index 0x41), alternating 0/4 on line-strip/loop draws.
- Where the vertex-ring **base address** reaches the hardware: libGL emits only
  four register indices in total (0x2c, 0x41, 0xa0/0xa1, 0x24/0x25), so the base
  must be bound once at context creation by the server/kernel side.



## Vertex format — CONFIRMED TWICE, INDEPENDENTLY

`tri_gouraud` (`glColor3f` per vertex, positions (0.5,0.9) (0.1,0.1) (0.9,0.1),
colours red/green/blue) lands in region 4 as:

```
0x4040: 3f000000 3f666666 3f800000 00000000 00000000   0.5 0.9 | 1 0 0
0x4054: 3dcccccd 3dcccccd 00000000 3f800000 00000000   0.1 0.1 | 0 1 0
0x4068: 3f666666 3dcccccd 00000000 00000000 3f800000   0.9 0.1 | 0 0 1
```

**`[x, y, r, g, b]`, five IEEE-754 floats, stride 20 bytes.** `tri_flat` with
`glColor3f(1,1,0)` gives `1.0 1.0 0.0` in the colour slot of all three vertices,
as it must. This independently reproduces the earlier gdb capture of the ICD's
internal vertex array, by a completely different route — so the layout is settled.

Positions are the raw GL coordinates (0.5, 0.9, 0.1 …), *not* screen space: the
viewport transform happens after this buffer. z and w are not streamed; they are
pipeline constants (w = 1.0, z compared against 0.0).

## The state stream (region 2) — (index, value) pairs

Region 2's lower part is a stream of **(register-index, value) dword pairs**,
register = `0x1000 + index*4`. Proof from the capture, at byte offset 0x0d4:

```
0x0d4: 40 ffffffff  41 ffffffff  42 ffffffff ...  5f ffffffff
```

= registers **0x1100-0x117c all set to 0xffffffff**, the whole vertex/attribute
register file initialised. And at 0x0cc the pair `a6, 45000000` = register
**0x1298 = 2048.0f**. Both match the independent gdb capture exactly.

Further pairs decoded from the same run: `0x0ac: 0c,24` = 0x1030 = 0x24;
`0x0b4: 17,02` = 0x105c = 2; `0x0bc: 26,3f800000` = 0x1098 = 1.0f;
`0x0c4: 9b,00` = 0x126c = 0.

The pair stream is **not** aligned at offset 0 and is interrupted by other
record types — the buffer also holds 64-bit kernel pointers of the form
`0xffffc200_05c59004` (x86-64 vmalloc addresses on 2.6.30), which makes region 2
a control structure shared between the ICD and the kernel driver rather than a
pure GPU command buffer. A decoder must locate and validate the pair runs, not
assume alignment.

## What this means for the open driver

The submission model to reproduce:

1. Allocate the buffers and learn their bus addresses (kernel side).
2. Emit state as (index, value) pairs into the command buffer.
3. Write the interleaved `[x,y,r,g,b]` vertex array into the vertex heap.
4. Write the 7-dword descriptor naming the vertex count, sequence and vertex
   buffer address.
5. Publish the slot by updating the head/tail dwords.
6. The kernel arms the engine (0x12b8/0x10b0/0x300/0x384/0x308) and submits via
   the DMA ring: `0x404` = buffer bus address, `0x408` = `end|3` per packet,
   `0x314` = 1 to kick, then poll `0x8f0` / the `0x450` fence.

Bus addresses here are plain PCI addresses with no GART; see
docs/parhelia-3d-driving.md "Address-space caveat" for how AGP and PCIe differ.

## Buffer allocation — observed live

The buffers are **locked (pinned) page allocations**, made by the kernel driver
when the GL context is created, not preallocated at module load. When the
capture box (512 MB RAM) ran short, the driver logged:

```
[mtx] *ERROR* Parhelia OS: Failed to allocate locked pages (nb: 801, type: 2)
[mtx] *ERROR* Failed to create device buffer (size: 3280896, type: 1)
[mtx] *ERROR* Parhelia OS: Failed to allocate locked pages (nb: 2, type: 2)
```

801 pages x 4096 = 3,280,896 bytes, which is the 3 MB vertex heap (region 4)
plus one page. So region 4 is one pinned allocation of 801 pages, and the small
control pages are 2-page allocations. The `type:` field is the buffer memory
class the `GET_BUFFER` ioctl takes (VRAM / system / locked); the driver retries
a failed type 2 as type 1, which is visible above.

Consequences worth knowing before reproducing this: allocation failure is **not**
reported to the GL client as an error — the client blocks in `glFinish`
indefinitely. On a memory-constrained box, free page cache before capturing, and
do not leave killed capture processes behind, or later runs will stall rather
than fail cleanly.

## Open items

Resolved since first writing (see the sections above): primitive-type encoding
(reg 0x10b0), descriptor `+0x04`/`+0x08`/`+0x0c`/`+0x18`, and how the vertex
stride and attribute order change with texcoords or a specular colour.

Still open:

- Draw-header bit30 (`0x40000000`), gated on `ctx+0xa4f2`.
- Register 0x1104 (index 0x41), alternating 0/4 on line-strip/loop draws.
- Where the vertex-ring **base address** reaches the hardware. The ICD emits only
  four register indices in total (0x2c, 0x41, 0xa0/0xa1, 0x24/0x25), so the base
  is bound once at context creation by the server/kernel side.
- The submit-record `+0x00` discrepancy noted above.
- The full decode of the region 2 init/state stream (see
  docs/parhelia-gl-feature-map.md).
- **Texture capture is still missing.** Every attempt to capture the texture
  operations failed: the driver could not create the GL context's buffers
  ("Failed to allocate locked pages"), and this persisted across a page-cache
  drop, a module reload and a full reboot with 420 MB free and free order-10
  blocks available in DMA32 — so it is not memory pressure or fragmentation.
  Even a 2-page request fails. Cause not yet identified; the texture rows in the
  feature map are therefore uncaptured.
