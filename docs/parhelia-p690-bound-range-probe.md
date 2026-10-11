# P690 bound-stream range probe and direct-fetch primitive word

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The complete eight-base/four-stride snapshot and its ordering in the failed
mode-7 screen test match `mtx3355`.  Neither the Linux nor Windows emitter has
an extra cache, latch, WARP bind, or fence packet between that state and the
draw.  One state value was still different: the test re-emitted
`PRIMWORD=0x0006305c`, while the vendor's packet/direct-fetch context emits
`0x0002305c`.

The binary chain is exact.  `mtx3661` initializes `ctx+0x430=2` and `mtx2218`
only installs the direct-fetch table while that field is 2.  `mtx3319` always
sets primitive bit 17, but sets bit 18 only when `ctx+0x430==1`; triangles in
the direct-fetch context therefore produce `0x0002305c`.  This correction is
now confined to the guarded mode-7 screen builder.  It is a justified third
hardware experiment, not a production enable.

## Why mode 3 is the safer adjacent experiment

The retained P690 vendor run in `captures/ioq3-p690-working-draw.txt` rendered
and retired a mode-3 packet:

```
0x8f000001, 0, 4
```

Mode 3 draws the sequential ordinal range `[first,last)` from the bound
streams.  It needs the same complete stream snapshot, byte stride, fetch
bounds, and descriptors as mode 7, but it has no index-buffer address or
immutable-index lifetime.  The current Mesa frontend already produces an
expanded, deindexed vertex stream, so mode 3 can test a packet-count reduction
without changing that frontend.

An isolated test now builds exactly:

```
STREAM0..7  = class-3 submit vertex base, 0, 0, 0, 0, 0, 0, 0
STRIDE0..7  = 20, 0, 0, 0, 0, 0, 0, 0
PRIMWORD    = 0x0002305c
floor/limit = 0, 6
descriptors = 0x20c00000, 0xa1c50020, 0
draw         = 0x8f000001, 0, 6
```

The kernel admits this only with `mode3_screen_test=1`, only on P690 with the
raw aperture owned, only synchronously, and only with an exact 30-dword
kernel-copied class-3 vertex blob.  It accepts no address operand and patches
only `STREAM0` from its own DMA address.  `--mode3-screen-test` compares the
result pixel-for-pixel against the same six vertices drawn as two established
`0x97` packets.

## Depth-interlock comparison

This is not yet a production Q3 batching path.  The existing hardware tests
show that overlapping triangles grouped in one draw packet can lose depth
interlock.  The retained P690 `0x8f` record proves that mode 3 executes; it
does not prove that a multi-triangle range has different depth behavior from a
multi-triangle `0x97` range.

The default-off diagnostic renders the same overlapping near/far Z24
triangle pair in both submission orders through four command arrangements:

1. two established one-triangle `0x97` packets (the reference);
2. one `0x8f` packet with `[0,6)`;
3. one `0x8f` packet containing `[0,3)` and `[3,6)`
   (`header=0x8f000003`); and
4. two separate `0x8f` packets, `[0,3)` then `[3,6)`.

Each candidate gets an independent tiled color and Z24 allocation.  After
synchronous completion, the diagnostic compares every color and depth pixel
against the `0x97` reference and reports the first mismatch.  This separates
interlock at a range-pair boundary from interlock at a packet boundary.

The live P690 result is decisive.  With the near triangle first, all three
`0x8f` arrangements matched the two-`0x97` reference.  With the far triangle
first, both the single `[0,6)` range and the two ranges carried by one packet
failed all 18,336 covered color pixels and all 18,336 corresponding depth
pixels.  Two separate `0x8f` packets matched the reference exactly in both
orders, and no case hung.  The depth interlock boundary is therefore the draw
packet, not an individual range pair inside a packet.

**2026-09-11 re-reading.**  The two triangles of this test are identical in
x/y and differ only in depth and colour.  A far-first failure is therefore
equally explained by the P690 drawing only the *first* triangle of a range:
`--fill-rate-probe` with `PROBE_GROUP=2` and non-overlapping triangles lights
exactly half of the expected pixels, and 4 triangles per 0x97 packet hang the
engine (`parhelia-p690-q3-performance-analysis.md`).  "Depth interlock is per
packet" is not established for the P690.

## Production opt-in

The production builder consequently emits one `0x8f` packet per triangle:
`[0,3)`, `[3,6)`, and so on.  It binds the batch's contiguous transformed
vertex stream once through the existing persistently pinned CREATE_VERTEX
ring, replays the complete eight-base/four-stride snapshot, uses direct-fetch
`PRIMWORD=0x0002305c`, sets exact floor/limit bounds, and replays the active
descriptor list before the range packets.  One relocation names STREAM0 and
the selected fenced ring-slot offset; individual packets contain ordinals and
need no address relocation.

The kernel advertises `PARHELIA_PARAM_BOUND_RANGE` only on P690 when the raw
aperture is owned and the independent `mode3_bound_range=1` module parameter
was requested.  The parameter defaults false and is unrelated to
`mode3_screen_test`.  The exact validator requires the complete tail, one
strictly consecutive three-vertex range per packet, descriptor extent equal
to stride, and the whole range inside its creator-owned CREATE_VERTEX object.
Async submissions retain the GEM reference until their fence retires; Mesa's
three ring slots are not reused until their individual fences complete.

## Status

The Mesa library, four-way diagnostic, production opt-in, and kernel module
build locally, and the full `parhelia-drm-test --selftest` passes.  The screen
test remains separately gated by `mode3_screen_test`.  The production path has
not been deployed or exercised by Q3.  With `mode3_bound_range` left at its
default false, Mesa continues to emit the established `0x97` path.
