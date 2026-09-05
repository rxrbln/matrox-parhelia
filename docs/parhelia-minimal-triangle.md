# A minimal 3D triangle: client, capture, decode, replay

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

`tools/gltri.c` is the smallest client that makes the vendor stack emit a real
3D primitive. It exists because glxgears produces ~148 MB of trace for the same
information; one triangle produces **936 KB**, and the whole draw is 19
doorbells.

Captured on the original Parhelia 0x0528 (PCI-X), vendor 2009 mtx stack,
`libGL.so.1.3.0`, X with the `mtx` driver.

## Ground truth

    ./gltri                       # 320x240 window, one triangle, glFlush, exit
    ./3dtri --findcolor ff00ff --vramscan 0x4000000

    findcolor ff00ff: 28000 px, byte range 0x2f654f8..0x30d1b6c

**28000 pixels is exactly `(320-40) * (240-40) / 2`** — the analytic area of the
triangle the program asks for. This is the positive control the 3D work had been
missing: an operation that provably exercises the WARP, with a countable result.

## Traps, each one paid for

* **`atoi` on a hex environment variable returns 0.** `GLTRI_RGB=0xff00ff`
  parsed with `atoi` draws a *black* triangle, which is indistinguishable from a
  draw that never happened. Use `strtol(v, NULL, 0)`.
* **`glFinish` and `glReadPixels` never return** on this stack — `glReadPixels`
  hangs the client outright, so the draw must be verified from outside, by
  scanning VRAM. `glFlush` is fine.
* **The capture's own triangle is still on screen** when the replay runs, so a
  replay that draws nothing looks identical to one that works. Draw the capture
  in a distinctive colour, then overwrite it with a black triangle at the same
  coordinates before replaying.
* **A single-buffered visual** keeps the result in the visible surface where a
  VRAM scan finds it; with double buffering the triangle lands in a back buffer.
* `GL_OUT_OF_MEMORY` is raised once at `glXMakeCurrent` and never again. It is
  the known allocator refusal (AGP down, order-9 cap) and does **not** stop the
  draw — every draw after it reports `GL_NO_ERROR`.
* Linking against the vendor `libGL` needs `-Wl,--allow-shlib-undefined`: it
  wants `GLIBCXX_3.4` symbols this toolchain's `libstdc++` does not version.
  The 64-bit X libraries are in `/usr/X11/lib64`, not `/usr/X11/lib`.

## Shape of the capture

    1 x nr=0   GET_MEMORY_INFO
    1 x nr=31  buffer create (explicit ioctl)
    19 x nr=33 doorbell
    7 mmap regions

Control-page command (`control+0x00`) per doorbell:

| bell | cmd | what |
|---|---|---|
| 1-3 | `0x0c` | create buffer, in band (see below) |
| 4 | `0x05` | pointer-carrying context op |
| 6-7 | `0x14` | |
| 8 | `0x1e` | attach drawable (MakeCurrent) |
| 9 | `0x1d` | |
| 10 | `0x01` | RENDER |
| 18 | `0x01` | **RENDER — the triangle** |

## The draw packet — 7 dwords, complete

Command ring region (`0xa8000`, 672 KB), at offset 0:

```
+0x00  8f000001   draw, index mode; count field = payload dwords - 1 = 1
+0x04  00000000   first index
+0x08  00000003   3 vertices
+0x0c  00000002   register record: 2 dwords follow the header
+0x10  21212425   header, indices LSB first: 0x25, 0x24, pad, pad
+0x14  00000002     idx 0x25 -> 0x1094 = 2            (write-back VALUE)
+0x18  191ca000     idx 0x24 -> 0x1090 = 0x191ca000   (write-back ADDRESS)
```

So a draw is **the primitive plus its completion fence, nothing else**. The
`0x1090`/`0x1094` pair is the memory write-back described in
`parhelia-drm-design.md` §4.1, and the address it is given is the bus address of
the 4 KB region published at `control+0x48` — that region is the **completion
write-back page**, which is what a replay must map before this packet can retire.

