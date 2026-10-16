Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690: a draw packet with more than two triangles wedges the engine

`PARHELIA_PRIMS_PER_PACKET=2` ships as the default because four and eight
triangles per 0x97 packet wedge the engine during ioquake3's level load, while
the synthetic probes appeared to pass.  This is the hardware bisect of that
failure and its outcome: the shape is **more than two triangles in one packet**,
the failure is **probabilistic**, and the probes do reproduce it - the earlier
passes were under-sampling, not evidence of a workload-specific trigger.

All measurements are on the P690 target, ioquake3 `opengl1` timedemo `bench` at
1280x1024 in a glamor X server, unless stated otherwise.

## 1. The failure, exactly

On a freshly booted engine with `PARHELIA_PRIMS_PER_PACKET=4` the wedge is
reproducible on the **third** submit of the ioquake3 client, always the same
batch:

```
parhelia: asynchronous submit fence 000000f0 made no progress for 2001 ms (done 000000e8); wedged
parhelia: BM=03000000 prim=35ce004c/35ce031b sec=00000000/00000000 41c=000000e8
          status450=f8585e09 writeback=0000000b DWGCTL=00a60ec4 908=dbaf8041 924=00200108
```

The primary command fetcher has stopped - `prim` get is far behind put - with
the secondary fetcher idle (`sec=0/0`), so the engine took the batch and then
never made progress again; the terminal fence of that batch is the one that
never retires.  The signature is identical in every run and every variant
below, including on a freshly booted engine.

`PARHELIA_HANG_DUMP` gives the batch, retained as
`captures/p690-multi-prim-hang/prims-per-packet-4-fresh-engine.txt`.  It is
one state stream of 55 packets: 21 register packets and **34 draw packets of
twelve vertices each**, eight
dwords per vertex (x, y, z, r, g, b, s, t), R5G6B5 colour, D16 depth in slot 1,
one texture, the pass-through WARP program - ioquake3's two-dimensional
loading-screen text, 136 triangles of glyph quads.  Its command bytes are
byte-identical to the six batches that precede it in a run that wedges later,
so nothing in the stream distinguishes a wedging instance from a surviving one.

## 2. What the builder emits per grouping

`packet_run()` already refuses a nine-vertex packet, so a run can only take
these shapes:

| `PARHELIA_PRIMS_PER_PACKET` | packet vertex counts |
|---|---|
| 1 | 3 |
| 2 | 3, 6 |
| 3 | 3, 6 (a nine-vertex packet always splits to six) |
| 4 | 3, 6, **12** |
| 8 | 3, 6, 12, 15, 18, 21, 24 |

Grouping 4 adds exactly one new shape over grouping 2: the twelve-vertex,
four-triangle packet.

## 3. Ruled out

Each row is a separate hardware run at grouping 4.  All of them wedged on the
same submit with the same register signature, so none of these is the trigger.

| variant | result |
|---|---|
| `PARHELIA_BATCH_STATE_STREAM=0`, no state-stream aggregation | wedges |
| `PARHELIA_STATS=sync`, one fence wait per submit | wedges |
| `PARHELIA_NO_HW_TCL=1`, draw module instead of the WARP transform | wedges |
| module `vram_vertex=0`, host vertex copies instead of the DMA ring | wedges |
| 640x480 and 512x384 targets (above and below the 250k-pixel watermark) | wedges |
| module `pack_adjacent_packets=0`, no generic CALL packing | wedges |
| `PARHELIA_NO_MIPS=1` | wedges |
| `PARHELIA_NO_LX_PIPES=1`, no ganged texture pipes | wedges |

So it needs neither the aggregate submit, nor the vertex ring, nor the
hardware transform, nor a particular target size, memory watermark, CALL
packing, mip chain or texture pipe route.

## 4. Which packet sizes ioquake3 survives

`PARHELIA_PACKET_TRAPS` makes the builder's refused-vertex-count set settable,
so a run can be made to carry exactly one large shape besides 3 and 6:

