# P690 native indexed-draw feasibility

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Current status, 2026-10-04

The proposal below is historical, not the current ABI inventory. The current
private UAPI includes stream/index relocations and a mode-7 diagnostic. Mesa
still does not expose native indexed acceleration: `PARHELIA_PARAM_NATIVE_INDEXED`
remains zero. Retained P690 class-0 VRAM and class-3 coherent diagnostics both
stalled at the final primary CALL. No successful P690 vendor mode-7 runtime
capture is retained. Static vendor packet construction is insufficient proof
of a working submission. See the newer
[actual vendor-path audit](parhelia-p690-mode7-actual-vendor-path-audit.md).

The current GPU WARP transform path also supersedes the description below
that every draw uses software transforms. It expands source indices and packs
vertices for the verified address-mode packets, with safe two-triangle
batching. Native index reuse and bound strided fetch remain performance gaps.

Next work must derive the missing direct-fetch state/lifecycle from vendor
execution or a matching capture before enabling production mode 7. Do not
repeat speculative packets based only on the historical ABI proposal.

## Historical feasibility result

**Native indexed drawing is supported by the Parhelia hardware and is a
credible high-value Q3 optimization, but it is not reachable through the
current DRM ABI or Mesa path.**  It needs a deliberately narrow ABI extension
for a `CREATE_VERTEX`-owned local-VRAM vertex/index allocation, followed by a
new Mesa fast path.  It must not be enabled from arbitrary GEM objects,
userspace addresses, or before a standalone indexed-triangle hardware test.

The current path invokes Gallium's software draw module, turns each indexed
triangle into three transformed vertices, and emits one `0x97` draw packet per
triangle.  It therefore does not exercise this hardware feature at all.

## Packet evidence

Clean-room disassembly of the retained vendor Linux GL/kernel binaries and
the Windows display driver agrees on the draw-mode field.  The full derivation
is in [parhelia-indexed-draws.md](parhelia-indexed-draws.md); the relevant
wire packets are:

| operation | three-dword packet | operands |
|---|---|---|
| direct vertex bytes | `0x97000001 | layout` | byte `start`, exclusive byte `end` |
| direct 16-bit indices | `0x9b000001 | flags` | index byte `start`, exclusive rounded `end` |
| direct 32-bit indices | `0x9f000001 | flags` | index byte `start`, exclusive `end` |
| sequential bound-stream range | `0x8f000001 | flags` | first vertex, exclusive end vertex |

For the Q3 `GL_UNSIGNED_INT` case the exact first test packet is:

```
0x9f000001, index_gpu_offset, index_gpu_offset + 4 * index_count
```

The final operand carries the memory class in its low two bits.  A
`CREATE_VERTEX` local-VRAM object is class 0, so both operands are aligned and
the final operand's low bits are zero.  There is no index-count or
index-buffer-base register: count comes from `(end - start) / 4`.

Hardware fetches vertices from `0x1000..0x101c` (up to eight stream bases),
with byte strides in `0x1040..0x104c`, addressability limit in `0x1060`, and
floor in `0x1064`.  Attribute descriptors are supplied through
`0x10b4/0x10b8`.  Vendor code proves that a stream base is
`gpu_offset | memory_class`; direct indexed packets select elements from those
streams.  Start with one interleaved stream: vendor Linux and Windows drivers
disagree on multi-stream descriptor offsets, while the single-stream meaning
is unambiguous.

The vendor GL capture corpus contains only `0x97` packets because its client
array fallback de-indexes on the CPU.  This is evidence of the vendor GL
fallback, not a lack of hardware indexed support.  The Windows D3D driver
independently emits the same mode-6/mode-7 constants and address arithmetic.

## Why the current driver cannot submit it

The render-node UAPI accepts header-ful secondaries, but
`parhelia_validate_packet()` only accepts opcode `0x00`, `0x41`, and `0x97`.
`PARHELIA_RELOC_DRAW` and `PARHELIA_RELOC_DRAW_VRAM` likewise only accept a
`0x97` packet.  The latter changes the final stream-class-3 bind to class 0,
but it cannot install the non-zero local-VRAM stream base required by index
fetch.  The existing state stream base is just `3`, appropriate for a `0x97`
packet whose operands name the vertex bytes.

Mesa currently maps buffers into `draw_vbo()`, calls software `draw`, and
`ph_stage_tri()` copies `prim->v[0..2]` into its transient packed array.
`parhelia_batch.c` then makes one draw relocation per triangle.  Thus there
is neither a resident index buffer nor a source vertex stream available at
the point where the kernel sees the submission.

## Proposed render-node interface

Extend the existing *private, P690-only* `CREATE_VERTEX` allocation model;
do not expose GPU addresses or make generic VRAM/render objects fetchable.

Add two relocation types, with the same `{ packet, dword, handle, delta }`
shape as the existing range relocation:

