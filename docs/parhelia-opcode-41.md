# Opcode 0x41: four different things with the same number

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Three documents in this repository gave `0x41` three incompatible meanings, and
the reconciliation is that they were all describing different objects that
happen to share the number.  Recorded because the collision is easy to
re-introduce.

## 1. The generic command-packet header

    dword0 = [31:24] opcode | [22:17] count | [15:0] totalDwords - 2

Verified against opcodes 0x00, 0x03, 0x17 and 0xd7 (`mtx1017` @0x675e,
`mtx3063` @0x70a3, `mtx1385` @0x4a453), so this is the packet shape in general,
not something specific to 0x41.

`count` is **(16-byte groups after the header) − 1** and is clamped to 63 at
`0x901d9`/`0x901fd` because the field is six bits.  Both known 0x41 packets fit:

| packet | dword0 | count | payload |
|---|---|---|---|
| block load, 1 vec4 | `0x41020007` | 1 → 2 groups | 4 descriptor dwords + 4 data = 8 |
| vertex, 16 attrs | `0x4106000f` | 3 → 4 groups | 16 data dwords, **no descriptor** |

Note the vertex packet carries no descriptor block.  A block load does.  They are
distinguished by the count/length fields, not by the opcode.

## 2. Three different emitters of an 0x41 packet

| emitter | address | what it sends |
|---|---|---|
| `mtx1342` | `.text+0x90010` | the block/constant load; payload copied from `blockbase + 0x230 + index*0x10` |
| `mtx1814` | `.text+0x43140` | a **caller-preformatted body** — used for program blobs |
| `mtx1185` | `.text+0x435d0` | same encoding, differs only in sub-header dwords 1 and 3; **no in-object caller**, exported for the DDX side |

`mtx1814`'s length form reconciles with `mtx1342`'s exactly when
`ndw = 4*(N+1)`, i.e. the caller's blob is `[4-dword sub-header][N × vec4]`.
That the sub-header carries the same meaning in both is **inference**, not
observed.

## 3. A token verb on a shader object (this is reading "(b)")

`0x40`..`0x43` are four adjacent verbs on one object type, dispatched through
the token table at `.rela.rodata+0x1b08 + 8*token`:

| token | verb | handler |
|---|---|---|
| `0x40` | create / realize | `0x45815`, via `mtx2166` @0x8ac40 |
| `0x41` | **destroy** | `0x45628` |
| `0x42` | bind | `0x455dc` |
| `0x43` | set constants | `0x45462` |

This is why `0x41` looked like "vertex-shader object allocation" — it is the
neighbouring verb to one, and it is a **destroy**, not an allocate.

**The `handle = 2*id+1` claim is withdrawn for this binary.** An exhaustive
search for that arithmetic shape (`lea r,[r+r*1+1]`, `lea r,[r*2+1]`, `shl r,1`,
`add r,r` + `inc`/`or 1`) finds nothing of the kind; the `2v+1` sites that exist
are SNORM16 dequantisation in `mtx3159` and a 2-bit field select.  What is real
is **bit 0 of the object id used as a type tag** (`test sil,0x1` @0x45602,
`and ebp,0x1` @0x8ac58) with only odd ids routed to the program/constant path.
If a `2*id+1` handle exists it is computed in `mtx_drv.so`, which has not been
examined.

## 4. A hardware register index

`0x41` is also simply register index 0x41 = MMIO `0x1104`.  `mtx1412` @0x7ba79
returns `0x41` from an enum→index map, and the literal `0x21444140` at `0x3c3e9`
packs indices `0x40`, `0x41`, `0x44` and the terminator `0x21` into one command
dword.  Same command buffer, different byte position, unrelated namespace.

## Consequence for the draw sequence

`mtx3400` emits **four** 0x41 uploads per state flush, not one: three program
blobs through `mtx1814` (`0x441e4` under dirty bit 0x04, `0x44224` under 0x02,
`0x44261` under 0x10) plus the incremental constant upload through
`mtx693`→`mtx1342` under dirty 0x20 — **all before** the vertex-format and entry
writes.

That matters for a from-scratch driver: the vendor delivers program images as
0x41 packets in the command stream, not only through the `0x918`/`0x91c`
streaming port.