| packets in the stream | result |
|---|---|
| 3, 6 | complete timedemo, 48.3 fps |
| 3, 6, 12 | wedges on the first batch that carries a 12-vertex packet |
| 3, 6, 15 | wedges after 2.9 s of the demo |
| 3, 6, 18 | wedges on the first batch that carries an 18-vertex packet |
| 3, 6, 21 | four complete timedemos over two boots, 48.8-49.1 fps |
| 3, 6, 24 | wedges after 2.9 s of the demo |

Two sizes fail on first use, two after a few thousand packets, and one (21)
never failed.  That is not a function of the vertex count; section 5 shows why.

## 5. The probe reproduces it, probabilistically

`--multi-prim-probe` with `PROBE_VERTS=12 PROBE_PACKETS=8` submits **one** batch
with eight draw packets of twelve vertices (32 triangles) in the simplest state
the driver can build: pass-through WARP, untextured, no depth, a 32 bpp 256x256
target, host vertex copies, X stopped, one DRM client.  Repeated on the
hardware, rebooting after each wedge:

```
attempt  1 pass    attempt  6 pass    attempt 11 WEDGE
attempt  2 pass    attempt  7 pass    attempt 12 WEDGE
attempt  3 pass    attempt  8 pass    attempt 13 pass
attempt  4 pass    attempt  9 pass    attempt 14 pass
attempt  5 WEDGE   attempt 10 WEDGE   attempt 15 WEDGE
```

**Five wedges in fifteen attempts.**  The same reproducer at two and four
packets of twelve vertices passed, and no run with six-vertex packets has ever
wedged.

This settles the contradiction in the earlier evidence.  The probe batteries
draw each case once, so a per-batch wedge probability of roughly a third for a
32-triangle stream shows up as "all cases pass" often enough to look clean:
`--multi-prim-probe`, `--depth-interlock-probe` and the GL tests all passed at
grouping 4 and 8 by luck, not because their shapes were different.  ioquake3
issues thousands of such packets per second, so it finds the condition within
the first three submits every time.

The same probe, extended to every vertex layout ioquake3 uses, also passes and
also wedges at random:

| mode | program | layout | dwords/vertex |
|---|---|---|---|
| 0 | pass-through | x y r g b | 5 |
| 1 | WARP transform | x y z r g b | 6 |
| 2 | pass-through + one texture | x y r g b s t | 7 |
| 3 | WARP transform + one texture | x y z r g b s t | 8 |
| 4 | WARP transform + two textures | + s1 t1 | 10 |
| 5 | pass-through + one texture + depth | x y z r g b s t | 8 |
| 6 | mode 5 on an R5G6B5 target with a D16 buffer | | 8 |

Every mode passed every case in the runs that did not wedge, including single
packets of 12, 24 and 60 vertices and runs of 8, 16 and 34 consecutive
twelve-vertex packets, each compared pixel for pixel against the same geometry
drawn one triangle per packet.  The vertex layout, the attribute descriptors,
the surface formats and the depth state are therefore not involved; **the only
variable that changes the wedge probability is how many triangles a packet
carries.**

## 6. Raising the grouping is not worth anything measurable

Head to head on one boot, two timedemos per configuration, no wedges:

| grouping | depth 16 | depth 24 |
|---|---|---|
| 1 (one triangle per packet) | 47.8 / 48.1 fps | 33.4 / 33.4 fps |
| 2 (shipped default) | 48.8 / 49.0 fps | 33.7 / 33.7 fps |
| 7 with 12/15/18 trapped (21-vertex packets) | 49.0 / 49.0 fps | 33.8 / 33.7 fps |

Going from one triangle per packet to two is worth about 1 fps at depth 16 and
0.3 fps at depth 24.  Going from two to seven is worth 0.2 fps and 0.1 fps -
inside run-to-run noise.  At 0.4 us per packet the draw packets are no longer a
measurable share of a frame that is bound by colour and depth traffic, so there
is nothing left to win here even if a larger packet were safe.

## 7. Conclusion and what is not established

Two triangles (six vertices) per packet stays the default.  It is the largest
shape that has never produced the condition, over every probe run, every GL
test and repeated timedemos, and larger shapes buy no measurable frame rate.