```
PARHELIA_RELOC_VERTEX_STREAM_VRAM = 3
PARHELIA_RELOC_INDEX_VRAM         = 4
```

`VERTEX_STREAM_VRAM` names the data word of an index-0..7 stream-base write
in a regular `0x00` state packet.  The kernel verifies that `handle` is a
live `CREATE_VERTEX` object owned by this render file, `delta` is 4-byte
aligned and inside it, and replaces the zero placeholder with
`vram_offset + delta` (class 0).  A first implementation accepts **only
stream 0**, with a non-zero 4-byte-aligned offset, and validates one matching
stride, `0x1060`, `0x1064`, and descriptor set in the same submission.

`INDEX_VRAM` names a `0x9b` or `0x9f` packet.  `delta` is the byte offset of
the first index in the same file-owned object and the relocation's byte range
must be represented explicitly (add `length` to a versioned relocation
structure, or define `dword` as offset and `delta` as length for this type,
as `RELOC_DRAW` already does).  After checked addition, the kernel writes:

```
cmd[1] = object_vram_offset + index_offset
cmd[2] = cmd[1] + index_bytes              // class 0, exclusive
```

For the initial mode-7-only interface require `index_offset % 4 == 0`,
`index_bytes != 0`, `index_bytes % 4 == 0`, and a triangle-list count divisible
by three.  Mode 6 can wait: its halfword skip bits make the command arithmetic
more complex without helping Q3.  The index and vertex data may occupy
non-overlapping ranges of one `CREATE_VERTEX` allocation, or two separately
owned `CREATE_VERTEX` objects if the relocation form gains a second handle.

The kernel must also: (1) accept only the exact three-word `0x9f` framing,
(2) reject a submission mixing host-DMA `RELOC_DRAW`, old `RELOC_DRAW_VRAM`,
and indexed relocation types, (3) ensure every indexed packet has exactly one
index relocation, (4) reject unrelocated `0x9f` packets, and (5) retain the
existing kernel-owned primary ring, writeback, EOB and fence generation.
The current synchronous completion rule remains necessary until independently
owned in-flight vertex/index allocations and fences are implemented.

## Mesa fast-path boundary

A valid Mesa implementation is not a packet substitution in `ph_stage_tri()`.
That stage receives **software-transformed** output vertices.  The fast path
must either:

1. retain the software-transform result in a persistent, single-interleaved
   P690 vertex allocation plus a 32-bit index allocation, or
2. program the proven transform WARP path and constants before fetching the
   application's source arrays.

Option 1 is the safe first milestone.  It retains existing GL correctness,
builds one interleaved transformed vertex array for eligible
`PIPE_PRIM_TRIANGLES` draw calls, copies the original/rebased 32-bit indices,
then emits one state packet and one `0x9f` packet rather than `N/3` `0x97`
packets.  It needs explicit invalidation for vertex buffers, element buffers,
vertex elements, shaders, framebuffer state, clipping, and base-vertex
changes.  It must fall back for line/point primitives, primitive restart,
indirect/multi draws, CPU alpha paths, unsupported shader outputs, and any
multi-stream configuration until separately tested.

Option 2 avoids transformed-vertex materialization but is larger work: it
requires full equivalence with Gallium's current software vertex transform,
including matrices, clipping, fog, colour conversion, and the Q3 two-texture
coordinate path.  Do not use it as the first hardware indexed test.

## Expected effect and limits

One state-compatible Q3 `glDrawElements` submission can collapse from up to
180 per-triangle `0x97` secondary CALLs (the current `PH_BATCH_MAX_DRAWS`) to
one indexed draw CALL.  It also removes the threefold de-indexed vertex upload
and the `ph_stage_tri()` packing loop.  That directly targets the current
submit-bound workload, whose target sample spends nearly all runtime in
kernel submit/fence polling.

It will not by itself remove the synchronous ioctl/fence boundary between
state-incompatible GL draws.  Therefore it can substantially reduce work per
submission, but an FPS estimate needs measurement after a safe prototype;
it is not evidence that asynchronous queueing is correct.

## Safe bring-up sequence

1. Add a kernel self-test that accepts a single class-0 stream-base relocation
   and a single mode-7 index relocation, rejects all malformed ranges and
   preserves the existing `0x97` tests.
2. Add a standalone P690 test with a four-vertex interleaved local-VRAM
   buffer, six 32-bit indices, one `0x9f` packet, known WARP passthrough, and
   a hash/readback comparison against the existing two-triangle `0x97` test.
3. Stress offset/range boundary checks and repeated submits; use only the
   staged `/tmp/parhelia-stage` module, never `/lib/modules`.
4. Integrate Mesa's narrow single-stream Q3 fast path behind an opt-in debug
   environment variable; compare images and submit counts before making it
   default.
5. Only then consider multi-stream, 16-bit index, source-array WARP, or
   asynchronous submission.

