# P690 mode-7 timeout: direct-fetch state audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope and result

The first private, immutable-index mode-7 screen test reached the P690 but
stalled at the indexed draw.  This note compares that test's state with the
saved vendor direct-fetch path.  It identifies one concrete state discrepancy
that must be removed before another hardware attempt.  It does not claim that
this alone explains the stall.

The failed test submitted one class-0, 20-byte interleaved vertex stream,
`0x1064 = 0`, `0x1060 = 4`, and

```
0x9f000001, index_vram_start, index_vram_start + 24
```

for the six ordinals `{ 0, 1, 2, 1, 3, 2 }`.  The kernel had copied and
validated those ordinals into a private, page-aligned local-VRAM object.  Its
post-timeout report was:

```
submit timeout at CALL 28/28:
BM=03000000 prim=35c40238/35c4023f sec=00000000/00000000
fence=00000058 want=00000060; wedged
DMA status450=fbefdf0f want=00000008 writeback=00000007
DWGCTL=00a60ec4 908=638fa047 924=00201002
```

Thus this is not an index-range, handle-ownership, user-mapping race, or
allocation-size rejection.  The engine accepted the primary chain far enough
to stop retirement at the final indexed CALL.  The staged driver now reports
`PARHELIA_PARAM_NATIVE_INDEXED = 0`; do not route Mesa or Q3 to this packet.

## What the vendor actually emits for direct fetch

The unstripped Linux HAL gives an exact, executable answer.  `mtx1035`
(`mtx_parhelia.o` `.text+0xb940`) builds direct-fetch state when its third
argument is nonzero:

* at `0xbaf0..0xbb35`, for every bound stream, it writes
  `state[0x08 + 4*slot] = mtx2320(buffer) + (offset & ~3)` and
  `u16(state[0x28 + 2*slot]) = stride`;
* at `0xb9ee` it divides `(size - offset)` by the byte stride, reducing the
  result into `state[0x3c]`; at `0xba04..0xba20` it reduces the floor into
  `state[0x38]`.

The following vendor routine, `mtx3355` at `.text+0x417a0`, writes the state.
When the stream dirty bit is present, its disassembly at `0x41910..0x419a1`
reserves exactly 16 dwords and writes this one packet:

```
0x0000000e,
0x03020100, base0, base1, base2, base3,
0x07060504, base4, base5, base6, base7,
0x13121110, stride01, stride23, stride45, stride67
```

`0x41933..0x4199e` loads all eight bases from offsets `0x08..0x24` and all
four stride pairs from `0x28..0x34`; it does not emit a two-register shortcut.
The same routine then emits, in order selected by dirty bits:

```
0x00000001, 0x2121212c, primitive_word       # 0x10b0
0x00000002, 0x21211819, floor, limit         # 0x1064, 0x1060
0x00000001, 0x2121212d, 0                    # reset 0x10b4
0x00000004, 0x2e2e2e2e, descriptor...        # write 0x10b8
```

The address-mode index emitter is independent of this state setup.
`mtx1443` at `0x5dc4..0x5dfd` asks `mtx2661` for an index interval then emits
only `[0x9f000001 | flags, start, end]`.  `mtx2661` at `0xbf7c..0xc02b`
constructs the class-tagged u32 interval.  There is no hidden index base,
count, or enable register that the test omitted.

Windows agrees on the important division: `MTXPARD.dll` `0x12df80..0x12e111`
prepares the stream/descriptors, and `0x12e1c7..0x12e1f9` makes the three-word
mode-7 packet.  The packet header and operand arithmetic are therefore
well-established; the unproven part is reproducing the direct-fetch state on
our own context.

## Difference in the failed test

`parhelia_batch_mode7_screen_test()` currently calls `p3d_emit_state()`, then
adds only this final stream packet:

```
[tag 0x21211000] stream0_base, 20
```

where the kernel relocates `stream0_base` to the local-VRAM vertex object.
It had already emitted the primitive, floor, limit and descriptors in the
ordinary state stream.  It never writes stream bases 1--7 or packed stride
pairs 1--3 in this direct-fetch setup.  This differs from the recovered
vendor state emitter in two observable ways:

1. the vendor always transmits a complete eight-base/four-pair snapshot when
   direct-fetch stream state becomes dirty; and