What is **not** established is the mechanism.  The register state at the wedge
says the bus-master command fetcher stops with the secondary fetcher idle,
which is consistent with a stalled PCI read by the vertex fetch unit - the
fetch of a range longer than two triangles - blocking the same bus-master
engine that fetches commands.  Nothing in the retained vendor binaries limits
a packet to two triangles, and the 0x0528 captures carry 24-vertex packets, so
this looks like a P690 erratum rather than a programming mistake, but that is
inference.

The three experiments that would settle it, in order:

1. Sweep the wedge probability against the packet's byte length at a fixed
   triangle count - 4 triangles at 5, 6, 8 and 10 dwords per vertex - to
   separate "more than two primitives" from "a range longer than N bytes".
   `PROBE_VERTS`/`PROBE_PACKETS` on `--multi-prim-probe` already do this; it
   needs about 20 attempts per point, so roughly 30 minutes of a dedicated
   target window per point.
2. Put the vertices inline in the command buffer immediately before the packet,
   as every vendor packet that carries more than one primitive does
   (`parhelia-p690-multi-primitive-packets-re.md` section 5, experiment V2).
   If a bus-master vertex read is what stalls, an inline range removes it.
3. Allocate the vertex range with 4 KiB of mapped slack after its last vertex
   (experiment V6) to test whether the fetch unit prefetches past the exclusive
   end address.

## 8. Aside: two large native render targets wedge the engine

While building a 1024x768 variant of the probe, creating a 1024x768 native
colour target and a 1024x768 native depth target in one file and then issuing a
2-D fill of an unrelated 64x64 texture wedged the engine on the fill, twice,
at both 32 and 16 bits per pixel.  The same probe with 256x256 targets is
clean.  That is a separate allocator or placement defect, not part of this
bisect, and it is why the probe keeps its 256x256 targets.

## 8. Two candidate fixes tested on hardware, both refuted (2026-09-12)

The section 7 inference - a vertex-fetch read stalling the bus-master engine -
suggested two concrete causes named by the vendor RE.  Both were implemented
and measured; neither makes a larger packet safe.

| candidate | evidence for it | result at grouping 7 |
|---|---|---|
| `0x1064` (bound-range high) written as `0xffffffff`, the value in every vendor capture, instead of our `0` | `parhelia-p690-multi-primitive-packets-re.md` section 2.3 | still wedges |
| Guard band `0x12f4` clamped to 31.0f, which the vendor does on the LX family and we did not | same document, section 4.1, PROVEN vendor behaviour | still wedges |

The guard-band value is otherwise harmless - a full timedemo at the shipped
grouping runs at the usual 29.2 fps with it - and is kept behind
`PARHELIA_LX_GUARD_BAND=1` rather than enabled, since it buys nothing measured
and the vendor sets it together with other state in the same branch.

**A measurement trap worth stating plainly:** a wedged engine stays wedged
until the machine reboots.  Reloading the module does not recover it - the
device fails to re-probe, `/dev/dri` disappears entirely, and every later run
reports an engine hang that has nothing to do with the change under test.  Two
conclusions in this investigation were drawn from runs on an already-wedged
engine and were wrong: the guard band appeared to break rendering at every
grouping, including the working default, and appeared not to be recovered by
reverting it.  Reboot between variants, and treat a timedemo that finishes
early at an implausible frame rate (791 frames in 0.8 s, "998 fps") as a
failure to draw, not a result.

What remains untried is the structural difference section 5 of the sibling
document names: the vendor copies the vertices *into the command buffer*
immediately before the packet, so `end` is the address of the draw packet
itself, while this driver points `start`/`end` at a separate pinned vertex
object.  That is a kernel ABI change, and the measured value of a larger
packet is still 0.1-0.2 fps.

## 2026-10-05: renewed vendor trace and current performance

The two-triangle default remains an unresolved GPU-wedge workaround, not a
proved hardware maximum. User explicitly requests tracing the original vendor
commands and correctly lifting it. The earlier throughput comparisons do not
close this correctness task or prove today's optimized submission path has
identical costs.