## Vertex layout — confirmed against known input

Vertex ring region (`0x320000`, 3 MB), written in the same submit as the draw:

```
+0x40  41a00000 41a00000 3f800000 (unchanged) 3f800000   20.0  20.0 1.0 . 1.0
+0x54  43960000 41a00000 3f800000 (unchanged) 3f800000  300.0  20.0 1.0 . 1.0
+0x68  43200000 435c0000 3f800000 (unchanged) 3f800000  160.0 220.0 1.0 . 1.0
```

**Stride 0x14 = 20 bytes = 5 dwords, base offset 0x40**, and the three positions
are exactly the ones `gltri.c` asks for: `(20,20)`, `(width-20,20)`,
`(width/2,height-20)`. Under a `glOrtho(0,w,0,h,-1,1)` projection those are also
the window coordinates, so this capture does not by itself separate object from
clip space — it does pin the stride, the base and the component order.

No per-vertex colour appears: with `glShadeModel(GL_FLAT)` and one `glColor3f`,
the colour travels in the state stream, not the vertex.

## The state stream, decoded

Record stream region (`0x22000`, 136 KB), 3656 dwords changed for the draw.
Parsed with `record = {u32 key; u32 count; payload}`, `size = 8 + count*itemsize`:

| rec | key | count | meaning |
|---|---|---|---|
| 0 | `0x0d` | 1 | stream opener |
| 1 | `0x35` | 1 | |
| 2 | `0x0c` | 37 | flat render state — see below |
| 3 | `0x12` | 1 | rect |
| 4 | `0x46` | 1 | **combiner (fragment) program object, 1656 B** |
| 5 | `0x05` | 2 | **WARP program bind** |
| 6-11 | | | rect, light-model, misc |
| 12 | `0x21` | 24 | lights |

**Record 5 is the vertex-program bind, live and confirmed:**

```
(0x13, 1)   enable the vertex-engine program
(0x14, 2)   select program serial 2
```

That is exactly the bind form reversed from libGL (`parhelia-vertex-isa.md`,
"Microcode load lifecycle"), now seen in a working hardware draw.

Record 2's flat state, in full:

```
(0x17, 2)          depth func
(0x26, 3f800000)   1.0f
(0x09, 1)
(0x9b, 0)
(0xa6, 45000000)   2048.0f  = GL_POINT_SIZE_MAX, third independent confirmation
(0x40 .. 0x5f, ffffffff)   all 32 slots set to all-ones
```

Slots `0x40..0x5f` were recorded as "destination unresolved". A minimal
untextured triangle sets all 32 to `0xffffffff`, and glxgears does the same, so
they are a 32-entry mask array whose all-ones state is the permissive default —
still not identified, but now known to be uniform and not per-draw.

**Parser gap found:** key `0x21` is *not* a fixed 8-byte item. The doc records
"8, or 112 when op == 2"; a count-24 light record with mixed ops therefore
cannot be advanced by a constant stride, and our parser walks off the end into
float data at that point. Everything before record 12 parses exactly.

## Replay: how far it gets, and the wall

`tools/parhelia-replay.c` against this trace:

```
pass A: 7 mappings
pass H: 7 in-band buffer requests recovered
  nr=31 with live PDEV handle ffffc200014d8004
  nr=31 returned cookie 0xc656000            <- fresh, live
  mapped 0xc656000, 0x9d2e000
  cookie for 0xac1f000 read live from control +0x28 = 0xc6ad000
  mapped 0xc6ad000 (len 0x22000)             <- the record stream, live
  mmap off=0x191ca000 FAILED
  *** doorbell 4 did not return in 2000 ms
applied 26703 writes, 3/7 mappings live
```

Two fixes got it this far and are worth keeping:

1. **In-band buffer creation is now replayable.** Six of the seven regions are
   not created by `nr=31` at all: the client writes `control+0x00 = 0x0c`, puts a
   pointer to a request struct **in its own heap** at `control+0x10`, and rings
   the doorbell. The tracer now dumps that struct (`HEAPREQ` lines) and the
   replay rebuilds it and hands the kernel a pointer to its own copy. The three
   requests are `{0x21,1,…}`, `{0x81,1,fmt 0x15,1920,1200,1}` and
   `{0x02,1,0x4b,1920,1200,0}` — type, count, format, width, height.
2. **`control+0x10` is a pointer only when the command is `0x0c`.** For every
   other command it carries offsets and sizes. An address-range heuristic
   rejected `0x00200002`, which is data, and skipping it broke the stream. Key
   the test on the recovered request table instead: a value we captured a struct
   for is a pointer, and nothing else is.

**The wall is structural.** Doorbell 4 carries command `0x05`, and its control
page is a table of raw pointers:

```
+0x020 = 0ac1c004  +0x024 = ffffc200   -> 0xffffc2000ac1c004   KERNEL pointer
+0x038 = a7cfdbc0  +0x03c = 00007fff   -> 0x00007fffa7cfdbc0   client STACK
+0x058 = aee90a8e  +0x05c = 00002b57   -> 0x00002b57aee90a8e   client MMAP
+0x040 = 0061d280                       -> client HEAP
```

The vendor ABI passes raw process pointers across the ioctl boundary, in the
control page, as ordinary payload. Replaying those addresses into a different
process makes the kernel dereference another process's memory — in practice it
spins in a wait loop and the process becomes unkillable.

So **byte-exact replay of the vendor stream is not achievable in general**, and
that is a property of the ABI, not a gap in the harness. What replay *can* do,
and now does, is bring up the regions and reach the draw; what it cannot do is
carry a pointer graph across processes without relocating every pointer in it.

For the open driver this matters less than it looks: the pointer-passing lives
in the *context management* commands, and none of it is needed to issue the draw
above. The draw packet, the vertex layout and the state stream in this document
are complete and self-contained.

## The register side: a 3D draw touches almost no MMIO

Same client, captured with mmiotrace (`captures/mmio-gltri-0528.txt`). This is
the capture `parhelia-mmio-capture.md` lists as "STILL TO CAPTURE" — it was not
practical with glxgears, and is trivial with `gltri`.

The trace is 296120 lines, of which **33 writes and 19 reads** are register
accesses; the other 296058 are `UNKNOWN` single-steps of framebuffer traffic
through a 4 KB `ioremap` of VRAM at offset `0xf00000`. The complete BAR1
sequence for one triangle:

```
W 0x0030 = 00000000
W 0x0408 = 182c0c23        doorbell
R 0x08f0, R 0x08f0, R 0x0450
W 0x0408 = 182c0c43
W 0x0314 = 00000001        front-end kick
W 0x0030 = 10f00000        pull-aperture descriptor
W 0x0034 = 018e8000
W 0x0038 = 32030020
W 0x003c = 00010020
W 0x0030 = 00000000        cleared either side
W 0x0408 = 182c0c63
W 0x0314 = 00000001
W 0x0030 = 10f00000        ... and again with 09002000 / 32030010 / 00010001
W 0x0408 = 182c0c83 .. 182c0e63    16 more doorbells, +0x20 each
W 0x0408 = 182c1043        one +0x1e0 jump: the big state payload
R 0x08f0 / R 0x0450 x3     wait for idle
R 0x1800 = 00000001
```

**Findings:**

* **There is no separate 3D launch register.** No `0x404`, no `0x918`/`0x91c`,
  no `0x1100`, no `0x1300` — not once in the whole draw. The ring base is set at
  init and every submission is `0x408 = end | 3`. The draw packet *in the ring*
  is the launch, and the doorbell is the trigger. This closes the open question
  "find the real 3D launch mechanism": the answer is that the 2D recipe already
  documented in `memory: parhelia-dma-ring-working` is the 3D recipe unchanged.
