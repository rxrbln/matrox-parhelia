# M9 CFG scheduling, selector lifetime, and linked state image

This note closes the vendor compiler's graph scheduling and physical-selector
lifetime mechanics and recovers one exact 0x100-byte image inside the Linux
linked state. It also marks the remaining join needed for a fresh fixed-function
first triangle. Addresses are virtual addresses. `[BIN]` marks direct static
binary evidence and `[DER]` a conclusion from it.

The pinned inputs are:

* Linux `m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* Windows `MSerX/MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

`tools/m9-cfg-linked-image-evidence.py` pins the identities, instruction
ranges and call graph described below. Its small Python models exercise the
recovered scheduler and lifetime rules without executing vendor code.

## Graph and work-item layout

Windows compiler pass `0x1d1d40` stores graph nodes at `compiler+0x20` with an
exact `0xc4`-byte stride [BIN]. Fields used by the scheduler are [BIN]:

| node offset | meaning at this boundary |
|---:|---|
| `+0x04` | graph operation/type |
| `+0x0c` | primary typed descriptor |
| `+0x1a` | pending dependency count, 16 bit |
| `+0x26` | minimum observed schedule level, 16 bit |
| `+0x28` | maximum observed schedule level, 16 bit |
| `+0x30` | special predecessor/mapping node index, 16 bit |
| `+0x68` | inline edge count |
| `+0x6c` | first of six inline 0x0c-byte edge records |
| `+0xb8/+0xbc` | overflow edge-vector begin/end |

The edge helpers at `0x1a2d50` and `0x1a2cf0` return the 16-bit values at edge
`+0x08` and `+0x0a`. Inline and overflow records use the same 0x0c-byte
layout. Iteration uses `ordinal + 1 < total`, excluding the final sentinel
edge [BIN].

The pass's pending queue consists of 0x28-byte work items. It clones them with
that stride at `0x1d2384..0x1d23a5`; it separately clones the graph with the
0xc4 stride at `0x1d23c3..0x1d23ea` [BIN]. The two layouts must remain
separate in a clean compiler.

## Exact dependency release and reconvergence rule

Helper `0x1cfc30(scratch, compiler, node_index, level)` implements the graph
release operation. Its body is pinned in full. It gathers the first and second
endpoint of every non-sentinel adjacency record into two 16-bit vectors, then
uses the second-endpoint vector as a reconvergence set [BIN]. In pseudocode:

```text
seconds = set(all edge.second values other than ffff)

for value in all edge.first values, retaining order and duplicates:
    if value == ffff or value is in seconds:
        continue

    child = graph[value]
    if child.pending != 0:
        child.minimum = min(child.minimum, level)
        child.maximum = max(child.maximum, level)
        child.pending--

    append value to the released-work vector
```

Membership in `seconds` suppresses release for the corresponding first
endpoint, which is the exact reconvergence behavior. The append remains
outside the nonzero-pending block: a zero-pending endpoint is still returned
to the caller [BIN]. Duplicate first endpoints are not deduplicated at this
stage [BIN].

The main pass calls this helper at `0x1d3362` only after the current work item
has converted successfully. The returned/released nodes feed later work-list
growth. Allocation failure is represented by descriptor `0xf0000000`; the
pass records that condition and leaves the work item on the retry path instead
of releasing its dependents [BIN][DER].

## Roots, topology, and schedule intervals

The pass scans at most 32 root keys. A graph node whose primary descriptor is
`0xfffe` becomes the special root; descriptors `0..0x20` populate the indexed
root table. It then makes one initial work item for each present root [BIN].
This is a multi-root graph scheduler and does not assume a linear instruction
stream [DER].

Helper `0x1d7e60` combines a requested level with node `+0x26/+0x28` [BIN]:

```text
lo = min(requested, node.minimum)
hi = max(requested, node.maximum)
if hi < lo: return 0
if hi == lo: return hi
return hi + 1
```

The main pass calls it for both endpoints at `0x1d3158` and `0x1d3182`. It
rejects reuse when either returned bound exceeds the current scheduling level
[BIN]. Thus graph release determines readiness, while the accumulated
inclusive interval determines whether a physical selector can remain live
across that edge [DER].

Together, the root table, sentinel-aware adjacency walk, reconvergence set,
pending count, interval accumulation, finite allocation and failure retry
close the vendor's structural scheduling algorithm for arbitrary graph
topology [DER]. The D3D lowerers' semantic choice of every typed descriptor is
still a separate frontend problem. No scheduler path performs PC-relative
byte arithmetic [BIN].

## Physical-selector reset, reuse, and retirement

Context initializer `0x1de2d0` resets the selector compiler in place [BIN]:

```text
u16 +04 = 0x40
u16 +06 = 0x0c
u16 +08 = 0x20
u16 +0a = 0x10
```

It reconstructs the helper groups at `+0x0c`, `+0x1c0`, `+0x250`, `+0x37c`
and `+0x3c0`; reinitializes vectors at `+0x404/+0x414/+0x424/+0x434/+0x444`;
and writes `0xf0000000` to `+0x454/+0x458` [BIN]. Context initialization is
therefore the reset boundary. Exhaustion returns the sentinel; none of these
allocators wraps [DER].

The 23-slot C-family allocator has keys `+0x00..+0x5b`, allocated mask
`+0xe8`, current mask `+0xec`, owner-valid mask `+0xf0`, 16-bit owners at
`+0xf4`, and quarantine mask `+0x124` [BIN]. Lookup `0x1e2d60` scans slots
0..22 and matches `(key ^ source) & 0xf000ffff == 0`. Scheduling allocator
`0x1e3040` chooses a miss slot only when it is absent from allocated, current,
and quarantine masks. It emits [BIN]:

```text
(source & 0xfc000000) | (slot + 8)
```

Retire helper `0x1e3130` clears the current, allocated and owner-valid bits,
sets owner to `ffff`, writes key `f0000000`, and sets the quarantine bit
[BIN]. Quarantine therefore forbids reuse during the current scheduling
attempt. Ordinary allocator `0x1e2f50` does not consult quarantine, allowing
the same physical namespace to be reused after that attempt [DER].

The A/B families share a four-slot layout: allocated `+0x28`, current `+0x2c`,
owner-valid `+0x30`, owners `+0x34`, quarantine `+0x3c`. A allocator
`0x1e34b0` returns low selector `slot`; B allocator `0x1e3890` returns
`slot+4`. Retire helpers `0x1e3590` and `0x1e3970` apply the same clear and
quarantine rule. Helper `0x1e3680` can migrate a live B-family allocation to a
requested free physical selector while preserving occupancy and owner state
[BIN].

The type-9 allocator at `0x1df0c0` searches 0x1c-byte entries in vector
`+0x424` with the same `0xf000ffff` key mask. On a miss it decrements the
16-bit `+0x0a` counter and appends `{source, replacement, 0, 0, 0, 0}`. The
low selectors therefore descend from `0x2f` to `0x20`. A zero counter returns
`0xf0000000`; there is no per-program retire or wrap. Reinitializing the
compiler context reconstructs the vector and restores counter 16 [BIN][DER].

## Exact 0x100-byte image inside the linked object

Linux state compiler `m9x18211` operates on embedded pipeline state at
`pipeline+0x150` and always calls `m9x10392` at `0x3d3a03` before clearing the
state dirty word [BIN]. Within that state object, `m9x12620` constructs the
linked nested object at `state+0x1fd`. Therefore `state+0x2d6` equals
`linked+0xd9` [DER]. Constructor `m9x31054` independently zeros exactly 0x100
bytes at `linked+0xd9`, confirming the identity and extent [BIN].

`m9x10392` derives six sub-states, zeros the complete image, and calls six
serializers in fixed order [BIN]:

| final image bytes | final writer |
|---:|---|
| `00..04` | `m9x23548` |
| `08..44` | `m9x29069` |
| `48..6c` | `m9x14822` |
| `70..74` | `m9x6390`, overwriting the earlier range owner |
| `78..84` | `m9x14822` |
| `88..8c` | `m9x23548`, overwriting the earlier writer |
| `90..a4` | `m9x14822` |
| `a8..f4` | `m9x6390` |
| `f8..fc` | `m9x1064` when source `+0x3c != 0`; zero otherwise |

The fourth serializer overwrites `m9x14822` at `+0x88/+0x8c`. The final values
are therefore `+0x88 = 0` or `0x3f000000` according to state `+0x1f5`, and
`+0x8c = 0x437f0000` [BIN]. Other fixed values include
[BIN]:

```text
a8/ac 38800006    b0/b4 3ffff000    b8/bc 47800000
c0/c4 477fff00    c8/cc 3b800000    d0/d4 40800000
d8/dc 37800006    e0/e4 3ffff088
```

`m9x6390` writes four normalized color components at `+e8..+f4`.
`m9x11020`, the sixth serializer, is a return-only no-op [BIN].

After construction, `m9x10392` compares all 0x100 bytes with the shadow at
`state+0x7e8`. A difference increments device generation `+0x20`, sets state
dirty bit `0x10`, saves the generation at state `+0x2d2`, and copies all
0x100 bytes to the shadow [BIN]. The image is therefore a coherent,
generation-tracked state subrecord rather than incidental padding [DER].

This closes the byte constructor for that 0x100-byte linked-state image. It
does **not** prove that the host bytes are copied verbatim into the allocation
selected by command index `6c`. Backend translator `m9x11630` reads the linked
object's resource descriptor/object at `+0x301/+0x309`, but does not read the
`+0xd9` image. `m9x21983` copies that pair into a separate `m9x31420`
resource/link state, while final `m9x2427` consumes the independently owned
pair in an `m9x18369` program state [BIN]. All eight invariant dwords from the
host image are absent even individually from every complete saved `6c` system
allocation [CAP]. `parhelia-m9-linked-record-final-join-boundary.md` pins this
ownership split and proves that no verbatim copy occurs.

## Terminal operations and first-triangle gate

Windows finalizer `0x1c8590` remains the exact execution-bound evidence. It
emits result-only `fffc7e1d:a2803f0f`, kill-only
`3ffcfe1e:ac800c3f`, or the two-slot kill-plus-result suffix
`3ffcf81e:ac800c3f, fffc7e1c:a2803f1f` according to the two positive counters
[BIN]. Neither compiler slot count nor aligned upload size is submitted as an
execution bound [BIN]. Static evidence still cannot distinguish an endpoint
selector from full terminal-operation semantics [DER].

The fixed-function first-triangle executable gate is therefore narrower now:
graph scheduling and selector lifetime are closed, and the linked object's
0x100-byte state image is byte-exact. The remaining blocker is the semantic
serializer that constructs the heterogeneous, variable-size 0x80-aligned GPU
allocation addressed by selector `6c`. The bounded hardware-write probe in
`tools/m9x-linked-record-final-join-gdb.py` stops at that writer on the first
baseline record. Until it is captured, treating `linked+0xd9` as the final
record base would be an unsupported shortcut.

## Reproduction

From the repository root:

```sh
python3 tools/m9-cfg-linked-image-evidence.py
```

The verifier uses only Python's standard library and performs static reads.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