Genuine MtxPser instruction oracles now cover upstream vertex copy/prelude,
range emission, actual CALL publication, old-allocation fence tagging, selector
retirement and rollover. The endpoint is the old vertex payload's exclusive
end; after header rollover it need not equal the new header address. Therefore
matching end to header in every case is not the complete vendor contract.

The next active trace examines uninterrupted prelude→CALL publication→inline
vertex copy→range header→CALL publication. Vendor publication boundaries
exclude vertex payload bytes from executable command spans. This is a concrete
structural difference under investigation, not yet a native fix or proof of
why our separate DMA vertex ranges wedge. Guard-band/high-bound retries remain
refuted; no speculative larger-packet native run has been performed this round.

Current P690 GL1 baseline is 38.1 FPS at1024x768 fullscreen FAAoff/vsyncoff,
uncompressed/aniso1, two-triangle commands, after a57fbfb fixed mapped-ring
allocation beyond the kernel's1MiB ABI. Async submits68→11/frame andCPU
upload/submit14.5→5.0ms. An opt-in doubled mapped capacity test retained smaller
commands but four ring slots, yielded32.6FPS/9submits/9.6ms upload, and stays
opt-in. Correct improvements remain retained; defaults use the faster measured
path. Main evidence is in mesa-driver/tests/p690-swap.

## 2026-10-14: stress reproducer, vertex source, per-draw rebind, vendor span layout

### Reproducer

`parhelia-drm-test --multi-prim-stress` (Mesa parhelia_drm_test.c) repeats one
request of `STRESS_PACKETS` triangle-list packets of `STRESS_VERTS` vertices
(5-dword pass-through vertices, 256x256 target, colour per triangle) for
`STRESS_SUBMITS` requests and compares the target against two triangles per
packet every `STRESS_CHECK` requests.  `STRESS_REBIND=1` sets
`p3d_state.stream_bind_per_draw`: `0x1000 = 3`, `0x1040 = 4` before every
0x97 packet.  Host script: `tools/parhelia-nano-stress.sh`.

| setup (one boot each, nano P690) | result |
|---|---|
| 32 x 12 vertices, host class-3 vertices | stall at request 29 |
| 64 x 12 vertices, local-VRAM class-0 vertices (module `local_vertex_draws=1`) | stall at request 1 |
| 32 x 12 vertices, rebind before every packet | 300 requests pass, pixel-exact |
| rebind: 16 x 24, 8 x 60, 32 x 12, 8 x 63, 24 x 15 vertices | 500 requests each pass |
| rebind: 20 x 18 vertices | stall at request 1 (one sample) |

The class-0 path draws pixel-identically to the class-3 one at two triangles
per packet (hash 8ac6f3c5), so the vertex source is not the cause.  The
per-draw stream rebind is the first change that moves the behaviour, but it
is not sufficient on its own.

### Vendor P690 packet loop (mtxpserd.dll, PROVEN from instructions)

Per packet, loop `0x13732f`..`0x137854`:

1. `0x13748d`: `[2][0x21211000][class][4]` at the cursor (0x1000/0x1040).
2. `0x1374c5`..`0x1374f1`: `0xc1530(bus(mark), bus(cursor), 2, class)`
   publishes the span from the previous mark up to and including that
   rebind: it appends the primary CALL
   `[0x20292820][0][start|2][end|class][0]` and advances the primary put
   (`[esi+0x6c]`), i.e. kicks it.  `0x1148` = 0/1 around the kick only with
   `OGL.FastContextSwitching` (option `+0x3c0`).
3. `0x137506`: mark = cursor; `start` = bus(cursor) (`0x13752b`).
4. `0x1376d2`/`0x137723`: the vertices are copied to the cursor.
5. `0x1377eb`: mark = cursor (past the vertices); `0x1377fb`: `end` =
   bus(cursor); `0x137835`: header `0x17000001 | (dw-1)<<17 | layout` at the
   cursor - the header opens the next span.

