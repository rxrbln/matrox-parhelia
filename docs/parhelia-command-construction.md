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
| **2** | 139264 (136K) | **command + state buffer** and the submission head/tail |
| **4** | 3276800 (3M) | **vertex data heap** |
| **5** | 688128 (672K) | **per-draw command descriptors** |
| 6 | 4194304 (4M) | untouched by these draws (texture/surface heap) |

Regions 4 and 5 are addressed in lockstep: draw *n* uses slot `n * 0x2000` in
both — descriptor at `0x2000*n` in region 5, vertex data at `0x2000*n + 0x40` in
region 4.

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

## The per-draw descriptor (region 5) — CAPTURED

Seven dwords at the slot base:

| Offset | tri_flat | tri_gouraud | points | lines | Reading |
|---|---|---|---|---|---|
| +0x00 | 0x8f000001 | 0x8f000001 | 0x8f000001 | 0x8f000001 | constant header/opcode |
| +0x04 | 0 | 0 | **1** | 0 | set only for GL_POINTS |
| +0x08 | 3 | 3 | 3 | **2** | **vertex count** |
| +0x0c | 2 | 2 | 2 | 2 | constant in all observed draws |
| +0x10 | 0x21212425 | 0x21212425 | 0x21212425 | 0x21212425 | register-index tag: bytes 0x25,0x24,0x21,0x21 -> regs 0x1094, 0x1090 (0x21 = pad) |
| +0x14 | 3 | 4 | 6 | 7 | **sequence number**, +1 per draw |
| +0x18 | 0x05f58000 | 0x05f58000 | 0x05f58000 | 0x05f58000 | bus address of the vertex buffer |

`+0x08` tracking the vertex count across GL_TRIANGLES (3) and GL_LINES (2) is
the clearest single confirmation that this is the draw descriptor.

Note that triangles and lines produce otherwise identical descriptors, so the
**primitive type is not encoded here** — it must live in the state stream or in
the 0x1090/0x1094 pair the tag names. Resolving that is an open item.

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

- Where the primitive type (triangles / lines / points / strip / fan) is encoded.
- Descriptor `+0x0c` (constant 2) and the exact meaning of `+0x04`.
- How the vertex stride and attribute order change when texture coordinates or
  a specular colour are added.
- The full decode of the region 2 init/state stream (see
  docs/parhelia-gl-feature-map.md).