2. the vendor's direct-fetch snapshot precedes the primitive, floor/limit,
   descriptor reset and descriptor list that consume it.

A fresh DRM context happens to start with many fields zero, but that is not a
valid replacement for an emitted indexed-file snapshot.  More importantly,
the ordinary `0x97` path deliberately sets stream 0 to class 3 and uses the
packet operands as vertex byte addresses.  Its successful results therefore
cannot prove that leaving the remaining direct-fetch registers inherited is
safe for `0x9f`.

## Minimal safe next packet proposal

Keep the module capability disabled.  In the standalone screen test only,
replace the two-register tail with the complete vendor-shaped direct-fetch
sequence after `p3d_emit_state()` and immediately before mode 7:

```
# one opcode-0 packet, 16 dwords total; only base0 is relocated
0x0000000e,
0x03020100, vertex_vram_base, 0, 0, 0,
0x07060504, 0, 0, 0, 0,
0x13121110, 0x00000014, 0, 0, 0,

0x00000001, 0x2121212c, triangle_primword,
0x00000002, 0x21211819, 0, 4,
0x00000001, 0x2121212d, 0,
0x00000003, 0x21212e2e, 0x20c00000, 0xa1c50020, 0,
0x9f000001, index_vram_start, index_vram_end
```

`stride01 = 0x00000014` means stream 0 is 20 bytes and stream 1 is disabled.
The other bases and stride pairs are explicit zeroes, matching an initial
single-stream state; do not use vendor diagnostic poison values for unused
slots.  The final descriptor list is the already validated 5-dword
`[x, y, r, g, b]` layout and its zero terminator.  The kernel must continue to
permit only the one relocated stream-0 operand, validate all zero unused
bases/strides, validate the exact descriptor extent, and copy the index list
privately.

This proposal removes the only known packet/state mismatch without widening
the UAPI or exposing an address.  It is still a hardware experiment: run it
only with the staged module, one screen test, and reset the target after any
timeout.  A successful fence plus a pixel-for-pixel match against the existing
`0x97` reference is required before re-enabling the capability.

## Full-state experiment (2026-09-10)

The proposal above was implemented only in the standalone test. Its packet
was checked locally to contain the exact 16-dword stream snapshot, then the
four vendor-order primitive/bounds/descriptor packets. The private test gate
was enabled only for that staged module load; normal Mesa never received a
mode-7 capability.

The P690 still timed out at the indexed draw:

```
submit timeout at CALL 31/31:
BM=03000000 prim=d7efa274/d7efa27b sec=00000000/00000000
fence=003a1e90 want=003a1e98; wedged
DMA status450=fbefdf0f want=00000008 writeback=00000007
DWGCTL=00a60ec4 908=638fa047 924=00201002
```

The target was reset and returned to the default-disabled staged module
before Xorg and Q3 were restarted. This rules out omitted inherited stream
registers as the sole cause of the earlier timeout. Native indexed rendering
remains an experimental test-only capability, not a Q3 feature.

## Remaining possibilities if the complete sequence also wedges

A second timeout after this exact sequence would rule out inherited
stream-register state, but not these unresolved hardware conditions:

* the particular direct-fetch layout may require the vendor's optional
  `0xdf000001` layout flag, whose predicate is known in the binaries but has
  not been correlated with this 5-dword Mesa descriptor set;
* the P690 may require a context-creation direct-fetch state latch outside
  the submitted per-draw packet; and
* mode 7 could have an additional ordering or cache/synchronisation condition
  not exercised by the client-array GL captures, which de-index before
  submission.

Do not guess those values into Q3.  Capture a known-good vendor direct-fetch
indexed draw or continue disassembling the Windows direct-fetch setup before
trying any alternate header or latch.

## Direct-fetch primitive-word correction

A later audit found one vendor-proven value mismatch outside the stream
snapshot.  `mtx3661` initializes the packet/direct-fetch context with
`ctx+0x430=2`; `mtx2218` requires that value to install its direct-fetch draw
table; and `mtx3319` clears primitive bit 18 unless `ctx+0x430==1`.
Consequently this path emits triangle `PRIMWORD=0x0002305c`, while the two
failed diagnostics used the copied-path `0x0006305c`.  The guarded builder now
uses `0x0002305c`.  No hardware result exists for that corrected experiment
yet, and normal Mesa remains on `0x97`.
