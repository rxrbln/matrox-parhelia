# P690 mode-7: context, layout flag, and ordering audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The saved Linux and Windows drivers contain no extra mode-7 arm, cache
invalidation, or context-creation latch between their complete direct-fetch
state and the three-dword indexed packet.  Both stacks reduce the emission to
the same state object and exactly this packet:

```
header = 0x03000001 | context_header_flags | index_size_mode
start  = index_object_gpu_base + byte_offset
end    = start + index_count * element_size | memory_class
```

For 32-bit indices, `index_size_mode = 0x1c000000`; with the ordinary
header flags this is `0x9f000001, start, end`.  The P690 mode-7 screen
experiment already emitted the complete vendor-shaped stream snapshot and
still timed out.  Therefore an omitted stream register, `0xdf` layout flag,
or an undocumented state/fence packet is not evidence-based explanation for
that hang.

The remaining strong distinction is **memory transport**: the test makes both
the direct vertex stream and the immutable index snapshot class-0 local VRAM,
while the known good renderer's command and normal vertex stream are class-3
DMA32 system memory.  This is a testable hypothesis, but it is not a reason to
make native indexed draws public yet.

## Independent builder evidence

### Linux HAL

The unstripped Linux module's direct state builder is `mtx1035` at
`.text+0xb940`.  With its direct-fetch argument non-zero, instructions
`0xbaf0..0xbb35` set the stream base to

```
mtx2320(buffer) + (stream_byte_offset & ~3)
```

and set the selected 16-bit byte stride.  It also computes the vertex ordinal
floor and limit at `0xb9ee..0xba20`.

The state emitter `mtx3355` at `.text+0x417a0` consumes those fields.  Its
stream-dirty path at `0x41910..0x419a1` emits, without an intervening
strobe, cache command, or draw-specific MMIO operation:

```
0x0000000e,
0x03020100, base0, base1, base2, base3,
0x07060504, base4, base5, base6, base7,
0x13121110, stride01, stride23, stride45, stride67
```

It then emits the primitive, ordinal floor/limit, descriptor-index reset, and
descriptor list through the ordinary dirty-state paths.  The state constructor
`mtx2920` at `0x41710..0x4178c` initializes all eight bases and strides to
zero.  Thus explicit zeroes for unused streams in the first single-stream
test are vendor-consistent.  The `0xbaa0` poison branch in `mtx1035` is
only used if a *declared* stream record has no buffer; it is not the initial
state of omitted streams.

For the index side, `mtx2661` at `0xbf50..0xc02b` chooses
`ctx+0x428 = 0x1c000000` only when element size is four, calls
`mtx2320(index_buffer)`, computes `end = base + offset + count*4`, and
returns only start/end.  `mtx1443` at `0x5dc4..0x5e01` ORs
`ctx+0x428 | ctx+0x42c | 0x03000001` and writes the three dwords.  It does
not write any register before or after the packet.

### Windows PARD

The 32-bit Windows driver independently reproduces the same split:

* `MTXPARD.dll 0x12df80..0x12e111` builds direct stream bases/strides and
  the bounds.  Its missing-stream branch at `0x12e114..0x12e13d` writes the
  same diagnostic `0xbad0bad0` / `0xbad0` values as Linux, while its context
  constructor at `0x12e370..` initializes the stream records to zero.
* `0x12e150..0x12e1fa` selects `0x1c000000` for four-byte elements and
  returns only the start/end address pair.
* The two packet writers at `0x132920..0x132988` and
  `0x132990..0x1329ff` reserve 12 bytes and emit

  ```
  [context+0x37c] | [context+0x378] | 0x03000001,
  start,
  end
  ```

  The latter differs only by immediately flushing the ordinary command
  accumulator after the write.  Neither writer emits a cache operation,
  attribute write, mode-specific strobe, or hidden base register.

That agreement excludes an otherwise plausible failure mode: a Linux-only
compiler convention or an un-emitted Windows helper immediately surrounding
mode 7.

## The 0xdf header is not a five-dword XY+RGB predicate

The candidate header bit is bit 30.  Linux `mtx3712` at
`0x18b72..0x18b83` derives `ctx+0x42c` solely from bit 4 of the layout
byte: clear produces `0x80000000`, set produces `0xc0000000`.  It applies
that word to **every GL-path draw opcode**, including direct vertex packets,
16-bit indices, and 32-bit indices.  It is not calculated from the descriptor
port values or the stream stride.

The independent ICD analysis identifies the same input as its
quad-decomposition/polygon-offset condition.  In
`docs/parhelia-vertex-submission.md` §2.3, the state pass sets that condition
for the polygon-mode rule, then sets draw-header bit 30.  The known normal
five-float layout uses

```
0x20c00000, 0xa1c50020, 0
```

under the normal bit-30-clear header (`0x9f000001` for mode 7).  It is also
the descriptor pair used by the successful non-indexed `0x97080001` P690
path.  There is no saved-driver data that permits setting `0xdf000001` for
the isolated XY+RGB test, and doing so would couple a polygon-offset layout
condition to an unrelated descriptor.

## Ordering and completion

The vendor order for a fresh direct fetch is:

```
complete bases/strides
primitive
vertex floor, vertex limit
descriptor index = 0
descriptor words, terminator
mode-7 three-dword packet
normal batch completion / submission
```

Both implementations emit it through the same secondary command accumulator.
The Windows immediate variant calls its normal accumulator flush after the
packet; the Linux higher-level path ultimately uses `mtx1937`, which appends
a primary CALL and rings the usual producer doorbell.  There is no mode-7-only
cache command in either path.

A command *buffer* that itself resides in local VRAM needs the documented
`0x318/0x31c` memory-controller readiness poll before its doorbell.  That
rule is irrelevant to this test: its primary and secondary command buffers
remain coherent DMA32/class 3.  It does not justify inserting a cache packet
between the class-0 vertex/index data and the mode-7 draw.

## What the timeout now proves and does not prove

The two staged hardware attempts reached and stalled at the final indexed
CALL, including the later stream snapshot attempt:

```
submit timeout at CALL 31/31:
BM=03000000 prim=d7efa274/d7efa27b sec=00000000/00000000
fence=003a1e90 want=003a1e98; wedged
DMA status450=fbefdf0f want=00000008 writeback=00000007
DWGCTL=00a60ec4 908=638fa047 924=00201002
```

The validator supplied the normal five-dword descriptor extent, floor zero,
limit four, a complete single-stream snapshot, and the sealed index range
`{0,1,2,1,3,2}`.  That excludes the most direct packet framing, descriptor
extent, uninitialized-unused-stream, and index TOCTOU explanations.  It does
not prove that the P690 accepts the particular class-0 direct-fetch allocation
route.

The safest next experiment is not `0xdf` or a guessed latch.  Build a
separate, capability-disabled mode-7 diagnostic in which the kernel owns
persistent DMA32/class-3 stream and index buffers, copies both inputs into
them, validates the same ordinal/bounds contract, and emits the vendor's
class-3 addresses in the complete state snapshot and index end operand.
Keep the existing local-VRAM test unchanged as a separate class-0 case.  This
isolates the memory-class question without giving userspace physical addresses
or weakening the private immutable-index lifetime rule.

Only a retired fence plus a pixel-for-pixel comparison against the 0x97
reference can enable either path.  Until then `PARHELIA_PARAM_NATIVE_INDEXED`
must remain zero.