So every executable CALL span is `0x97 header ... state ... 0x1000/0x1040
rebind`, the vertex bytes lie between spans and are never fetched as
commands, and no two draw packets share a span.  The only bound on vertices
per packet is the room left in the command buffer (`(limit - cursor)/4 - 7`
dwords, rounded to the primitive at `0x1373c5`).

The 0x0528 vendor captures (236 files, 37024 0x97 ranges) cross a 4 KiB
boundary 780 times, so range alignment is not part of the vendor contract.

Our stream differs in three ways: vertices in a separate buffer, many draw
packets per CALL span, and (before this test) no per-packet rebind.  Next
step: the vendor span layout in the kernel's CALL planner, behind an option.

### Correction: the rebind runs also split the CALL spans

The stress probe submits without `PARHELIA_SUBMIT_CALL_BOUNDARIES`, so the
only packer is `parhelia_coalesce_draw_calls()` (`pack_draw_packets`), which
merges *adjacent* 0x97 packets into one CALL.  Without the rebind the 32 draws
were adjacent and shared one CALL span; with it a register packet separated
every draw, so every 0x97 got its own CALL.  The 2026-09 bisect turned off
`pack_adjacent_packets` but never `pack_draw_packets`, so one draw per CALL had
not been tried before.

| setup (nano, 4 x 5-dword triangles per packet unless noted) | result |
|---|---|
| draws share CALL spans, no rebind | stall at request 29 |
| `pack_draw_packets=0` (one CALL per 0x97), no rebind | 500 x 32 packets pass, pixel-exact |
| `pack_draw_packets=0`, 18-vertex packets, 20 per request | stall at request 1, twice |
| `pack_draw_packets=0`, 18-vertex packets, 1 per request | requests 1-2 retire, stall at request 3 |

**Stall 1 (probabilistic, any count above six vertices): several multi-
primitive draw packets in one CALL span.**  One draw per CALL span removes
it - the vendor never puts two draws in one span.  The per-draw rebind is not
needed for it.

**Stall 2 (count-dependent): 18-vertex packets of 5-dword vertices** (and the
long-known nine-vertex trap) stall with or without the rebind and with one
draw per CALL.  A single packet per request retired twice and stalled on the
third identical request (same commands, same vertex address), so it is a high
per-packet stall probability, not a function of the bytes or the address.
6, 12, 15, 24, 60 and 63 vertices ran thousands of packets clean.  The stall
report CALL listing (kernel 604efb8) shows the primary fetcher a few CALLs
past the first 18-vertex draws, secondary registers zero.  Open: whether the
vendor's inline vertex placement (end == header) avoids it - the one layout
difference left - and what the count dependence is.

### Production (kernel 9db97cb, Mesa 59fd188)

On the P690 the kernel keeps every 0x97 draw of more than six vertices out of
shared CALL spans (both packers; a DRAW_RANGES run may not carry one) and
reports it as `PARHELIA_PARAM_SPLIT_MULTI_DRAWS`.  Mesa mirrors the rule in
its CALL accounting, traps 18-vertex packets next to nine, and then defaults
to 21 triangles per packet: ioquake3 720p CPU per frame 11.67 -> 9.70 ms on
nano, frames unchanged, no stall.  The vendor capture (faaemu, P690 PSER path)
shows the same span structure with whole draws per packet, vertices inline
in a class-2 command buffer, PRIMWORD bit 18 clear, DWGCTL `0x00960ec4` and
`0x1060` = the draw's vertex count; which of these explains the 9/18-vertex
stall is open.

## 2026-10-14 evening: inline vertices, vendor draw values, unbounded packets

Measured on nano (kernel 9dddacd + p690-todo 8f227c8, Mesa p690-todo),
power-cycled after every stall.