* `0x408` advances by exactly `0x20` per write, confirming the 32-byte
  submission granularity the arm trace showed and the `0x14` write-pointer
  increment in `mtx1937` does not contradict — the doorbell is padded.
* `0x0030`–`0x003c` is a four-register descriptor written twice, each time
  bracketed by `0x0030 = 0`. `0x0030 = 0x10f00000` is the VRAM offset
  `0xf00000` with bit 28 set, matching the 4 KB `ioremap` at `0xd0f00000` — so
  this is the pull aperture, programmed per transfer and disarmed after.
* `0x314 = 1` appears exactly twice, both times immediately before an aperture
  program, which narrows its "purpose unknown" note: it is tied to the aperture
  path, not to the doorbell.

For an open driver this is the reassuring result: **everything that makes a
triangle is data in the ring**, and the register interface for a draw is one
write. The hard part is entirely the command stream, which is what the rest of
this document decodes.

## DWGSYNC as a completion signal — and what it now proves

The write-back pair `0x1090`/`0x1094` from the captured draw does **not** on its
own cause anything to be written: appended to the known-good 2D fill it changes
nothing, and both registers read back as 0 (write-only, like the WARP bind
registers). Adding the vendor's DWGSYNC quad from `mtx2153` does not make the
memory write appear either, so how the write-back is armed is still open.

But the DWGSYNC quad itself works, and it is the instrument this project has
been missing:

```
[4][0xc0afafc0][0x00000c07][tag|1][tag][dwgctl_restore]
   -> 0x1300 = 0xc07 flush, 0x12bc = tag|1, 0x12bc = tag, 0x1300 restored
```

Measured on the 0528, submitted in our own ring behind a 2D fill:

| tag submitted | `0x41c` after |
|---|---|
| 8 | `00000008` |
| 16 | `00000010` |
| 64 | `00000040` |

`0x41c` returns the tag of the last retired DWGSYNC. That is a per-packet
completion signal that depends on neither VRAM scanning nor the WARP readback
port — the two instruments that have misled this work.

### What it says about our 3D draw

Bracketing our own address-mode triangle with tag `0x20` before and `0x40`
after:

```
tri: address-mode draw, 5 dw/vertex, verts in VRAM at +0x500000..+0x50003c, tags 32/64
  bmstat=00020000 fetch 0x404=13208280 == end, consumed
  DWGSYNC 0x41c=00000040 -- BOTH tags retired: the draw packet was consumed
  no pixels written to the render target
```

**The post-draw tag retired.** The engine executed through the draw packet and
out the other side, then produced no fragments.

This retires a whole class of explanations. Until now "packets consumed, engine
idle, nothing drawn" rested on `0x404 == end`, which only says the *fetch*
pointer advanced — it is consistent with the engine skipping or mis-parsing the
packet. DWGSYNC says execution genuinely passed the draw. So the draw packet is
**accepted and executed as a no-op**, and the fault is in state or program:
geometry culled or clipped away, a transform producing degenerate positions, a
vertex format that assembles zero primitives, or a fragment program that kills
every fragment. It is not framing, not the ring, and not the doorbell.

Every future negative on this path should carry a DWGSYNC bracket; without one
the result cannot distinguish "did nothing" from "never ran".

## Next

* Relocate the mmap-class pointers (`0x00002b57…`) in command `0x05` using the
  recorded-to-live mapping table the replay already has; that is mechanical.
  Stack and kernel pointers are not relocatable and need the command emulated
  rather than replayed.
* Map the write-back page (`control+0x48`) before the first RENDER; without it
  no draw can retire.
* Give key `0x21` a per-item stride so the state stream parses to the end.
* Find how the write-back at `0x1090`/`0x1094` is armed — the captured draw
  clearly relies on it, and a memory-visible completion word is better than an
  MMIO read.
* Diff our state against the captured record stream field by field. With the
  draw packet now proven to execute, that diff is the whole remaining problem.