**Vendor inline layout in class-3 memory does not retire.**  Kernel option
`p690_inline_draws` (default off) builds a span that opens with a 0x97 draw
the PSER way: the draw's vertices copied into the command block, the header
right behind them, `end` = the header's address, the CALL starting at the
header so the vertex bytes are in no CALL.  The kernel's own T2 prime (one
24-vertex draw, synchronous) timed out with it; on the asynchronous path the
first inline draws stalled (the listing of that request shows unexpected
dwords at the stalled span's start, not understood).  The same draws in their
separate vertex buffer retire on every module load.  The vendor runs this
layout through class 2, the `0x330..0x368` page-table aperture, which this
driver never programs; that is the remaining structural difference.

**The vendor PSER draw values do not explain the 18-vertex stall.**
PRIMWORD bit 18 clear, DWGCTL `0x00960ec4` and guard band 16.0 together:
`--multi-prim-stress` with 20 x 18-vertex packets still stalls at request 1,
as without them.  DWGCTL bit 20 (set by the D3D driver) changes rasterisation:
each 12-pixel right triangle covers 78 instead of 66 pixels, i.e. integer
pixel centres (D3D9), not GL's half-pixel centres, so it is not a value for
this GL driver.  Bit 21 (cleared on the LX family) shows no pixel difference.

**Packets as large as the vertex layout allows stall the P690.**  Mesa
9ff1dba raised the P690 default from 21 triangles (63 vertices) to the 4096-
dword layout bound.  piglit `gl-1.0-beginend-coverage` then stalls the engine
on its third test, on one 135-vertex packet of seven-dword vertices in a span
of its own; with `PARHELIA_PRIMS_PER_PACKET=21` or `2` it passes.

**63 vertices is not safe either, and the bad counts are no simple rule.**
With packets capped at 21 triangles, Mesa's context prime (one 63-vertex,
five-dword packet on a 64x16 target) stalled the engine at
`gl-1.0-drawbuffer-modes` in two of two piglit runs, although 63-vertex
stress packets had passed thousands of times.  `--multi-prim-stress` with
about 800 vertices per request, 200 requests: 30 vertices clean, 33 stalls
at request 1.  Counts that stall: 9, 18, 33, 63, 135 (seven-dword); clean:
6, 12, 15, 24, 30, 60.  Not multiples of 9 (33), not byte or 64-byte
boundaries.

**Overlapping triangles inside a large packet are a second trigger.**  The
prime's triangles wrap at x = 60, so a packet of more than 15 of them
overlaps itself; with the 60 + 3 split it still stalled (`gl-1.0-blend-func`).
`--multi-prim-stress STRESS_OVERLAP=n` (grid cells reused every n triangles):

| vertices/packet | overlap inside the packet | packets | result |
|---|---|---|---|
| 6 | both triangles on one cell | 26000 | clean |
| 12 | every 2nd triangle | 19500 | clean |
| 24 | every 4th | 9900 | clean |
| 30 | every 5th | 7800 | clean |
| 60 | every 15th | 39 | stall at request 3 |
| 60 | none | thousands (earlier) | clean |

**The lockup is a state left behind, not the stalling packet's shape.**
Every fix on the stalling request only moved the lockup: Mesa's context
prime stalled at `gl-1.0-drawbuffer-modes` with packets of 63, 60+3,
30+30+3 and 24+24+15 vertices, with its triangles kept apart, with them
kept in the left half of its 64x16 target, and finally with the prime at two
triangles (one 6-vertex packet) while the application draws of the earlier
tests still used packets of up to 24 vertices.  `drawbuffer-modes` alone
passes.  With two triangles per packet everywhere
(`PARHELIA_PRIMS_PER_PACKET=2`) the full core list matches the previous
day's run test for test (276 pass), twice.  So packets larger than two
triangles leave the P690 engine in a state that locks up a later draw; the
stress probe, which draws one shape repeatedly, does not show this.

Consequences (Mesa p690-todo 63cf691): the P690 default is two triangles
per packet again (ioquake3 720p on nano 35.8-36.1 fps against 35.5-36.2,
CPU 11.6 against 9.6 ms per frame; the frame is GPU-bound).
`PARHELIA_PRIMS_PER_PACKET` still selects larger packets, which then take
only the stress-measured counts 3, 6, 12, 15 and 24 vertices; the original
family keeps the layout bound measured on the 0528.  What state the larger
packets leave behind is open; the WARP T2 per-slot latch (the reason for
the kernel's eight-slot and Mesa's per-context prime) is the first suspect,
not tested.
