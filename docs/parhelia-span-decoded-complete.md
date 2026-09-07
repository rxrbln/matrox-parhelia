# The captured triangle span, decoded end to end

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

One `tools/gltest.c` frame captured from the vendor stack on a Parhelia-512
(PCI 0x0528), taken apart packet by packet so that the same submission can be
*generated* rather than replayed.  Everything here comes from
`tools/decode-span.py`, which walks `ring7.hex` / `sec7.hex` / `reloc7.txt` in
execution order; the annotated listing in section 6 is that tool's output
verbatim.

    tools/decode-span.py captures/live-ring-0528 --all

## 0. Which image this describes, and what is stable in it

`tools/build-ring-replay.py` rebuilds the three images from a live capture, and
the capture directory is rewritten on every hardware run.  This document was
written against

    14044b469ffa69332195737d4715ec24  ring7.hex
    77a59ce550fa7a31210cbc6b6fc2087f  sec7.hex
    b0c93632690ea7f0901cb8bb7237ddb7  reloc7.txt

Across three regenerations observed while writing this, **`reloc7.txt` did not
change at all** (same md5) and every structural quantity was identical: 749
ring dwords, 1041 secondary dwords, 122 calls, 648 register writes, 10
opcode-0x41 packets, one draw, the same 120-step reduced recipe, the same block
file slots and the same viewport.  What changed was only the system-memory bus
addresses: the WARP heap moved `0x07aa****` -> `0x07d5****` -> `0x076a****`,
the pushbuffer arena `0x16a4****` -> `0x1626****`, the fence page
`0x1cfd2020` -> `0x1c9b6020`.

So: **every `sec7` offset quoted below is invariant; every value with a
system-memory address in it is not.**  A generator relocates by offset.  VRAM
surface bases (`0x02000000`, `0x01000000`) did not move across those runs, but
they are allocation-dependent and must be treated the same way.

## 1. What the span is

The span is the richest gap between two consecutive draws of a looping GL
client, so it carries a full state emission rather than the four-packet steady
state.  Three phases are interleaved, and separating them is most of the work:

| phase | ring range | what it is |
|---|---|---|
| previous GL frame teardown / context restore | 0 .. 515 | two near-identical blocks of texture-unit, combiner and DDA-plane writes |
| X server 2D traffic | throughout, and one header-less bare-tag section at ring[258] | solid and pattern fills into surface slot 1 |
| the frame we want | 516 .. 744 | raster state, block loads, WARP binds, vertex format, surfaces, combiner, draw |

The section at ring[258] is the X server's own 2D buffer: that call's START low
bits are `|0`, so it carries no packet headers at all, only bare tag dwords.
`decode-span.py` handles both framings; a decoder that assumes headers
desynchronises there.

## 2. The block file, and the viewport it implies

Ten opcode-0x41 packets: **eight are constant loads, two are not.**

A constant load has the descriptor shape `[n, firstSlot, 1, 0]` with `n` equal
to the header's count field.  Two packets do not have that shape; they are the
`mtx1814` caller-preformatted form of `docs/parhelia-opcode-41.md`, that is,
WARP program bind blobs.  `decode-span.py` discriminates on the descriptor, not
on the opcode.

| # | ring | sec7 | header | kind | slots written |
|---|---|---|---|---|---|
| 0 | 571 | 686 | `41020007` | blob | program bind, carries 4 class-3 pointers |
| 1 | 581 | 701 | `41100023` | load | `c[0x000..0x007]` |
| 2 | 586 | 738 | `410a0017` | load | `c[0x134..0x138]` |
| 3 | 591 | 763 | `410a0017` | load | `c[0x149..0x14d]` |
| 4 | 596 | 788 | `410a0017` | load | `c[0x155..0x159]` |
| 5 | 601 | 813 | `4104000b` | load | `c[0x160..0x161]` |
| 6 | 606 | 826 | `41080013` | load | `c[0x172..0x175]` |
| 7 | 611 | 847 | `41020007` | load | `c[0x178]` |
| 8 | 616 | 856 | `4104000b` | load | `c[0x17d..0x17e]` |
| 9 | 626 | 877 | `4106000f` | blob | program bind, carries `b0=0x0E0`, `b1=0x160` |

32 of the 384 vec4 slots are written in total.

### 2.1 The MVP

`c[0x000..0x003]` is the identity matrix and `c[0x004..0x006]` a second identity
3x3: palette entry 0, whose layout is `7i+0..3 = MVP`, `7i+4..6 = normal
matrix`.  `c[0x007]` is all zero.  The capturing client sets neither projection
nor modelview, so an identity MVP is exactly right, and it confirms the palette
layout independently of the vendor binary.

### 2.2 The viewport

    c[0x160] scale  = (160, -120, 0.5, 1)
    c[0x161] offset = (160,  120, 0.5, 240)

Applying `scale = (W/2, -H/2, (f-n)/2, 1)` and
`offset = (x0+W/2, y0+H/2, (n+f)/2, H)`:

| quantity | derivation | value |
|---|---|---|
| W | `2 * scale.x` | **320** |
| H | `-2 * scale.y` | **240** (`offset.w` is also 240, an independent check) |
| x0 | `offset.x - scale.x` | **0** |
| y0 | `offset.y + scale.y` | **0** |
| near | `offset.z - scale.z` | **0** |
| far | `offset.z + scale.z` | **1** |

**The implied viewport is `glViewport(0, 0, 320, 240)` with
`glDepthRange(0, 1)`**, which is the default `GLTEST_W`/`GLTEST_H` of
`tools/gltest.c` with the window origin at the surface origin.  Two independent
witnesses in the register stream agree: the last scissor before the draw is
`0x132c SCISSOR_MAX = 0x00f00140` = (x=320, y=240), and the X server's 2D
background fill for that window is `0x138c LEN = 0x00f00140` = 320x240.

The `gltest.c` NDC triangle therefore lands at:

| NDC | window pixel |
|---|---|
| (-0.7, -0.6) | (48, 192) |
| ( 0.0, +0.6) | (160, 48) |
| (+0.7, -0.6) | (272, 192) |

y grows downward, so the triangle points *up* in the framebuffer, spans
x 48..272 and y 48..192, and covers about `0.5 * 224 * 144 = 16128` pixels.

With the colour target the span binds (`0x1220 = 0x02000000`,
`0x1224 = 0x32020780`, ARGB8888, pitch 1920 px = 7680 bytes per row) those
pixels are the VRAM byte range

    0x02000000 + 48*7680  + 48*4   =  0x0205a0c0
    0x02000000 + 192*7680 + 272*4  =  0x02168440

so **roughly 64 KB of changed bytes inside `0x0205a0c0 .. 0x02168440`** is what
a successful draw must produce.  That is the range to diff, and it is the
concrete prediction this decode buys.

> Open discrepancy carried forward from `docs/parhelia-rendertarget.md`: an
> earlier hardware measurement put a vendor triangle's footprint at
> `0x02f654f8..0x030d1b6f`, which is not reachable from `0x02000000` with a
> 7680-byte pitch.  Either that measurement belongs to a run with a different
> surface allocation, or `0x1220` is not the colour destination for this
> `DWGCTL` (section 5.6).  One hardware run resolves it: diff both ranges.

### 2.3 The rest of the block file

| slots | content | reading |
|---|---|---|
| `0x134..0x138` | 0.8, 0.2, 0, (0,0,0,1), 0.04 | the GL default **material**: diffuse 0.8, ambient 0.2, specular 0, emission (0,0,0,1), and 0.04 = material ambient 0.2 x light-model ambient 0.2 |
| `0x149..0x14d` | identity 4x4, then (0, 1200, 0, 32) | a second matrix; 1200 is the screen height and 32 the guard band, so `0x14d` is a window/device vector rather than a matrix row [INF] |
| `0x155..0x159` | (0,0,0,1), 0.8, 0.2, 0, (0,0,0,1) | the **back-face material**, same defaults in a different order |
| `0x160..0x161` | viewport scale / offset | section 2.2 |
| `0x172` | (1, 0, 0, 0.5) | `.w = 0.5` = half point size, i.e. `glPointSize(1.0)` |
| `0x173` | (0, 2048, 0.5, -0.5) | |
| `0x174` | (8160, 8160, -8160, -8160) | guard-band rectangle; `0x12f4 GUARDBAND = 32.0` and 32 x 255 = 8160 |
| `0x175` | (128, 0, 0, 0) | |
| `0x178` | (0, 0, 0.5, 786432) | polygon offset: factor 0, units 0 |
| `0x17d` | (8388608, 0.05, 31, 0.032258) | `2^23` is the float-to-fixed snap bias; 31 and 1/31 are a paired scale |
| `0x17e` | (0, 0.0625, 0, 0) | 1/16, the subpixel quantum |

Everything the client actually varies is in `0x160/0x161` and the palette; the
rest is fixed-function defaults the vendor driver uploads unconditionally.

### 2.4 The two blobs are WARP program binds

Blob 0 (ring 571, sec7 686), emitted immediately after a `0x1100 WARP_ENTRY`
write:

    descriptor 03800015 00000000 3f800000 <class-3 ptr>
    payload    3f803f80 <class-3 ptr> ffffffff <class-3 ptr>

Blob 9 (ring 626, sec7 877), likewise:

    descriptor 000000e0 00000160 00000080 00000800
    payload    00000000 00000001 00000002 00000003
               02002004 0200200c 00000000 00000000
               <class-3 ptr> ffffffff ffffffff ffffffff

The descriptor of blob 9 is `(0xE0, 0x160, 0x80, 0x800)`.  `0x0E0` is the light
block base and `0x160` the raster block base: this is the packet that tells the
vertex program where its two constant bases `b0` and `b1` live, and it
**settles item 4 of `docs/parhelia-block-file-constants.md`**.  The
`count=224 first=0x160` that looked like a 224-vec4 load is not a load at all;
it is this bind blob's `(b0, b1)` pair being misread as a load descriptor.
`0x02002004` and `0x0200200c` in the payload are the same words the span writes
to `0x1130 WARP_PIPECTL`.

Both blobs carry class-3 system-memory pointers and must be relocated
(section 4).

## 3. The ordered minimal recipe

The reduction rules, and why each is safe:

* engine registers are persistent, so for a plain register **only the last
  write before the draw survives**;
* `0x1260/0x1264/0x1268/0x126c` are a combiner *port*: the SRC write selects the
  slot in bits[4:0] and the DST write commits the instruction, so the unit of
  de-duplication is the (channel, slot) pair and the last program for each slot
  survives;
* `0x10b8 ATTRDESC` is a sequential FIFO rewound by `0x10b4 BLOCKPTR = 0`, so
  only the run after the last rewind survives;
* 2D fills, `SRC_XY`/`DST_XY`/`FCOL`/`BCOL`/`PAT`/`LEN`, and every `DWGCTL`
  whose low nibble is a 2D opcode are X-server traffic and are dropped;
* `FENCE_ADDR`/`FENCE_VALUE`/`DWGSYNC`/`STROBE`/`EOBTAG` are submission
  bookkeeping, which a generator emits itself, and are dropped.

**648 register writes reduce to 109.  The complete recipe is 120 steps: 109
register writes, 8 block-file loads, 2 program blobs and 1 draw.**  The full
ordered table is in section 6.

Its shape:

1. texture-unit defaults and disabled sampler state (15 writes)
2. the legacy DDA attribute planes (11 writes), **droppable on the WARP path**
3. the four surface descriptor pairs (8 writes)
4. raster state: scissor, raster mode, AA threshold, blend, alpha test, depth,
   stencil, z clip, fog, plane mask, `0x12b0`, the 3D `DWGCTL`, guard band
   (20 writes)
5. the two program blobs and the eight block loads, interleaved with WARP mode
   strobes (10 packets)
6. vertex defaults and `VTXFMT0/1/2` (7 writes)
7. the WARP entries, `WARP_MODE`, `WARP_PIPECTL`, `0x111c`, `0x1144`
   (12 writes); all seven entry pointers are boot-specific
8. `PRIMWORD`, the eight `VTXBASE`, four `VTXSTRIDE`, `VTXFIRST`, `VTXCOUNT`,
   `BLOCKPTR = 0` and the four `ATTRDESC` dwords (20 writes)
9. the combiner program, five RGB slots and one alpha slot, plus the four
   combiner constants (16 writes)
10. `VTXBASE0` memory class, `VTXSTRIDE0_1`, then the draw

By register family the 109 kept writes are: raster 20, vertex stream and
attribute 20, combiner 16, texture 15, WARP 12, legacy DDA 11, surfaces 8,
vertex format 7.

The vertex format is fully determined by the `ATTRDESC` run in step 8:

    ATTRDESC = 0x20c00000   stream 0, dword offset 0, mask 0x3 (xy)  -> position
    ATTRDESC = 0xa1c50020   stream 0, dword offset 2, mask 0x7 (xyz) -> colour
    ATTRDESC = 0x00000000   terminator
    ATTRDESC = 0x00000000

which is exactly the 5-dword `x, y, r, g, b` layout, and the draw's
`end - start = 0x3c` = 3 x 20 bytes confirms it.

**New inference worth recording:** `0x1040 VTXSTRIDE0_1 = 0x00000004` for a
5-dword vertex, and the draw header's count field is also 4.  The stream-0 field
of `0x1040` is therefore very probably **dwords-per-vertex minus one**, the same
encoding as the draw header, and *not* a byte stride; the existing decoder
prints "stride pair: 4, 0 bytes", which cannot be right for a 20-byte vertex.
[INF, two witnesses in one packet pair]

## 4. Boot-specific absolute addresses

Everything a generator has to relocate, by its dword offset in `sec7.hex`.  The
offsets are invariant across regenerations; the values are not, so they are
given only as a sample from the image named in section 0.  Section 6 prints the
live table.

**(a) WARP program entries** - register writes whose value is
`(busAddress / 3) | 3`.  All seven point into the vendor kernel's class-3
program heap, which is allocated per context and per boot:

| sec7 | ring | register | sample value | implied bus address |
|---|---|---|---|---|
| 685 | 566 | `0x1100` WARP_ENTRY | `076ac703` | `0x16405500` |
| 700 | 576 | `0x1100` WARP_ENTRY | `076ac903` | `0x16405b00` |
| 876 | 621 | `0x1100` WARP_ENTRY | `076acb03` | `0x16406100` |
| 917 | 646 | `0x1100` WARP_ENTRY | `076acd03` | `0x16406700` |
| 925 | 651 | `0x10c8` WARP_STAGE2_ENTRY | `076acf3f` | `0x16406db4` |
| 926 | 651 | `0x10cc` WARP_STAGE3_ENTRY | `076acf4f` | `0x16406de4` |
| 928 | 651 | `0x10d0` WARP_STAGE4_ENTRY | `076acf3f` | `0x16406db4` |
| 929 | 651 | `0x10d4` WARP_STAGE5_ENTRY | `076acf4f` | `0x16406de4` |
| 930 | 651 | `0x10d8` WARP_STAGE6_ENTRY | `076ac103` | `0x16404300` |

`0x10f8 WARP_ENTRY_OBJ2 = 2` at sec7 918 is the documented "unbound" marker,
not an address, and needs no relocation.

**(b) class-3 pointers embedded in the two 0x41 blobs.**  These are *data*
inside a packet rather than register writes, so a relocation pass has to know
the packet layout to find them at all:

| sec7 | ring | position | sample value |
|---|---|---|---|
| 690 | 571 | blob at sec[686], descriptor word 3 | `076ab703` |
| 692 | 571 | blob at sec[686], payload word 1 | `076aab03` |
| 694 | 571 | blob at sec[686], payload word 3 | `076ac103` |
| 890 | 626 | blob at sec[877], payload word 8 | `076aab03` |

**(c) the draw's vertex buffer, the surfaces and the fence page**

| sec7 | ring | sample value | what |
|---|---|---|---|
| 1039 | 744 | `16262414` | draw vertex buffer start, absolute PCI/system bus address |
| 1040 | 744 | `16262450` | draw vertex buffer end |
| 977 | 686 | `02000000` | `0x1220` SURF0_BASE, VRAM byte offset of the colour target |
| 983 | 691 | `01000000` | `0x1228` SURF1_BASE, VRAM byte offset of the depth target |
| 3 | 0 | `1687a000` | `0x1090` FENCE_ADDR, write-back bus address |
| inline | 10, 513, 736 | `1c9b6020` | `0x1090` FENCE_ADDR, in the primary ring |

Plus the X-server surface bases at sec7 546, 588, 603 and 618, which belong to
the droppable 2D traffic.

## 5. Completeness: why the span can execute and draw nothing

The span is *state-complete for a context that is already alive* and
*incomplete as a standalone submission*.  In descending order of likelihood:

### 5.1 The vertex data is not in the replay image at all

The draw is `97080001 <start> <end>`, and `0x1000 VTXBASE0` is set to
`0x00000003` immediately before it: base 0, **memory class 3 = PCI/system
memory**.  Those two dwords are absolute bus addresses in the capturing
client's pushbuffer arena.

`sec7.hex` contains only the sections that a CALL packet named.  In the image of
section 0 the vertices are at `0x16262414..0x16262450`, and the captured section
list has the section immediately before ending at exactly `0x16262414` and the
next beginning at exactly `0x16262450`.  **The vertex buffer occupies precisely
the uncaptured gap between two command sections**: it sits inline in the same
arena, right after a command section, and nothing calls it, so the capture never
dumped it.  This was re-checked after each regeneration and holds every time.

A byte-for-byte replay therefore points the vertex fetcher at memory that, once
the capturing client is gone, holds something else or nothing.  Degenerate or
off-screen vertices give exactly the observed symptom: every packet executes,
the engine goes idle, no pixel changes.  **This alone is sufficient to explain
the mystery**, and it is consistent with `same-boot-replay.sh --ringvtx`, which
substitutes the vertex pointer, being the variant that produced a triangle.

Fix for a generator: put the vertices in our own DMA buffer and write our own
address into the draw packet.  Fix for the capture tool: dump the arena around
every draw pointer, not only the called sections.

### 5.2 All seven WARP program entries point into a heap we do not own

`0x1100` is written four times and stages 2..6 once each, all with class-3
pointers into the vendor kernel's program heap for the capturing context, plus
four more pointers buried in the two 0x41 bind blobs.  **The programs themselves
are not in the span**, only pointers to them.  Replaying without those pages
resident and holding the same contents runs whatever happens to be at those bus
addresses as a vertex program.  The addresses moved on every one of the three
regenerations observed here, which is direct evidence that they are per-boot.

### 5.3 The render target is an off-screen back buffer and the swap is not in the span

`0x1220 = 0x02000000` (32 MB into VRAM), `0x1224 = 0x32020780`: ARGB8888, pitch
1920 px.  The visible screen is at VRAM offset 0, because every X-server 2D fill
in the span targets `dst slot 1` with `SURF1_BASE = 0x00000000`.
`build-ring-replay.py` deliberately stops *at* the draw's call, so the buffer
swap that copies `0x02000000` to the screen belongs to the next frame and is not
present.

A successful draw is therefore invisible on screen by construction, and only a
VRAM diff reaching past 32 MB can see it.  If the diff window did not cover
`0x0205a0c0..0x02168440`, "zero changed pixels" is a measurement artefact rather
than a hardware result.

### 5.4 Nothing clears the colour target

Every `DWGCTL` in the span, by destination slot:

    value     count  low  BOP  src slot  dst slot
    00000004    3      4    0      0         0     pipeline prime, not a fill
    001000f4    2      4    f      0         0     pipeline prime
    00360004    1      4    0      0         0     pipeline prime
    005000f5    4      5    f      0         0     pipeline prime
    002010cc    1     12    c      0         0     opcode 12, not a fill
    00a60ec4    2      4    c      2         3     the 3D draw word, see 5.6
    000005c0   10      0    c      1         1     2D solid fill
    000024c0    6      0    c      0         1     2D pattern fill
    00000207    2      7    -      -         -     DWGSYNC soft trap
    00000307    4      7    -      -         -     DWGSYNC soft trap
    00000407    3      7    -      -         -     DWGSYNC soft trap
    00000707    9      7    -      -         -     DWGSYNC soft trap

The only fills (low nibble 0) are `0x5c0` and `0x24c0`, both `dst slot 1`.
**No packet clears SURF0 at `0x02000000`.**  `gltest.c` calls `glClear` only
when `GLTEST_CLEAR=1`, which is not the default, so the back buffer holds
whatever was there.  Not a cause of an empty draw by itself, but it means a
before/after diff of the back buffer is the only valid detector.

### 5.5 The block file is only partially written

32 of 384 vec4 slots.  In particular the twelve light blocks at
`c[0x0E0..0x133]` are **not** written, although the material blocks that pair
with them (`0x134..0x138`, `0x155..0x159`) are, and blob 9 explicitly binds
`b0 = 0x0E0`.  Whatever the vertex program reads from `b0` is inherited from
the vendor context.  `gltest.c` leaves lighting off unless `GLTEST_LIGHT=1`, so
the program probably does not read them, but the state is genuinely incomplete
and a generator has to fill them.

### 5.6 The surface-slot ambiguity in the 3D DWGCTL

`0x1300 DWGCTL = 0x00a60ec4` is the vendor's 3D draw word.  Read with the *2D*
field map that the same register uses for fills, bits[9:8] = 2 and
bits[11:10] = 3, that is **src slot 2, dst slot 3**, and the span leaves both of
those surfaces at base 0 with **pitch 0**:

    0x1230 SURF2_BASE = 00000000    0x1234 SURF2_CFG = 32020000   pitch 0
    0x1238 SURF3_BASE = 00000000    0x123c SURF3_CFG = 32020000   pitch 0

`docs/parhelia-pixel-pipeline-complete.md` instead reads bits 9, 10 and 11 as
part of the fixed 3D constant `0x00860e00`, which would make the colour target
`0x1220` and the depth target `0x1228`, both of which the span sets sensibly
just before the draw.  The two readings cannot both be right, and this span
cannot distinguish them because the vendor always emits the same constant.

This is a cheap hardware experiment and it should be run: set `SURF2` and
`SURF3` to the same values as `SURF0` and `SURF1` and resubmit.  If pixels
appear, the slot fields are live in the 3D form too, and every from-scratch
stream has to program four surfaces instead of two.

### 5.7 State that is present and correct, so can be excluded

Each of these is a plausible suspect that the decode rules out:

| register | value | why it is not the problem |
|---|---|---|
| `0x1324` PLNWT | `ffffffff` | all planes writable |
| `0x1328/0x132c` scissor | `(0,0)-(320,240)` | matches the viewport exactly |
| `0x12e8` BLENDCTL | `00800202` | blend off, ONE / ZERO |
| `0x12e4` ALPHATEST | `0000000e` | alpha test off, func ALWAYS |
| `0x12c8` DEPTH_STENCIL | `001000e4` | depth test off, stencil off |
| `0x12d0` STENCIL_WRITE | `000000ff` | depth write off (bit 8 clear), harmless with the depth test off |
| `0x12fc` RASTERMODE | `0bb80024` | cull select bits[1:0] = 0, nothing culled |
| `0x12f4` GUARDBAND | `42000000` | 32.0, and `c[0x174]` agrees |
| `0x1264/0x126c` combiner slot 0 | `00000200` / `01400400` | `MOV(A)` with A = `v0`, the interpolated vertex colour, saturating into `r0`: the Gouraud path |
| `0x1200` TEXUNIT0_CTL | `0001000f` | texturing disabled |

One genuinely unexplained bit remains: `0x10b0 PRIMWORD` is changed from
`0x0002305c` to `0x0006305c` immediately before the draw, and bit 18 is
documented as selecting the *legacy per-vertex-register path*, which contradicts
a DMA vertex-buffer draw.  Either the bit-18 reading is wrong or it means
something else here; it should not be copied blindly into a generator without
testing both values.

## 6. Tool output

Everything below is `tools/decode-span.py --all` on the image of section 0,
verbatim.


# Annotated execution-order listing

ring[  i] / sec[off]

ring[  0] CALL  sec[   0..   4]  4 dw  START|2 header-ful opcode packets
          sec[   0] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[   1] TAG 21212425
            sec[   2]  s 0x1094 FENCE_VALUE        = 00000002  [SYNC/FENCE]
                        submission serial / write-back value
            sec[   3]  c 0x1090 FENCE_ADDR         = 1687a000  [BOOT-POINTER]
                        write-back bus address 0x1687a000 | class 0
ring[  5] CALL  sec[   4..  10]  6 dw  START|2 header-ful opcode packets
          sec[   4] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[   5] TAG c0afafc0
            sec[   6]  s 0x1300 DWGCTL             = 00000307  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope fence RT surface A+fence RT surface B
            sec[   7]  s 0x12bc DWGSYNC            = 000000f1  [SYNC/FENCE]
                        tag 0xf0; bit0=1 ARM block-until-retire
            sec[   8]  s 0x12bc DWGSYNC            = 000000f0  [SYNC/FENCE]
                        tag 0xf0; bit0=0 value that lands in 0x41c (read &~7)
            sec[   9]  a 0x1300 DWGCTL             = 00a60ec4  [ESSENTIAL-3D]
                        [documented form: the vendor kernel 3D draw DWGCTL (pixel-pipeline §3.8)]; 3D form: constant 0x00860e00 (bits 9,10,11,17,18 + clip bit23); low byte 0xc4 = opcode 4 | BOP 0xc (COPY); bit21 = cfg&0x40; NOTE pixel-pipeline §3.8 also ORs ((surf>>25)&0xf) into the low nibble; with the captured colour target 0x32020780 that is 9, and 9 is not present here - the formula does not reproduce this value
          ring[ 10] TAG 21212425  (inline ring tag)
                       s 0x1094 FENCE_VALUE        = 00000003  [SYNC/FENCE]
                        submission serial / write-back value
                       c 0x1090 FENCE_ADDR         = 1c9b6020  [BOOT-POINTER]
                        write-back bus address 0x1c9b6020 | class 0
ring[ 13] CALL  sec[  10..  14]  4 dw  START|2 header-ful opcode packets
          sec[  10] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[  11] TAG 2121c0c0
            sec[  12]  s 0x1300 DWGCTL             = 00000307  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope fence RT surface A+fence RT surface B
            sec[  13]  b 0x1300 DWGCTL             = 000024c0  [2D/X-DROPPABLE]
                        2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 0; dst slot 1; bit13 8x8 pattern enable
ring[ 18] CALL  sec[  14..  22]  8 dw  START|2 header-ful opcode packets
          sec[  14] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[  15] TAG 21afc0c0
            sec[  16]  b 0x1300 DWGCTL             = 00000004  [2D/X-DROPPABLE]
                        2D form: opcode 4=3D-only opcode, not in the 2D table; BOP 0x0; src slot 0; dst slot 0
            sec[  17]  s 0x1300 DWGCTL             = 00000407  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain
            sec[  18]  s 0x12bc DWGSYNC            = 00000109  [SYNC/FENCE]
                        tag 0x108; bit0=1 ARM block-until-retire
          sec[  19] TAG 2121c0af
            sec[  20]  s 0x12bc DWGSYNC            = 00000108  [SYNC/FENCE]
                        tag 0x108; bit0=0 value that lands in 0x41c (read &~7)
            sec[  21]  b 0x1300 DWGCTL             = 000024c0  [2D/X-DROPPABLE]
                        2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 0; dst slot 1; bit13 8x8 pattern enable
ring[ 23] CALL  sec[  22..  25]  3 dw  START|2 header-ful opcode packets
          sec[  22] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[  23] TAG 212121de
            sec[  24]  b 0x1378 REG_1378           = 00000000  [2D/X-DROPPABLE]
                        UNKNOWN - not documented anywhere in docs/
ring[ 28] CALL  sec[  25..  29]  4 dw  START|2 header-ful opcode packets
          sec[  25] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[  26] TAG 2121b5c9
            sec[  27]  a 0x1324 PLNWT              = 00000000  [ESSENTIAL-3D]
                        plane write mask 0x00000000
            sec[  28]  a 0x12d4 FOG_CTL            = 00000000  [ESSENTIAL-3D]
                        fog off; mode bits[2:1]=0; bit4=0; B=0 G=0 R=0
ring[ 33] CALL  sec[  29..  40] 11 dw  START|2 header-ful opcode packets
          sec[  29] PACKET 00000009 op=0x00 reglist  total=11 dw
          sec[  30] TAG b2cbcac0
            sec[  31]  b 0x1300 DWGCTL             = 001000f4  [2D/X-DROPPABLE]
                        [documented form: pipeline-priming pseudo-draw 1 (PARD 0x2e832)]; 2D form: opcode 4=3D-only opcode, not in the 2D table; BOP 0xf; src slot 0; dst slot 0; bits 0x00100000 UNKNOWN
            sec[  32]  a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[  33]  a 0x132c SCISSOR_MAX        = 00010001  [ESSENTIAL-3D]
                        (y=1, x=1)
            sec[  34]  a 0x12c8 DEPTH_STENCIL      = 00000000  [ESSENTIAL-3D]
                        depth test off; depth func NEVER; stencil off; stencil func NEVER; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 0
          sec[  35] TAG 8988bfb9
            sec[  36]  a 0x12e4 ALPHATEST          = 00000001  [ESSENTIAL-3D]
                        alpha test ENABLED; func NEVER; ref 0; mask[31:24]=0x00
            sec[  37]  a 0x12fc RASTERMODE         = 00000000  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=0 bit3=0 bit4=0; bits[31:5]=0x0 UNKNOWN
            sec[  38]  a 0x1220 SURF0_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[  39]  a 0x1224 SURF0_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[ 38] CALL  sec[  40..  49]  9 dw  START|2 header-ful opcode packets
          sec[  40] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[  41] TAG 79787780
            sec[  42]  a 0x1200 TEXUNIT0_CTL       = 00040101  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x1; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[  43]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[  44]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[  45]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[  46] TAG 21217170
            sec[  47]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[  48]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 43] CALL  sec[  49..  58]  9 dw  START|2 header-ful opcode packets
          sec[  49] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[  50] TAG 79787781
            sec[  51]  a 0x1204 TEXUNIT1_CTL       = 00040102  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x2; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[  52]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[  53]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[  54]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[  55] TAG 21217170
            sec[  56]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[  57]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 48] CALL  sec[  58..  67]  9 dw  START|2 header-ful opcode packets
          sec[  58] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[  59] TAG 79787782
            sec[  60]  a 0x1208 TEXUNIT2_CTL       = 00040104  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x4; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[  61]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[  62]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[  63]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[  64] TAG 21217170
            sec[  65]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[  66]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 53] CALL  sec[  67..  76]  9 dw  START|2 header-ful opcode packets
          sec[  67] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[  68] TAG 79787783
            sec[  69]  a 0x120c TEXUNIT3_CTL       = 00040108  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x8; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[  70]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[  71]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[  72]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[  73] TAG 21217170
            sec[  74]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[  75]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 58] CALL  sec[  76.. 107] 31 dw  START|2 header-ful opcode packets
          sec[  76] PACKET 0000001d op=0x00 reglist  total=31 dw
          sec[  77] TAG 59605e5b
            sec[  78]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  79]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  80]  a 0x1180 DDA_1180           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  81]  a 0x1164 DDA_1164           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[  82] TAG 605e5a60
            sec[  83]  a 0x1180 DDA_1180           = 3f666666  [ESSENTIAL-3D]
                        0.9 (float)
            sec[  84]  a 0x1168 DDA_1168           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  85]  a 0x1178 DDA_1178           = 3f666666  [ESSENTIAL-3D]
                        0.9 (float)
            sec[  86]  a 0x1180 DDA_1180           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[  87] TAG 635e5d5b
            sec[  88]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  89]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  90]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  91]  a 0x118c DDA_118C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[  92] TAG 645e5d5b
            sec[  93]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  94]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  95]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  96]  a 0x1190 DDA_1190           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[  97] TAG 655e5d5b
            sec[  98]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[  99]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 100]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 101]  a 0x1194 DDA_1194           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 102] TAG 665f5d5b
            sec[ 103]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 104]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 105]  a 0x117c DDA_117C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 106]  a 0x1198 DDA_1198           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
ring[ 63] CALL  sec[ 107.. 118] 11 dw  START|2 header-ful opcode packets
          sec[ 107] PACKET 00000009 op=0x00 reglist  total=11 dw
          sec[ 108] TAG b2cbcac0
            sec[ 109]  b 0x1300 DWGCTL             = 005000f5  [2D/X-DROPPABLE]
                        [documented form: pipeline-priming pseudo-draw 2 (PARD 0x2ea32)]; 2D form: opcode 5=3D-only opcode, not in the 2D table; BOP 0xf; src slot 0; dst slot 0; bit22 colour-key transparency; bits 0x00100000 UNKNOWN
            sec[ 110]  a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 111]  a 0x132c SCISSOR_MAX        = 00010001  [ESSENTIAL-3D]
                        (y=1, x=1)
            sec[ 112]  a 0x12c8 DEPTH_STENCIL      = 00000000  [ESSENTIAL-3D]
                        depth test off; depth func NEVER; stencil off; stencil func NEVER; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 0
          sec[ 113] TAG 8988bfb9
            sec[ 114]  a 0x12e4 ALPHATEST          = 00000001  [ESSENTIAL-3D]
                        alpha test ENABLED; func NEVER; ref 0; mask[31:24]=0x00
            sec[ 115]  a 0x12fc RASTERMODE         = 00000000  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=0 bit3=0 bit4=0; bits[31:5]=0x0 UNKNOWN
            sec[ 116]  a 0x1220 SURF0_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 117]  a 0x1224 SURF0_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[ 68] CALL  sec[ 118.. 127]  9 dw  START|2 header-ful opcode packets
          sec[ 118] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 119] TAG 79787780
            sec[ 120]  a 0x1200 TEXUNIT0_CTL       = 00050101  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x1; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 121]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 122]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 123]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 124] TAG 21217170
            sec[ 125]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 126]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 73] CALL  sec[ 127.. 136]  9 dw  START|2 header-ful opcode packets
          sec[ 127] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 128] TAG 79787781
            sec[ 129]  a 0x1204 TEXUNIT1_CTL       = 00050102  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x2; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 130]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 131]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 132]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 133] TAG 21217170
            sec[ 134]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 135]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 78] CALL  sec[ 136.. 145]  9 dw  START|2 header-ful opcode packets
          sec[ 136] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 137] TAG 79787782
            sec[ 138]  a 0x1208 TEXUNIT2_CTL       = 00050104  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x4; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 139]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 140]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 141]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 142] TAG 21217170
            sec[ 143]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 144]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 83] CALL  sec[ 145.. 154]  9 dw  START|2 header-ful opcode packets
          sec[ 145] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 146] TAG 79787783
            sec[ 147]  a 0x120c TEXUNIT3_CTL       = 00050108  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x8; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 148]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 149]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 150]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 151] TAG 21217170
            sec[ 152]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 153]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[ 88] CALL  sec[ 154.. 185] 31 dw  START|2 header-ful opcode packets
          sec[ 154] PACKET 0000001d op=0x00 reglist  total=31 dw
          sec[ 155] TAG 59605e5b
            sec[ 156]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 157]  a 0x1178 DDA_1178           = 41200000  [ESSENTIAL-3D]
                        10 (float)
            sec[ 158]  a 0x1180 DDA_1180           = 41200000  [ESSENTIAL-3D]
                        10 (float)
            sec[ 159]  a 0x1164 DDA_1164           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 160] TAG 2020605e
            sec[ 161]  a 0x1178 DDA_1178           = 41200000  [ESSENTIAL-3D]
                        10 (float)
            sec[ 162]  a 0x1180 DDA_1180           = 41300000  [ESSENTIAL-3D]
                        11 (float)
            sec[ 163]  PAD          <- 00000000 (discarded)
            sec[ 164]  PAD          <- 00000000 (discarded)
          sec[ 165] TAG 635e5d5b
            sec[ 166]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 167]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 168]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 169]  a 0x118c DDA_118C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 170] TAG 645e5d5b
            sec[ 171]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 172]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 173]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 174]  a 0x1190 DDA_1190           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 175] TAG 655e5d5b
            sec[ 176]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 177]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 178]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 179]  a 0x1194 DDA_1194           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 180] TAG 665f5d5b
            sec[ 181]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 182]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 183]  a 0x117c DDA_117C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 184]  a 0x1198 DDA_1198           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
ring[ 93] CALL  sec[ 185.. 191]  6 dw  START|2 header-ful opcode packets
          sec[ 185] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 186] TAG c0afafc0
            sec[ 187]  s 0x1300 DWGCTL             = 00000207  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope fence RT surface B
            sec[ 188]  s 0x12bc DWGSYNC            = 00000111  [SYNC/FENCE]
                        tag 0x110; bit0=1 ARM block-until-retire
            sec[ 189]  s 0x12bc DWGSYNC            = 00000110  [SYNC/FENCE]
                        tag 0x110; bit0=0 value that lands in 0x41c (read &~7)
            sec[ 190]  b 0x1300 DWGCTL             = 005000f5  [2D/X-DROPPABLE]
                        [documented form: pipeline-priming pseudo-draw 2 (PARD 0x2ea32)]; 2D form: opcode 5=3D-only opcode, not in the 2D table; BOP 0xf; src slot 0; dst slot 0; bit22 colour-key transparency; bits 0x00100000 UNKNOWN
ring[ 98] CALL  sec[ 191.. 197]  6 dw  START|2 header-ful opcode packets
          sec[ 191] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 192] TAG bebfcbca
            sec[ 193]  a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 194]  a 0x132c SCISSOR_MAX        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 195]  a 0x12fc RASTERMODE         = 18004014  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=1 bit3=0 bit4=1; bits[31:5]=0xc00200 UNKNOWN
            sec[ 196]  a 0x12f8 AA_THRESHOLD       = 02800000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §3.5 (0x12f8 = the vendor parameter reg3DThresholdAA); no bit field documented
ring[103] CALL  sec[ 197.. 203]  6 dw  START|2 header-ful opcode packets
          sec[ 197] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 198] TAG bbbcb9ba
            sec[ 199]  a 0x12e8 BLENDCTL           = 00000000  [ESSENTIAL-3D]
                        blend off; srcRGB ZERO; dstRGB ZERO; srcA ZERO; dstA ZERO; eq ADD; eq2 ADD; dither bits[24:23]=0
            sec[ 200]  a 0x12e4 ALPHATEST          = 00000000  [ESSENTIAL-3D]
                        alpha test off; func NEVER; ref 0; mask[31:24]=0x00
            sec[ 201]  a 0x12f0 BLENDCONST23       = 00000000  [ESSENTIAL-3D]
                        two 10-bit components: 0x000, 0x000
            sec[ 202]  a 0x12ec BLENDCONST01       = 00000000  [ESSENTIAL-3D]
                        two 10-bit components: 0x000, 0x000
ring[108] CALL  sec[ 203.. 208]  5 dw  START|2 header-ful opcode packets
          sec[ 203] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 204] TAG 21b4b3b2
            sec[ 205]  a 0x12c8 DEPTH_STENCIL      = 00000000  [ESSENTIAL-3D]
                        depth test off; depth func NEVER; stencil off; stencil func NEVER; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 0
            sec[ 206]  a 0x12cc STENCIL_MASKS      = 00000000  [ESSENTIAL-3D]
                        compare mask 0x00; reference 0x00
            sec[ 207]  a 0x12d0 STENCIL_WRITE      = 00000000  [ESSENTIAL-3D]
                        write mask 0x00; depth write off
ring[113] CALL  sec[ 208.. 213]  5 dw  START|2 header-ful opcode packets
          sec[ 208] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 209] TAG 21b7b6b5
            sec[ 210]  a 0x12d4 FOG_CTL            = 00000000  [ESSENTIAL-3D]
                        fog off; mode bits[2:1]=0; bit4=0; B=0 G=0 R=0
            sec[ 211]  a 0x12d8 FOG_START          = 00000000  [ESSENTIAL-3D]
                        0.000000 (v/16777215)
            sec[ 212]  a 0x12dc FOG_END            = 00ffffff  [ESSENTIAL-3D]
                        1.000000 (v/16777215) = fog off
ring[118] CALL  sec[ 213.. 216]  3 dw  START|2 header-ful opcode packets
          sec[ 213] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 214] TAG 2121212c
            sec[ 215]  a 0x10b0 PRIMWORD           = 00000060  [ESSENTIAL-3D]
                        A=0x60 UNKNOWN; B=0 points; bit17 CLEAR - unexpected; bit18 clear
ring[123] CALL  sec[ 216.. 222]  6 dw  START|2 header-ful opcode packets
          sec[ 216] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 217] TAG 8d8c8988
            sec[ 218]  a 0x1220 SURF0_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 219]  a 0x1224 SURF0_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
            sec[ 220]  a 0x1230 SURF2_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 221]  a 0x1234 SURF2_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[128] CALL  sec[ 222.. 225]  3 dw  START|2 header-ful opcode packets
          sec[ 222] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 223] TAG 212121c9
            sec[ 224]  a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
ring[133] CALL  sec[ 225.. 228]  3 dw  START|2 header-ful opcode packets
          sec[ 225] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 226] TAG 212121c0
            sec[ 227]  b 0x1300 DWGCTL             = 00360004  [2D/X-DROPPABLE]
                        [documented form: idle / default (PARD ctor 0xc94bb)]; 2D form: opcode 4=3D-only opcode, not in the 2D table; BOP 0x0; src slot 0; dst slot 0; bits[19:12]=0x60 - pattern fields only defined for opcode 0, meaning here UNKNOWN; bits 0x00300000 UNKNOWN
ring[138] CALL  sec[ 228.. 232]  4 dw  START|2 header-ful opcode packets
          sec[ 228] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 229] TAG 2121dd87
            sec[ 230]  a 0x121c TEXENGINE_LATCH    = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3 (0x121c): the kernel writes 0 before the unit block, [INF] a latch; MTXPARD has no writer
            sec[ 231]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
ring[143] CALL  sec[ 232.. 238]  6 dw  START|2 header-ful opcode packets
          sec[ 232] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 233] TAG 83828180
            sec[ 234]  a 0x1200 TEXUNIT0_CTL       = 0001000f  [ESSENTIAL-3D]
                        pipe select (one-hot) 0xf; bits[11:8]=0x0; state[18:16]=1 disabled
            sec[ 235]  a 0x1204 TEXUNIT1_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
            sec[ 236]  a 0x1208 TEXUNIT2_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
            sec[ 237]  a 0x120c TEXUNIT3_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
          ring[148] TAG 72717080  (inline ring tag)
                       a 0x1200 TEXUNIT0_CTL       = 00040101  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x1; bits[11:8]=0x1; state[18:16]=4 enabled 2D
                       a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
                       a 0x11c4 TEX_FILTER         = 00000249  [ESSENTIAL-3D]
                        f0=1 f1=1 f2=1 border-en=1; 0x249 not in the documented set - UNKNOWN
                       a 0x11c8 TEX_REG_11C8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[153] TAG 76757473  (inline ring tag)
                       a 0x11cc TEX_BORDER         = ffffffff  [ESSENTIAL-3D]
                        border colour 0xffffffff
                       a 0x11d0 TEX_BORDER_MASK    = 00000000  [ESSENTIAL-3D]
                        border mask 0x00000000
                       a 0x11d4 TEX_REG_11D4       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
                       a 0x11d8 TEX_REG_11D8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[158] TAG 72717081  (inline ring tag)
                       a 0x1204 TEXUNIT1_CTL       = 00040102  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x2; bits[11:8]=0x1; state[18:16]=4 enabled 2D
                       a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
                       a 0x11c4 TEX_FILTER         = 00000249  [ESSENTIAL-3D]
                        f0=1 f1=1 f2=1 border-en=1; 0x249 not in the documented set - UNKNOWN
                       a 0x11c8 TEX_REG_11C8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[163] TAG 76757473  (inline ring tag)
                       a 0x11cc TEX_BORDER         = ffffffff  [ESSENTIAL-3D]
                        border colour 0xffffffff
                       a 0x11d0 TEX_BORDER_MASK    = 00000000  [ESSENTIAL-3D]
                        border mask 0x00000000
                       a 0x11d4 TEX_REG_11D4       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
                       a 0x11d8 TEX_REG_11D8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[168] TAG 72717082  (inline ring tag)
                       a 0x1208 TEXUNIT2_CTL       = 00040104  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x4; bits[11:8]=0x1; state[18:16]=4 enabled 2D
                       a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
                       a 0x11c4 TEX_FILTER         = 00000249  [ESSENTIAL-3D]
                        f0=1 f1=1 f2=1 border-en=1; 0x249 not in the documented set - UNKNOWN
                       a 0x11c8 TEX_REG_11C8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[173] TAG 76757473  (inline ring tag)
                       a 0x11cc TEX_BORDER         = ffffffff  [ESSENTIAL-3D]
                        border colour 0xffffffff
                       a 0x11d0 TEX_BORDER_MASK    = 00000000  [ESSENTIAL-3D]
                        border mask 0x00000000
                       a 0x11d4 TEX_REG_11D4       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
                       a 0x11d8 TEX_REG_11D8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[178] TAG 72717083  (inline ring tag)
                       a 0x120c TEXUNIT3_CTL       = 00040108  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x8; bits[11:8]=0x1; state[18:16]=4 enabled 2D
                       a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
                       a 0x11c4 TEX_FILTER         = 00000249  [ESSENTIAL-3D]
                        f0=1 f1=1 f2=1 border-en=1; 0x249 not in the documented set - UNKNOWN
                       a 0x11c8 TEX_REG_11C8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[183] TAG 76757473  (inline ring tag)
                       a 0x11cc TEX_BORDER         = ffffffff  [ESSENTIAL-3D]
                        border colour 0xffffffff
                       a 0x11d0 TEX_BORDER_MASK    = 00000000  [ESSENTIAL-3D]
                        border mask 0x00000000
                       a 0x11d4 TEX_REG_11D4       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
                       a 0x11d8 TEX_REG_11D8       = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3: every vendor filler writes 0; meaning not documented
          ring[188] TAG 212121dd  (inline ring tag)
                       s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          ring[190] TAG 83828180  (inline ring tag)
                       a 0x1200 TEXUNIT0_CTL       = 0001000f  [ESSENTIAL-3D]
                        pipe select (one-hot) 0xf; bits[11:8]=0x0; state[18:16]=1 disabled
                       a 0x1204 TEXUNIT1_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
                       a 0x1208 TEXUNIT2_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
                       a 0x120c TEXUNIT3_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
          ring[195] TAG 212121c0  (inline ring tag)
                       b 0x1300 DWGCTL             = 002010cc  [2D/X-DROPPABLE]
                        [documented form: 3D primitive form paired with 0x12f4 = 31.0 (mtx2393)]; 2D form: opcode 12=textured rect (DPS); BOP 0xc (COPY); src slot 0; dst slot 0; bits[19:12]=0x01 - pattern fields only defined for opcode 0, meaning here UNKNOWN; bits 0x00200000 UNKNOWN
          ring[197] TAG 9a989b99  (inline ring tag)
                       a 0x1264 COMB_RGB_SRC       = 00000080  [ESSENTIAL-3D]
                        slot 0; A=r0 B=t0 C=t0
                       a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
                       a 0x1260 COMB_ALPHA_SRC     = 00000080  [ESSENTIAL-3D]
                        slot 0; A=r0 B=t0 C=t0
                       a 0x1268 COMB_ALPHA_DST     = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          ring[202] TAG 9a989b99  (inline ring tag)
                       a 0x1264 COMB_RGB_SRC       = 00000082  [ESSENTIAL-3D]
                        slot 2; A=r0 B=t0 C=t0
                       a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
                       a 0x1260 COMB_ALPHA_SRC     = 00000080  [ESSENTIAL-3D]
                        slot 0; A=r0 B=t0 C=t0
                       a 0x1268 COMB_ALPHA_DST     = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          ring[207] TAG 9a989b99  (inline ring tag)
                       a 0x1264 COMB_RGB_SRC       = 00000084  [ESSENTIAL-3D]
                        slot 4; A=r0 B=t0 C=t0
                       a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
                       a 0x1260 COMB_ALPHA_SRC     = 00000080  [ESSENTIAL-3D]
                        slot 0; A=r0 B=t0 C=t0
                       a 0x1268 COMB_ALPHA_DST     = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          ring[212] TAG 9a989b99  (inline ring tag)
                       a 0x1264 COMB_RGB_SRC       = 00000088  [ESSENTIAL-3D]
                        slot 8; A=r0 B=t0 C=t0
                       a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
                       a 0x1260 COMB_ALPHA_SRC     = 00000080  [ESSENTIAL-3D]
                        slot 0; A=r0 B=t0 C=t0
                       a 0x1268 COMB_ALPHA_DST     = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          ring[217] TAG 9a989b99  (inline ring tag)
                       a 0x1264 COMB_RGB_SRC       = 0000008a  [ESSENTIAL-3D]
                        slot 10; A=r0 B=t0 C=t0
                       a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
                       a 0x1260 COMB_ALPHA_SRC     = 00000080  [ESSENTIAL-3D]
                        slot 0; A=r0 B=t0 C=t0
                       a 0x1268 COMB_ALPHA_DST     = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          ring[222] TAG bab1b0b9  (inline ring tag)
                       a 0x12e4 ALPHATEST          = 00000000  [ESSENTIAL-3D]
                        alpha test off; func NEVER; ref 0; mask[31:24]=0x00
                       a 0x12c0 ZCLIP_MIN          = ffffffff  [ESSENTIAL-3D]
                        0xffffffff [INF min depth]
                       a 0x12c4 ZCLIP_MAX          = 7fffffff  [ESSENTIAL-3D]
                        0x7fffffff [INF max depth]
                       a 0x12e8 BLENDCTL           = 00000000  [ESSENTIAL-3D]
                        blend off; srcRGB ZERO; dstRGB ZERO; srcA ZERO; dstA ZERO; eq ADD; eq2 ADD; dither bits[24:23]=0
          ring[227] TAG b4b3b2b5  (inline ring tag)
                       a 0x12d4 FOG_CTL            = 00000000  [ESSENTIAL-3D]
                        fog off; mode bits[2:1]=0; bit4=0; B=0 G=0 R=0
                       a 0x12c8 DEPTH_STENCIL      = 00000000  [ESSENTIAL-3D]
                        depth test off; depth func NEVER; stencil off; stencil func NEVER; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 0
                       a 0x12cc STENCIL_MASKS      = 0000ffff  [ESSENTIAL-3D]
                        compare mask 0xff; reference 0xff
                       a 0x12d0 STENCIL_WRITE      = 000001ff  [ESSENTIAL-3D]
                        write mask 0xff; depth write ENABLED
          ring[232] TAG bfc5c6c9  (inline ring tag)
                       a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
                       b 0x1318 BCOL               = 00000000  [2D/X-DROPPABLE]
                        background colour 0x00000000
                       b 0x1314 FCOL               = 00000000  [2D/X-DROPPABLE]
                        foreground colour 0x00000000
                       a 0x12fc RASTERMODE         = 00000000  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=0 bit3=0 bit4=0; bits[31:5]=0x0 UNKNOWN
          ring[237] TAG c2c1cbca  (inline ring tag)
                       a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
                       a 0x132c SCISSOR_MAX        = 10001000  [ESSENTIAL-3D]
                        (y=4096, x=4096)
                       b 0x1304 SRC_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
                       b 0x1308 DST_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
          ring[242] TAG 212121bd  (inline ring tag)
                       a 0x12f4 GUARDBAND          = 41f80000  [ESSENTIAL-3D]
                        31 (float)
          ring[244] TAG afafddc0  (inline ring tag)
                       s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
                       s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
                       s 0x12bc DWGSYNC            = 00000119  [SYNC/FENCE]
                        tag 0x118; bit0=1 ARM block-until-retire
                       s 0x12bc DWGSYNC            = 00000118  [SYNC/FENCE]
                        tag 0x118; bit0=0 value that lands in 0x41c (read &~7)
          ring[249] TAG 212121ae  (inline ring tag)
                       s 0x12b8 EOBTAG             = 00070601  [SYNC/FENCE]
                        UNKNOWN - 3d-submission-audit A.5 (0x12b8, the vendor "EOBTag" patch); only the constant 0x00070601 is known
          ring[251] TAG afafddc0  (inline ring tag)
                       s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
                       s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
                       s 0x12bc DWGSYNC            = 00000121  [SYNC/FENCE]
                        tag 0x120; bit0=1 ARM block-until-retire
                       s 0x12bc DWGSYNC            = 00000120  [SYNC/FENCE]
                        tag 0x120; bit0=0 value that lands in 0x41c (read &~7)
          ring[256] TAG 212121ac  (inline ring tag)
                       a 0x12b0 REG_12B0           = 3fff8000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §3.5 / 3d-submission-audit A.5: the values 0x3fff8842 and 0x3fff8000 are traced to MTXPARD and mtx398, but the meaning of no bit is documented
ring[258] CALL  sec[ 238.. 263] 25 dw  START|0 header-less bare tags
          sec[ 238] TAG 21afddc0  (bare-tag section (start|0))
            sec[ 239]  s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
            sec[ 240]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
            sec[ 241]  s 0x12bc DWGSYNC            = 000000f9  [SYNC/FENCE]
                        tag 0xf8; bit0=1 ARM block-until-retire
          sec[ 242] TAG 21218b8a  (bare-tag section (start|0))
            sec[ 243]  a 0x1228 SURF1_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 244]  a 0x122c SURF1_CFG          = 30030780  [ESSENTIAL-3D]
                        pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0
          sec[ 245] TAG 21218b8a  (bare-tag section (start|0))
            sec[ 246]  a 0x1228 SURF1_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 247]  a 0x122c SURF1_CFG          = 30030780  [ESSENTIAL-3D]
                        pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0
          sec[ 248] TAG 212121c6  (bare-tag section (start|0))
            sec[ 249]  b 0x1318 BCOL               = 00ffffff  [2D/X-DROPPABLE]
                        background colour 0x00ffffff
          sec[ 250] TAG 212121c5  (bare-tag section (start|0))
            sec[ 251]  b 0x1314 FCOL               = 00000000  [2D/X-DROPPABLE]
                        foreground colour 0x00000000
          sec[ 252] TAG 2121c8c7  (bare-tag section (start|0))
            sec[ 253]  b 0x131c PAT0               = 77ddbbee  [2D/X-DROPPABLE]
                        pattern rows 0-3 = ee bb dd 77
            sec[ 254]  b 0x1320 PAT1               = 77ddbbee  [2D/X-DROPPABLE]
                        pattern rows 4-7 = ee bb dd 77
          sec[ 255] TAG 212121c9  (bare-tag section (start|0))
            sec[ 256]  a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
          sec[ 257] TAG 212121c0  (bare-tag section (start|0))
            sec[ 258]  b 0x1300 DWGCTL             = 000024c0  [2D/X-DROPPABLE]
                        2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 0; dst slot 1; bit13 8x8 pattern enable
          sec[ 259] TAG 21e3c2c0  (bare-tag section (start|0))
            sec[ 260]  b 0x1300 DWGCTL             = 000024c0  [2D/X-DROPPABLE]
                        2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 0; dst slot 1; bit13 8x8 pattern enable
            sec[ 261]  b 0x1308 DST_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
            sec[ 262]  b 0x138c LEN                = 00f00140  [2D/X-DROPPABLE]
                        height 240, width 320
ring[263] CALL  sec[ 263.. 267]  4 dw  START|2 header-ful opcode packets
          sec[ 263] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 264] TAG 2121c0c0
            sec[ 265]  s 0x1300 DWGCTL             = 00000307  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope fence RT surface A+fence RT surface B
            sec[ 266]  b 0x1300 DWGCTL             = 000024c0  [2D/X-DROPPABLE]
                        2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 0; dst slot 1; bit13 8x8 pattern enable
ring[268] CALL  sec[ 267.. 275]  8 dw  START|2 header-ful opcode packets
          sec[ 267] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[ 268] TAG 21afc0c0
            sec[ 269]  b 0x1300 DWGCTL             = 00000004  [2D/X-DROPPABLE]
                        2D form: opcode 4=3D-only opcode, not in the 2D table; BOP 0x0; src slot 0; dst slot 0
            sec[ 270]  s 0x1300 DWGCTL             = 00000407  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain
            sec[ 271]  s 0x12bc DWGSYNC            = 00000131  [SYNC/FENCE]
                        tag 0x130; bit0=1 ARM block-until-retire
          sec[ 272] TAG 2121c0af
            sec[ 273]  s 0x12bc DWGSYNC            = 00000130  [SYNC/FENCE]
                        tag 0x130; bit0=0 value that lands in 0x41c (read &~7)
            sec[ 274]  b 0x1300 DWGCTL             = 000024c0  [2D/X-DROPPABLE]
                        2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 0; dst slot 1; bit13 8x8 pattern enable
ring[273] CALL  sec[ 275.. 278]  3 dw  START|2 header-ful opcode packets
          sec[ 275] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 276] TAG 212121de
            sec[ 277]  b 0x1378 REG_1378           = 00000000  [2D/X-DROPPABLE]
                        UNKNOWN - not documented anywhere in docs/
ring[278] CALL  sec[ 278.. 282]  4 dw  START|2 header-ful opcode packets
          sec[ 278] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 279] TAG 2121b5c9
            sec[ 280]  a 0x1324 PLNWT              = 00000000  [ESSENTIAL-3D]
                        plane write mask 0x00000000
            sec[ 281]  a 0x12d4 FOG_CTL            = 00000000  [ESSENTIAL-3D]
                        fog off; mode bits[2:1]=0; bit4=0; B=0 G=0 R=0
ring[283] CALL  sec[ 282.. 293] 11 dw  START|2 header-ful opcode packets
          sec[ 282] PACKET 00000009 op=0x00 reglist  total=11 dw
          sec[ 283] TAG b2cbcac0
            sec[ 284]  b 0x1300 DWGCTL             = 001000f4  [2D/X-DROPPABLE]
                        [documented form: pipeline-priming pseudo-draw 1 (PARD 0x2e832)]; 2D form: opcode 4=3D-only opcode, not in the 2D table; BOP 0xf; src slot 0; dst slot 0; bits 0x00100000 UNKNOWN
            sec[ 285]  a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 286]  a 0x132c SCISSOR_MAX        = 00010001  [ESSENTIAL-3D]
                        (y=1, x=1)
            sec[ 287]  a 0x12c8 DEPTH_STENCIL      = 00000000  [ESSENTIAL-3D]
                        depth test off; depth func NEVER; stencil off; stencil func NEVER; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 0
          sec[ 288] TAG 8988bfb9
            sec[ 289]  a 0x12e4 ALPHATEST          = 00000001  [ESSENTIAL-3D]
                        alpha test ENABLED; func NEVER; ref 0; mask[31:24]=0x00
            sec[ 290]  a 0x12fc RASTERMODE         = 00000000  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=0 bit3=0 bit4=0; bits[31:5]=0x0 UNKNOWN
            sec[ 291]  a 0x1220 SURF0_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 292]  a 0x1224 SURF0_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[288] CALL  sec[ 293.. 302]  9 dw  START|2 header-ful opcode packets
          sec[ 293] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 294] TAG 79787780
            sec[ 295]  a 0x1200 TEXUNIT0_CTL       = 00040101  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x1; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[ 296]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 297]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 298]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 299] TAG 21217170
            sec[ 300]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 301]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[293] CALL  sec[ 302.. 311]  9 dw  START|2 header-ful opcode packets
          sec[ 302] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 303] TAG 79787781
            sec[ 304]  a 0x1204 TEXUNIT1_CTL       = 00040102  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x2; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[ 305]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 306]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 307]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 308] TAG 21217170
            sec[ 309]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 310]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[298] CALL  sec[ 311.. 320]  9 dw  START|2 header-ful opcode packets
          sec[ 311] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 312] TAG 79787782
            sec[ 313]  a 0x1208 TEXUNIT2_CTL       = 00040104  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x4; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[ 314]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 315]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 316]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 317] TAG 21217170
            sec[ 318]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 319]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[303] CALL  sec[ 320.. 329]  9 dw  START|2 header-ful opcode packets
          sec[ 320] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 321] TAG 79787783
            sec[ 322]  a 0x120c TEXUNIT3_CTL       = 00040108  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x8; bits[11:8]=0x1; state[18:16]=4 enabled 2D
            sec[ 323]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 324]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 325]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 326] TAG 21217170
            sec[ 327]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 328]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[308] CALL  sec[ 329.. 360] 31 dw  START|2 header-ful opcode packets
          sec[ 329] PACKET 0000001d op=0x00 reglist  total=31 dw
          sec[ 330] TAG 59605e5b
            sec[ 331]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 332]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 333]  a 0x1180 DDA_1180           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 334]  a 0x1164 DDA_1164           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 335] TAG 605e5a60
            sec[ 336]  a 0x1180 DDA_1180           = 3f666666  [ESSENTIAL-3D]
                        0.9 (float)
            sec[ 337]  a 0x1168 DDA_1168           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 338]  a 0x1178 DDA_1178           = 3f666666  [ESSENTIAL-3D]
                        0.9 (float)
            sec[ 339]  a 0x1180 DDA_1180           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 340] TAG 635e5d5b
            sec[ 341]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 342]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 343]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 344]  a 0x118c DDA_118C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 345] TAG 645e5d5b
            sec[ 346]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 347]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 348]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 349]  a 0x1190 DDA_1190           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 350] TAG 655e5d5b
            sec[ 351]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 352]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 353]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 354]  a 0x1194 DDA_1194           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 355] TAG 665f5d5b
            sec[ 356]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 357]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 358]  a 0x117c DDA_117C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 359]  a 0x1198 DDA_1198           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
ring[313] CALL  sec[ 360.. 371] 11 dw  START|2 header-ful opcode packets
          sec[ 360] PACKET 00000009 op=0x00 reglist  total=11 dw
          sec[ 361] TAG b2cbcac0
            sec[ 362]  b 0x1300 DWGCTL             = 005000f5  [2D/X-DROPPABLE]
                        [documented form: pipeline-priming pseudo-draw 2 (PARD 0x2ea32)]; 2D form: opcode 5=3D-only opcode, not in the 2D table; BOP 0xf; src slot 0; dst slot 0; bit22 colour-key transparency; bits 0x00100000 UNKNOWN
            sec[ 363]  a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 364]  a 0x132c SCISSOR_MAX        = 00010001  [ESSENTIAL-3D]
                        (y=1, x=1)
            sec[ 365]  a 0x12c8 DEPTH_STENCIL      = 00000000  [ESSENTIAL-3D]
                        depth test off; depth func NEVER; stencil off; stencil func NEVER; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 0
          sec[ 366] TAG 8988bfb9
            sec[ 367]  a 0x12e4 ALPHATEST          = 00000001  [ESSENTIAL-3D]
                        alpha test ENABLED; func NEVER; ref 0; mask[31:24]=0x00
            sec[ 368]  a 0x12fc RASTERMODE         = 00000000  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=0 bit3=0 bit4=0; bits[31:5]=0x0 UNKNOWN
            sec[ 369]  a 0x1220 SURF0_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 370]  a 0x1224 SURF0_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[318] CALL  sec[ 371.. 380]  9 dw  START|2 header-ful opcode packets
          sec[ 371] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 372] TAG 79787780
            sec[ 373]  a 0x1200 TEXUNIT0_CTL       = 00050101  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x1; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 374]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 375]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 376]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 377] TAG 21217170
            sec[ 378]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 379]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[323] CALL  sec[ 380.. 389]  9 dw  START|2 header-ful opcode packets
          sec[ 380] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 381] TAG 79787781
            sec[ 382]  a 0x1204 TEXUNIT1_CTL       = 00050102  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x2; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 383]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 384]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 385]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 386] TAG 21217170
            sec[ 387]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 388]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[328] CALL  sec[ 389.. 398]  9 dw  START|2 header-ful opcode packets
          sec[ 389] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 390] TAG 79787782
            sec[ 391]  a 0x1208 TEXUNIT2_CTL       = 00050104  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x4; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 392]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 393]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 394]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 395] TAG 21217170
            sec[ 396]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 397]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[333] CALL  sec[ 398.. 407]  9 dw  START|2 header-ful opcode packets
          sec[ 398] PACKET 00000007 op=0x00 reglist  total=9 dw
          sec[ 399] TAG 79787783
            sec[ 400]  a 0x120c TEXUNIT3_CTL       = 00050108  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x8; bits[11:8]=0x1; state[18:16]=5 enabled 2D + projective
            sec[ 401]  a 0x11dc TEX_BASE           = 00000000  [ESSENTIAL-3D]
                        VRAM byte offset 0x00000000 | space flags 0
            sec[ 402]  a 0x11e0 TEX_WIDTH_FMT      = 32020001  [ESSENTIAL-3D]
                        width 1 px | tiling 2 | format 0x32=ARGB8888
            sec[ 403]  a 0x11e4 TEX_HEIGHT_DEPTH   = 00010001  [ESSENTIAL-3D]
                        height 1 | depth 1
          sec[ 404] TAG 21217170
            sec[ 405]  a 0x11c0 TEX_ADDRMODE       = 00000000  [ESSENTIAL-3D]
                        base 0x1000 ABSENT; U=0 V=0 W=0 (1=?mirror 2=?clamp 0=repeat, [INF])
            sec[ 406]  a 0x11c4 TEX_FILTER         = 00000000  [ESSENTIAL-3D]
                        f0=0 f1=0 f2=0 border-en=0
ring[338] CALL  sec[ 407.. 438] 31 dw  START|2 header-ful opcode packets
          sec[ 407] PACKET 0000001d op=0x00 reglist  total=31 dw
          sec[ 408] TAG 59605e5b
            sec[ 409]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 410]  a 0x1178 DDA_1178           = 41200000  [ESSENTIAL-3D]
                        10 (float)
            sec[ 411]  a 0x1180 DDA_1180           = 41200000  [ESSENTIAL-3D]
                        10 (float)
            sec[ 412]  a 0x1164 DDA_1164           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 413] TAG 2020605e
            sec[ 414]  a 0x1178 DDA_1178           = 41200000  [ESSENTIAL-3D]
                        10 (float)
            sec[ 415]  a 0x1180 DDA_1180           = 41300000  [ESSENTIAL-3D]
                        11 (float)
            sec[ 416]  PAD          <- 00000000 (discarded)
            sec[ 417]  PAD          <- 00000000 (discarded)
          sec[ 418] TAG 635e5d5b
            sec[ 419]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 420]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 421]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 422]  a 0x118c DDA_118C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 423] TAG 645e5d5b
            sec[ 424]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 425]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 426]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 427]  a 0x1190 DDA_1190           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 428] TAG 655e5d5b
            sec[ 429]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 430]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 431]  a 0x1178 DDA_1178           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 432]  a 0x1194 DDA_1194           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
          sec[ 433] TAG 665f5d5b
            sec[ 434]  a 0x116c DDA_116C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 435]  a 0x1174 DDA_1174           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 436]  a 0x117c DDA_117C           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 437]  a 0x1198 DDA_1198           = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
ring[343] CALL  sec[ 438.. 444]  6 dw  START|2 header-ful opcode packets
          sec[ 438] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 439] TAG c0afafc0
            sec[ 440]  s 0x1300 DWGCTL             = 00000207  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope fence RT surface B
            sec[ 441]  s 0x12bc DWGSYNC            = 00000139  [SYNC/FENCE]
                        tag 0x138; bit0=1 ARM block-until-retire
            sec[ 442]  s 0x12bc DWGSYNC            = 00000138  [SYNC/FENCE]
                        tag 0x138; bit0=0 value that lands in 0x41c (read &~7)
            sec[ 443]  b 0x1300 DWGCTL             = 005000f5  [2D/X-DROPPABLE]
                        [documented form: pipeline-priming pseudo-draw 2 (PARD 0x2ea32)]; 2D form: opcode 5=3D-only opcode, not in the 2D table; BOP 0xf; src slot 0; dst slot 0; bit22 colour-key transparency; bits 0x00100000 UNKNOWN
ring[348] CALL  sec[ 444.. 450]  6 dw  START|2 header-ful opcode packets
          sec[ 444] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 445] TAG bebfcbca
            sec[ 446]  a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 447]  a 0x132c SCISSOR_MAX        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 448]  a 0x12fc RASTERMODE         = 18004014  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=1 bit3=0 bit4=1; bits[31:5]=0xc00200 UNKNOWN
            sec[ 449]  a 0x12f8 AA_THRESHOLD       = 02800000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §3.5 (0x12f8 = the vendor parameter reg3DThresholdAA); no bit field documented
ring[353] CALL  sec[ 450.. 456]  6 dw  START|2 header-ful opcode packets
          sec[ 450] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 451] TAG bbbcb9ba
            sec[ 452]  a 0x12e8 BLENDCTL           = 00000000  [ESSENTIAL-3D]
                        blend off; srcRGB ZERO; dstRGB ZERO; srcA ZERO; dstA ZERO; eq ADD; eq2 ADD; dither bits[24:23]=0
            sec[ 453]  a 0x12e4 ALPHATEST          = 00000000  [ESSENTIAL-3D]
                        alpha test off; func NEVER; ref 0; mask[31:24]=0x00
            sec[ 454]  a 0x12f0 BLENDCONST23       = 00000000  [ESSENTIAL-3D]
                        two 10-bit components: 0x000, 0x000
            sec[ 455]  a 0x12ec BLENDCONST01       = 00000000  [ESSENTIAL-3D]
                        two 10-bit components: 0x000, 0x000
ring[358] CALL  sec[ 456.. 464]  8 dw  START|2 header-ful opcode packets
          sec[ 456] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[ 457] TAG 21b4b3b2
            sec[ 458]  a 0x12c8 DEPTH_STENCIL      = 00000000  [ESSENTIAL-3D]
                        depth test off; depth func NEVER; stencil off; stencil func NEVER; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 0
            sec[ 459]  a 0x12cc STENCIL_MASKS      = 00000000  [ESSENTIAL-3D]
                        compare mask 0x00; reference 0x00
            sec[ 460]  a 0x12d0 STENCIL_WRITE      = 00000000  [ESSENTIAL-3D]
                        write mask 0x00; depth write off
          sec[ 461] TAG 2121b1b0
            sec[ 462]  a 0x12c0 ZCLIP_MIN          = 80000000  [ESSENTIAL-3D]
                        0x80000000 [INF min depth]
            sec[ 463]  a 0x12c4 ZCLIP_MAX          = 7fffffff  [ESSENTIAL-3D]
                        0x7fffffff [INF max depth]
ring[363] CALL  sec[ 464.. 469]  5 dw  START|2 header-ful opcode packets
          sec[ 464] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 465] TAG 21b7b6b5
            sec[ 466]  a 0x12d4 FOG_CTL            = 00000000  [ESSENTIAL-3D]
                        fog off; mode bits[2:1]=0; bit4=0; B=0 G=0 R=0
            sec[ 467]  a 0x12d8 FOG_START          = 00000000  [ESSENTIAL-3D]
                        0.000000 (v/16777215)
            sec[ 468]  a 0x12dc FOG_END            = 00ffffff  [ESSENTIAL-3D]
                        1.000000 (v/16777215) = fog off
ring[368] CALL  sec[ 469.. 475]  6 dw  START|2 header-ful opcode packets
          sec[ 469] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 470] TAG 4b4a4948
            sec[ 471]  a 0x1120 VTXDEFAULT_x       = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 472]  a 0x1124 VTXDEFAULT_y       = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 473]  a 0x1128 VTXDEFAULT_z       = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 474]  a 0x112c VTXDEFAULT_w       = 3f800000  [ESSENTIAL-3D]
                        1 (float)
ring[373] CALL  sec[ 475.. 480]  5 dw  START|2 header-ful opcode packets
          sec[ 475] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 476] TAG 21464544
            sec[ 477]  a 0x1110 VTXFMT0            = 00000f27  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x44 (named "vertex-attribute format", S[0x61c]|S[0x608]); bit layout undocumented
            sec[ 478]  a 0x1114 VTXFMT1            = 00287210  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x45 (S[0x60c]); bit layout undocumented
            sec[ 479]  a 0x1118 VTXFMT2            = 00006543  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x46 (S[0x610]); bit layout undocumented
ring[378] CALL  sec[ 480.. 488]  8 dw  START|2 header-ful opcode packets
          sec[ 480] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[ 481] TAG 212121dd
            sec[ 482]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          sec[ 483] TAG 3e403c3c
            sec[ 484]  a 0x10f0 WARP_MODE          = 20000000  [ESSENTIAL-3D]
                        mode[3:0]=0x0 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 485]  a 0x10f0 WARP_MODE          = 20000000  [ESSENTIAL-3D]
                        mode[3:0]=0x0 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 486]  a 0x1100 WARP_ENTRY         = 00000002  [ESSENTIAL-3D]
                        literal 2 - too small to be an entry (warp-binding documents 0x10f8 = 2 as the "unbound" marker)
            sec[ 487]  a 0x10f8 WARP_ENTRY_OBJ2    = 00000002  [ESSENTIAL-3D]
                        literal 2 - too small to be an entry (warp-binding documents 0x10f8 = 2 as the "unbound" marker)
ring[383] CALL  sec[ 488.. 501] 13 dw  START|2 header-ful opcode packets
          sec[ 488] PACKET 0000000b op=0x00 reglist  total=13 dw
          sec[ 489] TAG 212121dd
            sec[ 490]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          sec[ 491] TAG 33323c3c
            sec[ 492]  a 0x10f0 WARP_MODE          = 20000000  [ESSENTIAL-3D]
                        mode[3:0]=0x0 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 493]  a 0x10f0 WARP_MODE          = 20000000  [ESSENTIAL-3D]
                        mode[3:0]=0x0 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 494]  a 0x10c8 WARP_STAGE2_ENTRY  = 00000002  [ESSENTIAL-3D]
                        literal 2 - too small to be an entry (warp-binding documents 0x10f8 = 2 as the "unbound" marker)
            sec[ 495]  a 0x10cc WARP_STAGE3_ENTRY  = 00000002  [ESSENTIAL-3D]
                        literal 2 - too small to be an entry (warp-binding documents 0x10f8 = 2 as the "unbound" marker)
          sec[ 496] TAG 30363534
            sec[ 497]  a 0x10d0 WARP_STAGE4_ENTRY  = 00000002  [ESSENTIAL-3D]
                        literal 2 - too small to be an entry (warp-binding documents 0x10f8 = 2 as the "unbound" marker)
            sec[ 498]  a 0x10d4 WARP_STAGE5_ENTRY  = 00000002  [ESSENTIAL-3D]
                        literal 2 - too small to be an entry (warp-binding documents 0x10f8 = 2 as the "unbound" marker)
            sec[ 499]  a 0x10d8 WARP_STAGE6_ENTRY  = 00000000  [ESSENTIAL-3D]
                        null
            sec[ 500]  a 0x10c0 WARP_STAGE0_ENTRY  = 00000000  [ESSENTIAL-3D]
                        null
ring[388] CALL  sec[ 501.. 512] 11 dw  START|2 header-ful opcode packets
          sec[ 501] PACKET 00000009 op=0x00 reglist  total=11 dw
          sec[ 502] TAG 212121dd
            sec[ 503]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          sec[ 504] TAG 4c3c3c3c
            sec[ 505]  a 0x10f0 WARP_MODE          = 20000000  [ESSENTIAL-3D]
                        mode[3:0]=0x0 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 506]  a 0x10f0 WARP_MODE          = 20000000  [ESSENTIAL-3D]
                        mode[3:0]=0x0 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 507]  a 0x10f0 WARP_MODE          = 80000000  [ESSENTIAL-3D]
                        mode[3:0]=0x0 UNKNOWN; bit31 = software-WARP cache invalidate
            sec[ 508]  a 0x1130 WARP_PIPECTL       = 00002204  [ESSENTIAL-3D]
                        bit9 set (3d-submission-audit says mtx3400 ORs 0x200 in); every other bit UNKNOWN
          sec[ 509] TAG 21215147
            sec[ 510]  a 0x111c REG_111C           = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x47 (S[0x5f4]); role column blank
            sec[ 511]  a 0x1144 REG_1144           = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x51 (S[0x618]); role column blank
ring[393] CALL  sec[ 512.. 515]  3 dw  START|2 header-ful opcode packets
          sec[ 512] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 513] TAG 2121212c
            sec[ 514]  a 0x10b0 PRIMWORD           = 00000060  [ESSENTIAL-3D]
                        A=0x60 UNKNOWN; B=0 points; bit17 CLEAR - unexpected; bit18 clear
ring[398] CALL  sec[ 515.. 531] 16 dw  START|2 header-ful opcode packets
          sec[ 515] PACKET 0000000e op=0x00 reglist  total=16 dw
          sec[ 516] TAG 03020100
            sec[ 517]  a 0x1000 VTXBASE0           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 518]  a 0x1004 VTXBASE1           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 519]  a 0x1008 VTXBASE2           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 520]  a 0x100c VTXBASE3           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
          sec[ 521] TAG 07060504
            sec[ 522]  a 0x1010 VTXBASE4           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 523]  a 0x1014 VTXBASE5           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 524]  a 0x1018 VTXBASE6           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 525]  a 0x101c VTXBASE7           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
          sec[ 526] TAG 13121110
            sec[ 527]  a 0x1040 VTXSTRIDE0_1       = 00000000  [ESSENTIAL-3D]
                        stride pair: 0, 0 bytes
            sec[ 528]  a 0x1044 VTXSTRIDE2_3       = 00000000  [ESSENTIAL-3D]
                        stride pair: 0, 0 bytes
            sec[ 529]  a 0x1048 VTXSTRIDE4_5       = 00000000  [ESSENTIAL-3D]
                        stride pair: 0, 0 bytes
            sec[ 530]  a 0x104c VTXSTRIDE6_7       = 00000000  [ESSENTIAL-3D]
                        stride pair: 0, 0 bytes
ring[403] CALL  sec[ 531.. 535]  4 dw  START|2 header-ful opcode packets
          sec[ 531] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 532] TAG 21211819
            sec[ 533]  a 0x1064 VTXFIRST           = ffffffff  [ESSENTIAL-3D]
                        first usable index 4294967295 (=-1)
            sec[ 534]  a 0x1060 VTXCOUNT           = 00000000  [ESSENTIAL-3D]
                        0 vertices addressable
ring[408] CALL  sec[ 535.. 538]  3 dw  START|2 header-ful opcode packets
          sec[ 535] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 536] TAG 2121212d
            sec[ 537]  a 0x10b4 BLOCKPTR           = 00000000  [ESSENTIAL-3D]
                        block-file write pointer / attribute-port reset = 0
ring[413] CALL  sec[ 538.. 544]  6 dw  START|2 header-ful opcode packets
          sec[ 538] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 539] TAG 8d8c8988
            sec[ 540]  a 0x1220 SURF0_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 541]  a 0x1224 SURF0_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
            sec[ 542]  a 0x1230 SURF2_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 543]  a 0x1234 SURF2_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[418] CALL  sec[ 544.. 548]  4 dw  START|2 header-ful opcode packets
          sec[ 544] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 545] TAG 21218b8a
            sec[ 546]  c 0x1228 SURF1_BASE         = 02000000  [BOOT-POINTER]
                        base 0x02000000 | class 0
            sec[ 547]  a 0x122c SURF1_CFG          = 30030780  [ESSENTIAL-3D]
                        pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0
ring[423] CALL  sec[ 548.. 552]  4 dw  START|2 header-ful opcode packets
          sec[ 548] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 549] TAG 21218f8e
            sec[ 550]  a 0x1238 SURF3_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 551]  a 0x123c SURF3_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[428] CALL  sec[ 552.. 556]  4 dw  START|2 header-ful opcode packets
          sec[ 552] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 553] TAG 2121c5c6
            sec[ 554]  b 0x1318 BCOL               = 00000000  [2D/X-DROPPABLE]
                        background colour 0x00000000
            sec[ 555]  b 0x1314 FCOL               = 00000000  [2D/X-DROPPABLE]
                        foreground colour 0x00000000
ring[433] CALL  sec[ 556.. 559]  3 dw  START|2 header-ful opcode packets
          sec[ 556] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 557] TAG 212121c9
            sec[ 558]  a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
ring[438] CALL  sec[ 559.. 565]  6 dw  START|2 header-ful opcode packets
          sec[ 559] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 560] TAG acafafc0
            sec[ 561]  s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
            sec[ 562]  s 0x12bc DWGSYNC            = 00000141  [SYNC/FENCE]
                        tag 0x140; bit0=1 ARM block-until-retire
            sec[ 563]  s 0x12bc DWGSYNC            = 00000140  [SYNC/FENCE]
                        tag 0x140; bit0=0 value that lands in 0x41c (read &~7)
            sec[ 564]  a 0x12b0 REG_12B0           = 3fff8842  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §3.5 / 3d-submission-audit A.5: the values 0x3fff8842 and 0x3fff8000 are traced to MTXPARD and mtx398, but the meaning of no bit is documented
ring[443] CALL  sec[ 565.. 568]  3 dw  START|2 header-ful opcode packets
          sec[ 565] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 566] TAG 212121c0
            sec[ 567]  b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
ring[448] CALL  sec[ 568.. 573]  5 dw  START|2 header-ful opcode packets
          sec[ 568] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 569] TAG 21e3c1c2
            sec[ 570]  b 0x1308 DST_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
            sec[ 571]  b 0x1304 SRC_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
            sec[ 572]  b 0x138c LEN                = 04b00780  [2D/X-DROPPABLE]
                        height 1200, width 1920
ring[453] CALL  sec[ 573.. 576]  3 dw  START|2 header-ful opcode packets
          sec[ 573] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 574] TAG 212121bd
            sec[ 575]  a 0x12f4 GUARDBAND          = 42000000  [ESSENTIAL-3D]
                        32 (float)
ring[458] CALL  sec[ 576.. 580]  4 dw  START|2 header-ful opcode packets
          sec[ 576] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 577] TAG 2121dd87
            sec[ 578]  a 0x121c TEXENGINE_LATCH    = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3 (0x121c): the kernel writes 0 before the unit block, [INF] a latch; MTXPARD has no writer
            sec[ 579]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
ring[463] CALL  sec[ 580.. 586]  6 dw  START|2 header-ful opcode packets
          sec[ 580] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 581] TAG 83828180
            sec[ 582]  a 0x1200 TEXUNIT0_CTL       = 0001000f  [ESSENTIAL-3D]
                        pipe select (one-hot) 0xf; bits[11:8]=0x0; state[18:16]=1 disabled
            sec[ 583]  a 0x1204 TEXUNIT1_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
            sec[ 584]  a 0x1208 TEXUNIT2_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
            sec[ 585]  a 0x120c TEXUNIT3_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
          ring[468] TAG c0afafc0  (inline ring tag)
                       s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
                       s 0x12bc DWGSYNC            = 00000149  [SYNC/FENCE]
                        tag 0x148; bit0=1 ARM block-until-retire
                       s 0x12bc DWGSYNC            = 00000148  [SYNC/FENCE]
                        tag 0x148; bit0=0 value that lands in 0x41c (read &~7)
                       b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
ring[473] CALL  sec[ 586.. 598] 12 dw  START|2 header-ful opcode packets
          sec[ 586] PACKET 0000000a op=0x00 reglist  total=12 dw
          sec[ 587] TAG 21c58b8a
            sec[ 588]  c 0x1228 SURF1_BASE         = 01000000  [BOOT-POINTER]
                        base 0x01000000 | class 0
            sec[ 589]  a 0x122c SURF1_CFG          = 30030780  [ESSENTIAL-3D]
                        pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0
            sec[ 590]  b 0x1314 FCOL               = 00000000  [2D/X-DROPPABLE]
                        foreground colour 0x00000000
          sec[ 591] TAG e3c9c2c0
            sec[ 592]  b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
            sec[ 593]  b 0x1308 DST_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
            sec[ 594]  a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
            sec[ 595]  b 0x138c LEN                = 04b00780  [2D/X-DROPPABLE]
                        height 1200, width 1920
          sec[ 596] TAG 212121dd
            sec[ 597]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          ring[478] TAG c0afafc0  (inline ring tag)
                       s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
                       s 0x12bc DWGSYNC            = 00000151  [SYNC/FENCE]
                        tag 0x150; bit0=1 ARM block-until-retire
                       s 0x12bc DWGSYNC            = 00000150  [SYNC/FENCE]
                        tag 0x150; bit0=0 value that lands in 0x41c (read &~7)
                       b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
ring[483] CALL  sec[ 598.. 601]  3 dw  START|2 header-ful opcode packets
          sec[ 598] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 599] TAG 212121af
            sec[ 600]  s 0x12bc DWGSYNC            = 0000015d  [SYNC/FENCE]
                        tag 0x15c; bit0=1 ARM block-until-retire
ring[488] CALL  sec[ 601.. 613] 12 dw  START|2 header-ful opcode packets
          sec[ 601] PACKET 0000000a op=0x00 reglist  total=12 dw
          sec[ 602] TAG 21c58b8a
            sec[ 603]  c 0x1228 SURF1_BASE         = 09000000  [BOOT-POINTER]
                        base 0x09000000 | class 0
            sec[ 604]  a 0x122c SURF1_CFG          = 30030020  [ESSENTIAL-3D]
                        pitch 32 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0
            sec[ 605]  b 0x1314 FCOL               = ff000000  [2D/X-DROPPABLE]
                        foreground colour 0xff000000
          sec[ 606] TAG e3c9c2c0
            sec[ 607]  b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
            sec[ 608]  b 0x1308 DST_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
            sec[ 609]  a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
            sec[ 610]  b 0x138c LEN                = 00200020  [2D/X-DROPPABLE]
                        height 32, width 32
          sec[ 611] TAG 212121dd
            sec[ 612]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          ring[493] TAG c0afafc0  (inline ring tag)
                       s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
                       s 0x12bc DWGSYNC            = 00000161  [SYNC/FENCE]
                        tag 0x160; bit0=1 ARM block-until-retire
                       s 0x12bc DWGSYNC            = 00000160  [SYNC/FENCE]
                        tag 0x160; bit0=0 value that lands in 0x41c (read &~7)
                       b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
ring[498] CALL  sec[ 613.. 616]  3 dw  START|2 header-ful opcode packets
          sec[ 613] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 614] TAG 212121af
            sec[ 615]  s 0x12bc DWGSYNC            = 0000016d  [SYNC/FENCE]
                        tag 0x16c; bit0=1 ARM block-until-retire
ring[503] CALL  sec[ 616.. 628] 12 dw  START|2 header-ful opcode packets
          sec[ 616] PACKET 0000000a op=0x00 reglist  total=12 dw
          sec[ 617] TAG 21c58b8a
            sec[ 618]  c 0x1228 SURF1_BASE         = 018ea000  [BOOT-POINTER]
                        base 0x018ea000 | class 0
            sec[ 619]  a 0x122c SURF1_CFG          = 30030020  [ESSENTIAL-3D]
                        pitch 32 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0
            sec[ 620]  b 0x1314 FCOL               = ff000000  [2D/X-DROPPABLE]
                        foreground colour 0xff000000
          sec[ 621] TAG e3c9c2c0
            sec[ 622]  b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
            sec[ 623]  b 0x1308 DST_XY             = 00000000  [2D/X-DROPPABLE]
                        (y=0, x=0)
            sec[ 624]  a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
            sec[ 625]  b 0x138c LEN                = 00200020  [2D/X-DROPPABLE]
                        height 32, width 32
          sec[ 626] TAG 212121dd
            sec[ 627]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          ring[508] TAG c0afafc0  (inline ring tag)
                       s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
                       s 0x12bc DWGSYNC            = 00000171  [SYNC/FENCE]
                        tag 0x170; bit0=1 ARM block-until-retire
                       s 0x12bc DWGSYNC            = 00000170  [SYNC/FENCE]
                        tag 0x170; bit0=0 value that lands in 0x41c (read &~7)
                       b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
          ring[513] TAG 21212425  (inline ring tag)
                       s 0x1094 FENCE_VALUE        = 00000004  [SYNC/FENCE]
                        submission serial / write-back value
                       c 0x1090 FENCE_ADDR         = 1c9b6020  [BOOT-POINTER]
                        write-back bus address 0x1c9b6020 | class 0
ring[516] CALL  sec[ 628.. 631]  3 dw  START|2 header-ful opcode packets
          sec[ 628] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 629] TAG 212121af
            sec[ 630]  s 0x12bc DWGSYNC            = 0000018d  [SYNC/FENCE]
                        tag 0x18c; bit0=1 ARM block-until-retire
ring[521] CALL  sec[ 631.. 634]  3 dw  START|2 header-ful opcode packets
          sec[ 631] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 632] TAG 212121af
            sec[ 633]  s 0x12bc DWGSYNC            = 00000195  [SYNC/FENCE]
                        tag 0x194; bit0=1 ARM block-until-retire
ring[526] CALL  sec[ 634.. 638]  4 dw  START|2 header-ful opcode packets
          sec[ 634] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 635] TAG 2121c0c0
            sec[ 636]  s 0x1300 DWGCTL             = 00000307  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope fence RT surface A+fence RT surface B
            sec[ 637]  b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
ring[531] CALL  sec[ 638.. 646]  8 dw  START|2 header-ful opcode packets
          sec[ 638] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[ 639] TAG 21afc0c0
            sec[ 640]  b 0x1300 DWGCTL             = 00000004  [2D/X-DROPPABLE]
                        2D form: opcode 4=3D-only opcode, not in the 2D table; BOP 0x0; src slot 0; dst slot 0
            sec[ 641]  s 0x1300 DWGCTL             = 00000407  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain
            sec[ 642]  s 0x12bc DWGSYNC            = 000001a1  [SYNC/FENCE]
                        tag 0x1a0; bit0=1 ARM block-until-retire
          sec[ 643] TAG 2121c0af
            sec[ 644]  s 0x12bc DWGSYNC            = 000001a0  [SYNC/FENCE]
                        tag 0x1a0; bit0=0 value that lands in 0x41c (read &~7)
            sec[ 645]  b 0x1300 DWGCTL             = 000005c0  [2D/X-DROPPABLE]
                        [documented form: 2D solid fill, src slot 1 / dst slot 1 (CDPSSurface::Fill)]; 2D form: opcode 0=solid/rect fill; BOP 0xc (COPY); src slot 1; dst slot 1
ring[536] CALL  sec[ 646.. 652]  6 dw  START|2 header-ful opcode packets
          sec[ 646] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 647] TAG bebfcbca
            sec[ 648]  a 0x1328 SCISSOR_MIN        = 00000000  [ESSENTIAL-3D]
                        (y=0, x=0)
            sec[ 649]  a 0x132c SCISSOR_MAX        = 00f00140  [ESSENTIAL-3D]
                        (y=240, x=320)
            sec[ 650]  a 0x12fc RASTERMODE         = 0bb80024  [ESSENTIAL-3D]
                        cull select bits[1:0]=0; bit2=1 bit3=0 bit4=0; bits[31:5]=0x5dc001 UNKNOWN
            sec[ 651]  a 0x12f8 AA_THRESHOLD       = 007fe000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §3.5 (0x12f8 = the vendor parameter reg3DThresholdAA); no bit field documented
ring[541] CALL  sec[ 652.. 658]  6 dw  START|2 header-ful opcode packets
          sec[ 652] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 653] TAG bbbcb9ba
            sec[ 654]  a 0x12e8 BLENDCTL           = 00800202  [ESSENTIAL-3D]
                        blend off; srcRGB ONE; dstRGB ZERO; srcA ONE; dstA ZERO; eq ADD; eq2 ADD; dither bits[24:23]=1
            sec[ 655]  a 0x12e4 ALPHATEST          = 0000000e  [ESSENTIAL-3D]
                        alpha test off; func ALWAYS; ref 0; mask[31:24]=0x00
            sec[ 656]  a 0x12f0 BLENDCONST23       = 00000000  [ESSENTIAL-3D]
                        two 10-bit components: 0x000, 0x000
            sec[ 657]  a 0x12ec BLENDCONST01       = 00000000  [ESSENTIAL-3D]
                        two 10-bit components: 0x000, 0x000
ring[546] CALL  sec[ 658.. 666]  8 dw  START|2 header-ful opcode packets
          sec[ 658] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[ 659] TAG 21b4b3b2
            sec[ 660]  a 0x12c8 DEPTH_STENCIL      = 001000e4  [ESSENTIAL-3D]
                        depth test off; depth func LESS; stencil off; stencil func ALWAYS; sfail KEEP; szfail KEEP; spass KEEP; stencil bits 8
            sec[ 661]  a 0x12cc STENCIL_MASKS      = 000000ff  [ESSENTIAL-3D]
                        compare mask 0xff; reference 0x00
            sec[ 662]  a 0x12d0 STENCIL_WRITE      = 000000ff  [ESSENTIAL-3D]
                        write mask 0xff; depth write off
          sec[ 663] TAG 2121b1b0
            sec[ 664]  a 0x12c0 ZCLIP_MIN          = 80000000  [ESSENTIAL-3D]
                        0x80000000 [INF min depth]
            sec[ 665]  a 0x12c4 ZCLIP_MAX          = 7fffffff  [ESSENTIAL-3D]
                        0x7fffffff [INF max depth]
ring[551] CALL  sec[ 666.. 671]  5 dw  START|2 header-ful opcode packets
          sec[ 666] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 667] TAG 21b7b6b5
            sec[ 668]  a 0x12d4 FOG_CTL            = 00000000  [ESSENTIAL-3D]
                        fog off; mode bits[2:1]=0; bit4=0; B=0 G=0 R=0
            sec[ 669]  a 0x12d8 FOG_START          = 00000000  [ESSENTIAL-3D]
                        0.000000 (v/16777215)
            sec[ 670]  a 0x12dc FOG_END            = 00ffffff  [ESSENTIAL-3D]
                        1.000000 (v/16777215) = fog off
ring[556] CALL  sec[ 671.. 676]  5 dw  START|2 header-ful opcode packets
          sec[ 671] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 672] TAG 212c1000
            sec[ 673]  a 0x1000 VTXBASE0           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 674]  a 0x1040 VTXSTRIDE0_1       = 00000004  [ESSENTIAL-3D]
                        stride pair: 4, 0 bytes
            sec[ 675]  a 0x10b0 PRIMWORD           = 0002305c  [ESSENTIAL-3D]
                        A=0x5c triangles; B=3 triangle list; bit17 set (always); bit18 clear
ring[561] CALL  sec[ 676.. 680]  4 dw  START|2 header-ful opcode packets
          sec[ 676] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 677] TAG 21212e2d
            sec[ 678]  a 0x10b4 BLOCKPTR           = 00000000  [ESSENTIAL-3D]
                        block-file write pointer / attribute-port reset = 0
            sec[ 679]  a 0x10b8 ATTRDESC           = 83c00000  [ESSENTIAL-3D]
                        stream 0; dwordOffset 0; A[16:12]=0; B(type)[19:17]=0; C(mask)[25:22]=0xf; bit26=0; D[29:27]=0; last=1; bits[21:20]=0
ring[566] CALL  sec[ 680.. 686]  6 dw  START|2 header-ful opcode packets
          sec[ 680] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 681] TAG 403c3cdd
            sec[ 682]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
            sec[ 683]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
            sec[ 684]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
            sec[ 685]  c 0x1100 WARP_ENTRY         = 076ac703  [BOOT-POINTER]
                        bus 0x16405500 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
ring[571] CALL  sec[ 686.. 695]  9 dw  START|2 header-ful opcode packets
          sec[ 686] PACKET 41020007 op=0x41 PREFORMATTED BLOB  count=1 total=9
                     descriptor 03800015 00000000 3f800000 076ab703
                     NOT [n,first,1,0]: this is the mtx1814 caller-preformatted form (a program image)
                       + 0 3f803f80 076aab03 ffffffff 076ac103   |    1.00194, 1.7654e-34,    nan/inf, 1.7661e-34
ring[576] CALL  sec[ 695.. 701]  6 dw  START|2 header-ful opcode packets
          sec[ 695] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 696] TAG 403c3cdd
            sec[ 697]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
            sec[ 698]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
            sec[ 699]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
            sec[ 700]  c 0x1100 WARP_ENTRY         = 076ac903  [BOOT-POINTER]
                        bus 0x16405b00 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
ring[581] CALL  sec[ 701.. 738] 37 dw  START|2 header-ful opcode packets
          sec[ 701] PACKET 41100023 op=0x41 BLOCK LOAD  count=8 total=37
                     descriptor 00000008 00000000 00000001 00000000
                     -> 8 vec4 into block file slot 0x000..0x007
                       c[0x000] = 3f800000 00000000 00000000 00000000   |          1,          0,          0,          0
                       c[0x001] = 00000000 3f800000 00000000 00000000   |          0,          1,          0,          0
                       c[0x002] = 00000000 00000000 3f800000 00000000   |          0,          0,          1,          0
                       c[0x003] = 00000000 00000000 00000000 3f800000   |          0,          0,          0,          1
                       c[0x004] = 3f800000 00000000 00000000 00000000   |          1,          0,          0,          0
                       c[0x005] = 00000000 3f800000 00000000 00000000   |          0,          1,          0,          0
                       c[0x006] = 00000000 00000000 3f800000 00000000   |          0,          0,          1,          0
                       c[0x007] = 00000000 00000000 00000000 00000000   |          0,          0,          0,          0
ring[586] CALL  sec[ 738.. 763] 25 dw  START|2 header-ful opcode packets
          sec[ 738] PACKET 410a0017 op=0x41 BLOCK LOAD  count=5 total=25
                     descriptor 00000005 00000134 00000001 00000000
                     -> 5 vec4 into block file slot 0x134..0x138
                       c[0x134] = 3f4ccccd 3f4ccccd 3f4ccccd 3f800000   |        0.8,        0.8,        0.8,          1
                       c[0x135] = 3e4ccccd 3e4ccccd 3e4ccccd 3f800000   |        0.2,        0.2,        0.2,          1
                       c[0x136] = 00000000 00000000 00000000 00000000   |          0,          0,          0,          0
                       c[0x137] = 00000000 00000000 00000000 3f800000   |          0,          0,          0,          1
                       c[0x138] = 3d23d70b 3d23d70b 3d23d70b 3f800000   |       0.04,       0.04,       0.04,          1
ring[591] CALL  sec[ 763.. 788] 25 dw  START|2 header-ful opcode packets
          sec[ 763] PACKET 410a0017 op=0x41 BLOCK LOAD  count=5 total=25
                     descriptor 00000005 00000149 00000001 00000000
                     -> 5 vec4 into block file slot 0x149..0x14d
                       c[0x149] = 3f800000 00000000 00000000 00000000   |          1,          0,          0,          0
                       c[0x14a] = 00000000 3f800000 00000000 00000000   |          0,          1,          0,          0
                       c[0x14b] = 00000000 00000000 3f800000 00000000   |          0,          0,          1,          0
                       c[0x14c] = 00000000 00000000 00000000 3f800000   |          0,          0,          0,          1
                       c[0x14d] = 00000000 44960000 00000000 42000000   |          0,       1200,          0,         32
ring[596] CALL  sec[ 788.. 813] 25 dw  START|2 header-ful opcode packets
          sec[ 788] PACKET 410a0017 op=0x41 BLOCK LOAD  count=5 total=25
                     descriptor 00000005 00000155 00000001 00000000
                     -> 5 vec4 into block file slot 0x155..0x159
                       c[0x155] = 00000000 00000000 00000000 3f800000   |          0,          0,          0,          1
                       c[0x156] = 3f4ccccd 3f4ccccd 3f4ccccd 3f800000   |        0.8,        0.8,        0.8,          1
                       c[0x157] = 3e4ccccd 3e4ccccd 3e4ccccd 3f800000   |        0.2,        0.2,        0.2,          1
                       c[0x158] = 00000000 00000000 00000000 00000000   |          0,          0,          0,          0
                       c[0x159] = 00000000 00000000 00000000 3f800000   |          0,          0,          0,          1
ring[601] CALL  sec[ 813.. 826] 13 dw  START|2 header-ful opcode packets
          sec[ 813] PACKET 4104000b op=0x41 BLOCK LOAD  count=2 total=13
                     descriptor 00000002 00000160 00000001 00000000
                     -> 2 vec4 into block file slot 0x160..0x161
                       c[0x160] = 43200000 c2f00000 3f000000 3f800000   |        160,       -120,        0.5,          1
                       c[0x161] = 43200000 42f00000 3f000000 43700000   |        160,        120,        0.5,        240
ring[606] CALL  sec[ 826.. 847] 21 dw  START|2 header-ful opcode packets
          sec[ 826] PACKET 41080013 op=0x41 BLOCK LOAD  count=4 total=21
                     descriptor 00000004 00000172 00000001 00000000
                     -> 4 vec4 into block file slot 0x172..0x175
                       c[0x172] = 3f800000 00000000 00000000 3f000000   |          1,          0,          0,        0.5
                       c[0x173] = 00000000 45000000 3f000000 bf000000   |          0,       2048,        0.5,       -0.5
                       c[0x174] = 45ff0000 45ff0000 c5ff0000 c5ff0000   |       8160,       8160,      -8160,      -8160
                       c[0x175] = 43000000 00000000 00000000 00000000   |        128,          0,          0,          0
ring[611] CALL  sec[ 847.. 856]  9 dw  START|2 header-ful opcode packets
          sec[ 847] PACKET 41020007 op=0x41 BLOCK LOAD  count=1 total=9
                     descriptor 00000001 00000178 00000001 00000000
                     -> 1 vec4 into block file slot 0x178..0x178
                       c[0x178] = 00000000 00000000 3f000000 49400000   |          0,          0,        0.5,     786432
ring[616] CALL  sec[ 856.. 869] 13 dw  START|2 header-ful opcode packets
          sec[ 856] PACKET 4104000b op=0x41 BLOCK LOAD  count=2 total=13
                     descriptor 00000002 0000017d 00000001 00000000
                     -> 2 vec4 into block file slot 0x17d..0x17e
                       c[0x17d] = 4b000000 3d4ccccd 41f80000 3d042108   | 8.38861e+06,       0.05,         31,  0.0322581
                       c[0x17e] = 00000000 3d800000 00000000 00000000   |          0,     0.0625,          0,          0
ring[621] CALL  sec[ 869.. 877]  8 dw  START|2 header-ful opcode packets
          sec[ 869] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[ 870] TAG 3c3cdd4c
            sec[ 871]  a 0x1130 WARP_PIPECTL       = 0700200c  [ESSENTIAL-3D]
                        bit9 CLEAR (3d-submission-audit says mtx3400 ORs 0x200 in); every other bit UNKNOWN
            sec[ 872]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
            sec[ 873]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
            sec[ 874]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
          sec[ 875] TAG 21212140
            sec[ 876]  c 0x1100 WARP_ENTRY         = 076acb03  [BOOT-POINTER]
                        bus 0x16406100 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
ring[626] CALL  sec[ 877.. 894] 17 dw  START|2 header-ful opcode packets
          sec[ 877] PACKET 4106000f op=0x41 PREFORMATTED BLOB  count=3 total=17
                     descriptor 000000e0 00000160 00000080 00000800
                     NOT [n,first,1,0]: this is the mtx1814 caller-preformatted form (a program image)
                       + 0 00000000 00000001 00000002 00000003   |          0, 1.4013e-45, 2.8026e-45, 4.2039e-45
                       + 1 02002004 0200200c 00000000 00000000   | 9.4131e-38, 9.4132e-38,          0,          0
                       + 2 076aab03 ffffffff ffffffff ffffffff   | 1.7654e-34,    nan/inf,    nan/inf,    nan/inf
ring[631] CALL  sec[ 894.. 900]  6 dw  START|2 header-ful opcode packets
          sec[ 894] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 895] TAG 4c3c3cdd
            sec[ 896]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
            sec[ 897]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
            sec[ 898]  a 0x10f0 WARP_MODE          = 20000028  [ESSENTIAL-3D]
                        mode[3:0]=0x8 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE); rest 0x00000020 UNKNOWN
            sec[ 899]  a 0x1130 WARP_PIPECTL       = 0200200c  [ESSENTIAL-3D]
                        bit9 CLEAR (3d-submission-audit says mtx3400 ORs 0x200 in); every other bit UNKNOWN
ring[636] CALL  sec[ 900.. 906]  6 dw  START|2 header-ful opcode packets
          sec[ 900] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 901] TAG 4b4a4948
            sec[ 902]  a 0x1120 VTXDEFAULT_x       = 45ffc5ff  [ESSENTIAL-3D]
                        8184.75 (float)
            sec[ 903]  a 0x1124 VTXDEFAULT_y       = c5ff45ff  [ESSENTIAL-3D]
                        -8168.75 (float)
            sec[ 904]  a 0x1128 VTXDEFAULT_z       = 00000000  [ESSENTIAL-3D]
                        0.0 (float)
            sec[ 905]  a 0x112c VTXDEFAULT_w       = 3f800000  [ESSENTIAL-3D]
                        1 (float)
ring[641] CALL  sec[ 906.. 911]  5 dw  START|2 header-ful opcode packets
          sec[ 906] PACKET 00000003 op=0x00 reglist  total=5 dw
          sec[ 907] TAG 21464544
            sec[ 908]  a 0x1110 VTXFMT0            = 00000003  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x44 (named "vertex-attribute format", S[0x61c]|S[0x608]); bit layout undocumented
            sec[ 909]  a 0x1114 VTXFMT1            = 00287210  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x45 (S[0x60c]); bit layout undocumented
            sec[ 910]  a 0x1118 VTXFMT2            = 00006543  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x46 (S[0x610]); bit layout undocumented
ring[646] CALL  sec[ 911.. 919]  8 dw  START|2 header-ful opcode packets
          sec[ 911] PACKET 00000006 op=0x00 reglist  total=8 dw
          sec[ 912] TAG 212121dd
            sec[ 913]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          sec[ 914] TAG 3e403c3c
            sec[ 915]  a 0x10f0 WARP_MODE          = 20000002  [ESSENTIAL-3D]
                        mode[3:0]=0x2 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 916]  a 0x10f0 WARP_MODE          = 20000002  [ESSENTIAL-3D]
                        mode[3:0]=0x2 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 917]  c 0x1100 WARP_ENTRY         = 076acd03  [BOOT-POINTER]
                        bus 0x16406700 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
            sec[ 918]  a 0x10f8 WARP_ENTRY_OBJ2    = 00000002  [ESSENTIAL-3D]
                        literal 2 - too small to be an entry (warp-binding documents 0x10f8 = 2 as the "unbound" marker)
ring[651] CALL  sec[ 919.. 932] 13 dw  START|2 header-ful opcode packets
          sec[ 919] PACKET 0000000b op=0x00 reglist  total=13 dw
          sec[ 920] TAG 212121dd
            sec[ 921]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          sec[ 922] TAG 33323c3c
            sec[ 923]  a 0x10f0 WARP_MODE          = 20000002  [ESSENTIAL-3D]
                        mode[3:0]=0x2 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 924]  a 0x10f0 WARP_MODE          = 20000002  [ESSENTIAL-3D]
                        mode[3:0]=0x2 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 925]  c 0x10c8 WARP_STAGE2_ENTRY  = 076acf3f  [BOOT-POINTER]
                        bus 0x16406db4 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
            sec[ 926]  c 0x10cc WARP_STAGE3_ENTRY  = 076acf4f  [BOOT-POINTER]
                        bus 0x16406de4 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
          sec[ 927] TAG 30363534
            sec[ 928]  c 0x10d0 WARP_STAGE4_ENTRY  = 076acf3f  [BOOT-POINTER]
                        bus 0x16406db4 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
            sec[ 929]  c 0x10d4 WARP_STAGE5_ENTRY  = 076acf4f  [BOOT-POINTER]
                        bus 0x16406de4 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
            sec[ 930]  c 0x10d8 WARP_STAGE6_ENTRY  = 076ac103  [BOOT-POINTER]
                        bus 0x16404300 (=(v&~3)*3), class tag 3; instruction index = (bus-slotbase)/12; no matching entry in warp-programs/entries.txt (different boot)
            sec[ 931]  a 0x10c0 WARP_STAGE0_ENTRY  = 00000000  [ESSENTIAL-3D]
                        null
ring[656] CALL  sec[ 932.. 943] 11 dw  START|2 header-ful opcode packets
          sec[ 932] PACKET 00000009 op=0x00 reglist  total=11 dw
          sec[ 933] TAG 212121dd
            sec[ 934]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
          sec[ 935] TAG 4c3c3c3c
            sec[ 936]  a 0x10f0 WARP_MODE          = 20000002  [ESSENTIAL-3D]
                        mode[3:0]=0x2 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 937]  a 0x10f0 WARP_MODE          = 20000002  [ESSENTIAL-3D]
                        mode[3:0]=0x2 UNKNOWN; bit29 set (0x20000000 = the bind-path MODE)
            sec[ 938]  a 0x10f0 WARP_MODE          = 00000002  [ESSENTIAL-3D]
                        mode[3:0]=0x2 UNKNOWN
            sec[ 939]  a 0x1130 WARP_PIPECTL       = 0200200c  [ESSENTIAL-3D]
                        bit9 CLEAR (3d-submission-audit says mtx3400 ORs 0x200 in); every other bit UNKNOWN
          sec[ 940] TAG 21215147
            sec[ 941]  a 0x111c REG_111C           = 00000fdb  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x47 (S[0x5f4]); role column blank
            sec[ 942]  a 0x1144 REG_1144           = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - warp-binding idx 0x51 (S[0x618]); role column blank
ring[661] CALL  sec[ 943.. 946]  3 dw  START|2 header-ful opcode packets
          sec[ 943] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 944] TAG 2121212c
            sec[ 945]  a 0x10b0 PRIMWORD           = 0006305c  [ESSENTIAL-3D]
                        A=0x5c triangles; B=3 triangle list; bit17 set (always); bit18 set = legacy per-vertex-register path (ctx+0x430==1)
ring[666] CALL  sec[ 946.. 962] 16 dw  START|2 header-ful opcode packets
          sec[ 946] PACKET 0000000e op=0x00 reglist  total=16 dw
          sec[ 947] TAG 03020100
            sec[ 948]  a 0x1000 VTXBASE0           = 00000002  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 2 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 949]  a 0x1004 VTXBASE1           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 950]  a 0x1008 VTXBASE2           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 951]  a 0x100c VTXBASE3           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
          sec[ 952] TAG 07060504
            sec[ 953]  a 0x1010 VTXBASE4           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 954]  a 0x1014 VTXBASE5           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 955]  a 0x1018 VTXBASE6           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[ 956]  a 0x101c VTXBASE7           = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 0 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
          sec[ 957] TAG 13121110
            sec[ 958]  a 0x1040 VTXSTRIDE0_1       = 00000004  [ESSENTIAL-3D]
                        stride pair: 4, 0 bytes
            sec[ 959]  a 0x1044 VTXSTRIDE2_3       = 00000000  [ESSENTIAL-3D]
                        stride pair: 0, 0 bytes
            sec[ 960]  a 0x1048 VTXSTRIDE4_5       = 00000000  [ESSENTIAL-3D]
                        stride pair: 0, 0 bytes
            sec[ 961]  a 0x104c VTXSTRIDE6_7       = 00000000  [ESSENTIAL-3D]
                        stride pair: 0, 0 bytes
ring[671] CALL  sec[ 962.. 966]  4 dw  START|2 header-ful opcode packets
          sec[ 962] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 963] TAG 21211819
            sec[ 964]  a 0x1064 VTXFIRST           = 00000000  [ESSENTIAL-3D]
                        first usable index 0
            sec[ 965]  a 0x1060 VTXCOUNT           = 000280cc  [ESSENTIAL-3D]
                        164044 vertices addressable  [draw-builders-kernel prints 0x280cc as 163020; it is 164044, and 164044*20 = 3280880 bytes]
ring[676] CALL  sec[ 966.. 969]  3 dw  START|2 header-ful opcode packets
          sec[ 966] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 967] TAG 2121212d
            sec[ 968]  a 0x10b4 BLOCKPTR           = 00000000  [ESSENTIAL-3D]
                        block-file write pointer / attribute-port reset = 0
ring[681] CALL  sec[ 969.. 975]  6 dw  START|2 header-ful opcode packets
          sec[ 969] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 970] TAG 2e2e2e2e
            sec[ 971]  a 0x10b8 ATTRDESC           = 20c00000  [ESSENTIAL-3D]
                        stream 0; dwordOffset 0; A[16:12]=0; B(type)[19:17]=0; C(mask)[25:22]=0x3; bit26=0; D[29:27]=4; last=0; bits[21:20]=0
            sec[ 972]  a 0x10b8 ATTRDESC           = a1c50020  [ESSENTIAL-3D]
                        stream 0; dwordOffset 2; A[16:12]=16; B(type)[19:17]=2; C(mask)[25:22]=0x7; bit26=0; D[29:27]=4; last=1; bits[21:20]=0  [NOTE pixel-pipeline §5.2 prose calls this descriptor A=5; the draw-builders-kernel field formula applied to the wire value gives 16 - one of the two is wrong]
            sec[ 973]  a 0x10b8 ATTRDESC           = 00000000  [ESSENTIAL-3D]
                        no attribute / terminator
            sec[ 974]  a 0x10b8 ATTRDESC           = 00000000  [ESSENTIAL-3D]
                        no attribute / terminator
ring[686] CALL  sec[ 975.. 981]  6 dw  START|2 header-ful opcode packets
          sec[ 975] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 976] TAG 8d8c8988
            sec[ 977]  c 0x1220 SURF0_BASE         = 02000000  [BOOT-POINTER]
                        base 0x02000000 | class 0
            sec[ 978]  a 0x1224 SURF0_CFG          = 32020780  [ESSENTIAL-3D]
                        pitch 1920 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
            sec[ 979]  a 0x1230 SURF2_BASE         = 00000000  [ESSENTIAL-3D]
                        base 0x00000000 | class 0
            sec[ 980]  a 0x1234 SURF2_CFG          = 32020000  [ESSENTIAL-3D]
                        pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0
ring[691] CALL  sec[ 981.. 985]  4 dw  START|2 header-ful opcode packets
          sec[ 981] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[ 982] TAG 21218b8a
            sec[ 983]  c 0x1228 SURF1_BASE         = 01000000  [BOOT-POINTER]
                        base 0x01000000 | class 0
            sec[ 984]  a 0x122c SURF1_CFG          = 36020780  [ESSENTIAL-3D]
                        pitch 1920 px; type 2; swap 0; format 0x36=[INF] S8Z24 (32bpp class); tiling 0
ring[696] CALL  sec[ 985.. 988]  3 dw  START|2 header-ful opcode packets
          sec[ 985] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 986] TAG 212121c9
            sec[ 987]  a 0x1324 PLNWT              = ffffffff  [ESSENTIAL-3D]
                        plane write mask 0xffffffff = all planes
ring[701] CALL  sec[ 988.. 994]  6 dw  START|2 header-ful opcode packets
          sec[ 988] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[ 989] TAG acafafc0
            sec[ 990]  s 0x1300 DWGCTL             = 00000707  [SYNC/FENCE]
                        DWGSYNC soft-trap (low nibble 7, never cached by mtx2434): scope pipe drain+fence RT surface A+fence RT surface B
            sec[ 991]  s 0x12bc DWGSYNC            = 000001a9  [SYNC/FENCE]
                        tag 0x1a8; bit0=1 ARM block-until-retire
            sec[ 992]  s 0x12bc DWGSYNC            = 000001a8  [SYNC/FENCE]
                        tag 0x1a8; bit0=0 value that lands in 0x41c (read &~7)
            sec[ 993]  a 0x12b0 REG_12B0           = 3fff8842  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §3.5 / 3d-submission-audit A.5: the values 0x3fff8842 and 0x3fff8000 are traced to MTXPARD and mtx398, but the meaning of no bit is documented
ring[706] CALL  sec[ 994.. 997]  3 dw  START|2 header-ful opcode packets
          sec[ 994] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 995] TAG 212121c0
            sec[ 996]  a 0x1300 DWGCTL             = 00a60ec4  [ESSENTIAL-3D]
                        [documented form: the vendor kernel 3D draw DWGCTL (pixel-pipeline §3.8)]; 3D form: constant 0x00860e00 (bits 9,10,11,17,18 + clip bit23); low byte 0xc4 = opcode 4 | BOP 0xc (COPY); bit21 = cfg&0x40; NOTE pixel-pipeline §3.8 also ORs ((surf>>25)&0xf) into the low nibble; with the captured colour target 0x32020780 that is 9, and 9 is not present here - the formula does not reproduce this value
ring[711] CALL  sec[ 997..1000]  3 dw  START|2 header-ful opcode packets
          sec[ 997] PACKET 00000001 op=0x00 reglist  total=3 dw
          sec[ 998] TAG 212121bd
            sec[ 999]  a 0x12f4 GUARDBAND          = 42000000  [ESSENTIAL-3D]
                        32 (float)
ring[716] CALL  sec[1000..1004]  4 dw  START|2 header-ful opcode packets
          sec[1000] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[1001] TAG 2121dd87
            sec[1002]  a 0x121c TEXENGINE_LATCH    = 00000000  [ESSENTIAL-3D]
                        UNKNOWN - pixel-pipeline §2.3 (0x121c): the kernel writes 0 before the unit block, [INF] a latch; MTXPARD has no writer
            sec[1003]  s 0x1374 STROBE             = 00000000  [SYNC/FENCE]
                        strobe, payload ignored by hardware
ring[721] CALL  sec[1004..1010]  6 dw  START|2 header-ful opcode packets
          sec[1004] PACKET 00000004 op=0x00 reglist  total=6 dw
          sec[1005] TAG 83828180
            sec[1006]  a 0x1200 TEXUNIT0_CTL       = 0001000f  [ESSENTIAL-3D]
                        pipe select (one-hot) 0xf; bits[11:8]=0x0; state[18:16]=1 disabled
            sec[1007]  a 0x1204 TEXUNIT1_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
            sec[1008]  a 0x1208 TEXUNIT2_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
            sec[1009]  a 0x120c TEXUNIT3_CTL       = 00000000  [ESSENTIAL-3D]
                        pipe select (one-hot) 0x0; bits[11:8]=0x0; state[18:16]=0 UNKNOWN
ring[726] CALL  sec[1010..1021] 11 dw  START|2 header-ful opcode packets
          sec[1010] PACKET 00000009 op=0x00 reglist  total=11 dw
          sec[1011] TAG 9a9b9899
            sec[1012]  a 0x1264 COMB_RGB_SRC       = 00000200  [ESSENTIAL-3D]
                        slot 0; A=v0 B=t0 C=t0
            sec[1013]  a 0x1260 COMB_ALPHA_SRC     = 00000200  [ESSENTIAL-3D]
                        slot 0; A=v0 B=t0 C=t0
            sec[1014]  a 0x126c COMB_RGB_DST       = 01400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=1; scale=x1
            sec[1015]  a 0x1268 COMB_ALPHA_DST     = 01400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=1; scale=x1
          sec[1016] TAG a1a09f9e
            sec[1017]  a 0x1278 COMB_CONST_c0_xy   = 00000000  [ESSENTIAL-3D]
                        signed Q1.10 pair: 0.0000, 0.0000
            sec[1018]  a 0x127c COMB_CONST_c0_zw   = 00000000  [ESSENTIAL-3D]
                        signed Q1.10 pair: 0.0000, 0.0000
            sec[1019]  a 0x1280 COMB_CONST_c1_xy   = 00000000  [ESSENTIAL-3D]
                        signed Q1.10 pair: 0.0000, 0.0000
            sec[1020]  a 0x1284 COMB_CONST_c1_zw   = 00000000  [ESSENTIAL-3D]
                        signed Q1.10 pair: 0.0000, 0.0000
ring[731] CALL  sec[1021..1034] 13 dw  START|2 header-ful opcode packets
          sec[1021] PACKET 0000000b op=0x00 reglist  total=13 dw
          sec[1022] TAG 21219b99
            sec[1023]  a 0x1264 COMB_RGB_SRC       = 00000082  [ESSENTIAL-3D]
                        slot 2; A=r0 B=t0 C=t0
            sec[1024]  a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          sec[1025] TAG 21219b99
            sec[1026]  a 0x1264 COMB_RGB_SRC       = 00000084  [ESSENTIAL-3D]
                        slot 4; A=r0 B=t0 C=t0
            sec[1027]  a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          sec[1028] TAG 21219b99
            sec[1029]  a 0x1264 COMB_RGB_SRC       = 00000088  [ESSENTIAL-3D]
                        slot 8; A=r0 B=t0 C=t0
            sec[1030]  a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          sec[1031] TAG 21219b99
            sec[1032]  a 0x1264 COMB_RGB_SRC       = 0000008a  [ESSENTIAL-3D]
                        slot 10; A=r0 B=t0 C=t0
            sec[1033]  a 0x126c COMB_RGB_DST       = 00400400  [ESSENTIAL-3D]
                        MOV (A); dst=r0; sat=0; scale=x1
          ring[736] TAG 21212425  (inline ring tag)
                       s 0x1094 FENCE_VALUE        = 00000005  [SYNC/FENCE]
                        submission serial / write-back value
                       c 0x1090 FENCE_ADDR         = 1c9b6020  [BOOT-POINTER]
                        write-back bus address 0x1c9b6020 | class 0
ring[739] CALL  sec[1034..1038]  4 dw  START|2 header-ful opcode packets
          sec[1034] PACKET 00000002 op=0x00 reglist  total=4 dw
          sec[1035] TAG 21211000
            sec[1036]  a 0x1000 VTXBASE0           = 00000003  [ESSENTIAL-3D]
                        base 0x00000000 | memory class 3 (0 local/VRAM, 2 AGP, 3 PCI); on the copy path this is the pushbuffer class word, not an address
            sec[1037]  a 0x1040 VTXSTRIDE0_1       = 00000004  [ESSENTIAL-3D]
                        stride pair: 4, 0 bytes
ring[744] CALL  sec[1038..1041]  3 dw  START|2 header-ful opcode packets
          sec[1038] PACKET 97080001 op=0x97 DRAW
                     vertex buffer 0x16262414 .. 0x16262450 (60 bytes)
                     dwordsPerVertex=5 -> 3 vertices  [c BOOT-POINTER: both dwords are absolute bus addresses]

# Block (constant) file after the span

10 opcode-0x41 packets: 8 constant loads, 2 preformatted blobs

| # | ring | sec off | header | kind | slots | descriptor |
|---|------|---------|--------|------|-------|------------|
| 0 | 571 | 686 | `41020007` | blob | - | `03800015 00000000 3f800000 076ab703` |
| 1 | 581 | 701 | `41100023` | load | 0x000..0x007 | `00000008 00000000 00000001 00000000` |
| 2 | 586 | 738 | `410a0017` | load | 0x134..0x138 | `00000005 00000134 00000001 00000000` |
| 3 | 591 | 763 | `410a0017` | load | 0x149..0x14d | `00000005 00000149 00000001 00000000` |
| 4 | 596 | 788 | `410a0017` | load | 0x155..0x159 | `00000005 00000155 00000001 00000000` |
| 5 | 601 | 813 | `4104000b` | load | 0x160..0x161 | `00000002 00000160 00000001 00000000` |
| 6 | 606 | 826 | `41080013` | load | 0x172..0x175 | `00000004 00000172 00000001 00000000` |
| 7 | 611 | 847 | `41020007` | load | 0x178..0x178 | `00000001 00000178 00000001 00000000` |
| 8 | 616 | 856 | `4104000b` | load | 0x17d..0x17e | `00000002 0000017d 00000001 00000000` |
| 9 | 626 | 877 | `4106000f` | blob | - | `000000e0 00000160 00000080 00000800` |

c[0x000] = 3f800000 00000000 00000000 00000000   |            1,            0,            0,            0
c[0x001] = 00000000 3f800000 00000000 00000000   |            0,            1,            0,            0
c[0x002] = 00000000 00000000 3f800000 00000000   |            0,            0,            1,            0
c[0x003] = 00000000 00000000 00000000 3f800000   |            0,            0,            0,            1
c[0x004] = 3f800000 00000000 00000000 00000000   |            1,            0,            0,            0
c[0x005] = 00000000 3f800000 00000000 00000000   |            0,            1,            0,            0
c[0x006] = 00000000 00000000 3f800000 00000000   |            0,            0,            1,            0
c[0x007] = 00000000 00000000 00000000 00000000   |            0,            0,            0,            0
c[0x134] = 3f4ccccd 3f4ccccd 3f4ccccd 3f800000   |          0.8,          0.8,          0.8,            1
c[0x135] = 3e4ccccd 3e4ccccd 3e4ccccd 3f800000   |          0.2,          0.2,          0.2,            1
c[0x136] = 00000000 00000000 00000000 00000000   |            0,            0,            0,            0
c[0x137] = 00000000 00000000 00000000 3f800000   |            0,            0,            0,            1
c[0x138] = 3d23d70b 3d23d70b 3d23d70b 3f800000   |         0.04,         0.04,         0.04,            1
c[0x149] = 3f800000 00000000 00000000 00000000   |            1,            0,            0,            0
c[0x14a] = 00000000 3f800000 00000000 00000000   |            0,            1,            0,            0
c[0x14b] = 00000000 00000000 3f800000 00000000   |            0,            0,            1,            0
c[0x14c] = 00000000 00000000 00000000 3f800000   |            0,            0,            0,            1
c[0x14d] = 00000000 44960000 00000000 42000000   |            0,         1200,            0,           32
c[0x155] = 00000000 00000000 00000000 3f800000   |            0,            0,            0,            1
c[0x156] = 3f4ccccd 3f4ccccd 3f4ccccd 3f800000   |          0.8,          0.8,          0.8,            1
c[0x157] = 3e4ccccd 3e4ccccd 3e4ccccd 3f800000   |          0.2,          0.2,          0.2,            1
c[0x158] = 00000000 00000000 00000000 00000000   |            0,            0,            0,            0
c[0x159] = 00000000 00000000 00000000 3f800000   |            0,            0,            0,            1
c[0x160] = 43200000 c2f00000 3f000000 3f800000   |          160,         -120,          0.5,            1
c[0x161] = 43200000 42f00000 3f000000 43700000   |          160,          120,          0.5,          240
c[0x172] = 3f800000 00000000 00000000 3f000000   |            1,            0,            0,          0.5
c[0x173] = 00000000 45000000 3f000000 bf000000   |            0,         2048,          0.5,         -0.5
c[0x174] = 45ff0000 45ff0000 c5ff0000 c5ff0000   |         8160,         8160,        -8160,        -8160
c[0x175] = 43000000 00000000 00000000 00000000   |          128,            0,            0,            0
c[0x178] = 00000000 00000000 3f000000 49400000   |            0,            0,          0.5,       786432
c[0x17d] = 4b000000 3d4ccccd 41f80000 3d042108   |  8.38861e+06,         0.05,           31,    0.0322581
c[0x17e] = 00000000 3d800000 00000000 00000000   |            0,       0.0625,            0,            0

## Implied viewport

c[0x160] scale  = (160, -120, 0.5, 1)
c[0x161] offset = (160, 120, 0.5, 240)
  W    = 2*scale.x            = 320
  H    = -2*scale.y           = 240   (offset.w = 240, agrees)
  x0   = offset.x - scale.x   = 0
  y0   = offset.y + scale.y   = 0
  near = offset.z - scale.z   = 0
  far  = offset.z + scale.z   = 1

  => glViewport(0, 0, 320, 240), glDepthRange(0, 1)

  gltest.c NDC triangle maps to window pixels:
    (-0.70, -0.60) -> (48.0, 192.0)
    (+0.00, +0.60) -> (160.0, 48.0)
    (+0.70, -0.60) -> (272.0, 192.0)

# Ordered minimal recipe

One complete self-contained triangle submission, reduced from the 648 register writes in the span to 109 by dropping 2D/X-server traffic, fence bookkeeping and superseded writes.

**120 steps: 109 register writes, 8 block-file loads, 2 program blobs, 1 draw.**

| # | class | register / packet | value | note |
|---|---|---|---|---|
| 0 | ESSENTIAL-3D | `0x11c8` TEX_REG_11C8 | `00000000` |  |
| 1 | ESSENTIAL-3D | `0x11cc` TEX_BORDER | `ffffffff` |  |
| 2 | ESSENTIAL-3D | `0x11d0` TEX_BORDER_MASK | `00000000` |  |
| 3 | ESSENTIAL-3D | `0x11d4` TEX_REG_11D4 | `00000000` |  |
| 4 | ESSENTIAL-3D | `0x11d8` TEX_REG_11D8 | `00000000` |  |
| 5 | ESSENTIAL-3D | `0x1168` DDA_1168 | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 6 | ESSENTIAL-3D | `0x11dc` TEX_BASE | `00000000` |  |
| 7 | ESSENTIAL-3D | `0x11e0` TEX_WIDTH_FMT | `32020001` |  |
| 8 | ESSENTIAL-3D | `0x11e4` TEX_HEIGHT_DEPTH | `00010001` |  |
| 9 | ESSENTIAL-3D | `0x11c0` TEX_ADDRMODE | `00000000` |  |
| 10 | ESSENTIAL-3D | `0x11c4` TEX_FILTER | `00000000` |  |
| 11 | ESSENTIAL-3D | `0x1164` DDA_1164 | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 12 | ESSENTIAL-3D | `0x1180` DDA_1180 | `41300000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 13 | ESSENTIAL-3D | `0x118c` DDA_118C | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 14 | ESSENTIAL-3D | `0x1190` DDA_1190 | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 15 | ESSENTIAL-3D | `0x1178` DDA_1178 | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 16 | ESSENTIAL-3D | `0x1194` DDA_1194 | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 17 | ESSENTIAL-3D | `0x116c` DDA_116C | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 18 | ESSENTIAL-3D | `0x1174` DDA_1174 | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 19 | ESSENTIAL-3D | `0x117c` DDA_117C | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 20 | ESSENTIAL-3D | `0x1198` DDA_1198 | `00000000` | legacy DDA attribute plane - **droppable** on the WARP path |
| 21 | ESSENTIAL-3D | `0x1238` SURF3_BASE | `00000000` |  |
| 22 | ESSENTIAL-3D | `0x123c` SURF3_CFG | `32020000` |  |
| 23 | ESSENTIAL-3D | `0x1328` SCISSOR_MIN | `00000000` |  |
| 24 | ESSENTIAL-3D | `0x132c` SCISSOR_MAX | `00f00140` |  |
| 25 | ESSENTIAL-3D | `0x12fc` RASTERMODE | `0bb80024` |  |
| 26 | ESSENTIAL-3D | `0x12f8` AA_THRESHOLD | `007fe000` |  |
| 27 | ESSENTIAL-3D | `0x12e8` BLENDCTL | `00800202` |  |
| 28 | ESSENTIAL-3D | `0x12e4` ALPHATEST | `0000000e` |  |
| 29 | ESSENTIAL-3D | `0x12f0` BLENDCONST23 | `00000000` |  |
| 30 | ESSENTIAL-3D | `0x12ec` BLENDCONST01 | `00000000` |  |
| 31 | ESSENTIAL-3D | `0x12c8` DEPTH_STENCIL | `001000e4` |  |
| 32 | ESSENTIAL-3D | `0x12cc` STENCIL_MASKS | `000000ff` |  |
| 33 | ESSENTIAL-3D | `0x12d0` STENCIL_WRITE | `000000ff` |  |
| 34 | ESSENTIAL-3D | `0x12c0` ZCLIP_MIN | `80000000` |  |
| 35 | ESSENTIAL-3D | `0x12c4` ZCLIP_MAX | `7fffffff` |  |
| 36 | ESSENTIAL-3D | `0x12d4` FOG_CTL | `00000000` |  |
| 37 | ESSENTIAL-3D | `0x12d8` FOG_START | `00000000` |  |
| 38 | ESSENTIAL-3D | `0x12dc` FOG_END | `00ffffff` |  |
| 39 | BOOT-POINTER | op 0x41 preformatted blob | `41020007` | RELOCATE: program image carrying class-3 pointers |
| 40 | ESSENTIAL-3D | op 0x41 block load | `41100023` | into c[0x000..0x007] |
| 41 | ESSENTIAL-3D | op 0x41 block load | `410a0017` | into c[0x134..0x138] |
| 42 | ESSENTIAL-3D | op 0x41 block load | `410a0017` | into c[0x149..0x14d] |
| 43 | ESSENTIAL-3D | op 0x41 block load | `410a0017` | into c[0x155..0x159] |
| 44 | ESSENTIAL-3D | op 0x41 block load | `4104000b` | into c[0x160..0x161] |
| 45 | ESSENTIAL-3D | op 0x41 block load | `41080013` | into c[0x172..0x175] |
| 46 | ESSENTIAL-3D | op 0x41 block load | `41020007` | into c[0x178..0x178] |
| 47 | ESSENTIAL-3D | op 0x41 block load | `4104000b` | into c[0x17d..0x17e] |
| 48 | BOOT-POINTER | op 0x41 preformatted blob | `4106000f` | RELOCATE: program image carrying class-3 pointers |
| 49 | ESSENTIAL-3D | `0x1120` VTXDEFAULT_x | `45ffc5ff` |  |
| 50 | ESSENTIAL-3D | `0x1124` VTXDEFAULT_y | `c5ff45ff` |  |
| 51 | ESSENTIAL-3D | `0x1128` VTXDEFAULT_z | `00000000` |  |
| 52 | ESSENTIAL-3D | `0x112c` VTXDEFAULT_w | `3f800000` |  |
| 53 | ESSENTIAL-3D | `0x1110` VTXFMT0 | `00000003` |  |
| 54 | ESSENTIAL-3D | `0x1114` VTXFMT1 | `00287210` |  |
| 55 | ESSENTIAL-3D | `0x1118` VTXFMT2 | `00006543` |  |
| 56 | BOOT-POINTER | `0x1100` WARP_ENTRY | `076acd03` | RELOCATE: boot-specific absolute |
| 57 | ESSENTIAL-3D | `0x10f8` WARP_ENTRY_OBJ2 | `00000002` |  |
| 58 | BOOT-POINTER | `0x10c8` WARP_STAGE2_ENTRY | `076acf3f` | RELOCATE: boot-specific absolute |
| 59 | BOOT-POINTER | `0x10cc` WARP_STAGE3_ENTRY | `076acf4f` | RELOCATE: boot-specific absolute |
| 60 | BOOT-POINTER | `0x10d0` WARP_STAGE4_ENTRY | `076acf3f` | RELOCATE: boot-specific absolute |
| 61 | BOOT-POINTER | `0x10d4` WARP_STAGE5_ENTRY | `076acf4f` | RELOCATE: boot-specific absolute |
| 62 | BOOT-POINTER | `0x10d8` WARP_STAGE6_ENTRY | `076ac103` | RELOCATE: boot-specific absolute |
| 63 | ESSENTIAL-3D | `0x10c0` WARP_STAGE0_ENTRY | `00000000` |  |
| 64 | ESSENTIAL-3D | `0x10f0` WARP_MODE | `00000002` | bind-path MODE strobe; bit31 = software-WARP cache invalidate |
| 65 | ESSENTIAL-3D | `0x1130` WARP_PIPECTL | `0200200c` |  |
| 66 | ESSENTIAL-3D | `0x111c` REG_111C | `00000fdb` |  |
| 67 | ESSENTIAL-3D | `0x1144` REG_1144 | `00000000` |  |
| 68 | ESSENTIAL-3D | `0x10b0` PRIMWORD | `0006305c` |  |
| 69 | ESSENTIAL-3D | `0x1004` VTXBASE1 | `00000000` |  |
| 70 | ESSENTIAL-3D | `0x1008` VTXBASE2 | `00000000` |  |
| 71 | ESSENTIAL-3D | `0x100c` VTXBASE3 | `00000000` |  |
| 72 | ESSENTIAL-3D | `0x1010` VTXBASE4 | `00000000` |  |
| 73 | ESSENTIAL-3D | `0x1014` VTXBASE5 | `00000000` |  |
| 74 | ESSENTIAL-3D | `0x1018` VTXBASE6 | `00000000` |  |
| 75 | ESSENTIAL-3D | `0x101c` VTXBASE7 | `00000000` |  |
| 76 | ESSENTIAL-3D | `0x1044` VTXSTRIDE2_3 | `00000000` |  |
| 77 | ESSENTIAL-3D | `0x1048` VTXSTRIDE4_5 | `00000000` |  |
| 78 | ESSENTIAL-3D | `0x104c` VTXSTRIDE6_7 | `00000000` |  |
| 79 | ESSENTIAL-3D | `0x1064` VTXFIRST | `00000000` |  |
| 80 | ESSENTIAL-3D | `0x1060` VTXCOUNT | `000280cc` |  |
| 81 | ESSENTIAL-3D | `0x10b4` BLOCKPTR | `00000000` | resets the attribute/block write port |
| 82 | ESSENTIAL-3D | `0x10b8` ATTRDESC | `20c00000` | sequential port; the run is reset by BLOCKPTR=0 |
| 83 | ESSENTIAL-3D | `0x10b8` ATTRDESC | `a1c50020` | sequential port; the run is reset by BLOCKPTR=0 |
| 84 | ESSENTIAL-3D | `0x10b8` ATTRDESC | `00000000` | sequential port; the run is reset by BLOCKPTR=0 |
| 85 | ESSENTIAL-3D | `0x10b8` ATTRDESC | `00000000` | sequential port; the run is reset by BLOCKPTR=0 |
| 86 | BOOT-POINTER | `0x1220` SURF0_BASE | `02000000` | RELOCATE: boot-specific absolute |
| 87 | ESSENTIAL-3D | `0x1224` SURF0_CFG | `32020780` |  |
| 88 | ESSENTIAL-3D | `0x1230` SURF2_BASE | `00000000` |  |
| 89 | ESSENTIAL-3D | `0x1234` SURF2_CFG | `32020000` |  |
| 90 | BOOT-POINTER | `0x1228` SURF1_BASE | `01000000` | RELOCATE: boot-specific absolute |
| 91 | ESSENTIAL-3D | `0x122c` SURF1_CFG | `36020780` |  |
| 92 | ESSENTIAL-3D | `0x1324` PLNWT | `ffffffff` |  |
| 93 | ESSENTIAL-3D | `0x12b0` REG_12B0 | `3fff8842` |  |
| 94 | ESSENTIAL-3D | `0x1300` DWGCTL | `00a60ec4` |  |
| 95 | ESSENTIAL-3D | `0x12f4` GUARDBAND | `42000000` |  |
| 96 | ESSENTIAL-3D | `0x121c` TEXENGINE_LATCH | `00000000` |  |
| 97 | ESSENTIAL-3D | `0x1200` TEXUNIT0_CTL | `0001000f` |  |
| 98 | ESSENTIAL-3D | `0x1204` TEXUNIT1_CTL | `00000000` |  |
| 99 | ESSENTIAL-3D | `0x1208` TEXUNIT2_CTL | `00000000` |  |
| 100 | ESSENTIAL-3D | `0x120c` TEXUNIT3_CTL | `00000000` |  |
| 101 | ESSENTIAL-3D | `0x1264` COMB_RGB_SRC | `00000200` |  |
| 102 | ESSENTIAL-3D | `0x1260` COMB_ALPHA_SRC | `00000200` |  |
| 103 | ESSENTIAL-3D | `0x126c` COMB_RGB_DST | `01400400` |  |
| 104 | ESSENTIAL-3D | `0x1268` COMB_ALPHA_DST | `01400400` |  |
| 105 | ESSENTIAL-3D | `0x1278` COMB_CONST_c0_xy | `00000000` |  |
| 106 | ESSENTIAL-3D | `0x127c` COMB_CONST_c0_zw | `00000000` |  |
| 107 | ESSENTIAL-3D | `0x1280` COMB_CONST_c1_xy | `00000000` |  |
| 108 | ESSENTIAL-3D | `0x1284` COMB_CONST_c1_zw | `00000000` |  |
| 109 | ESSENTIAL-3D | `0x1264` COMB_RGB_SRC | `00000082` |  |
| 110 | ESSENTIAL-3D | `0x126c` COMB_RGB_DST | `00400400` |  |
| 111 | ESSENTIAL-3D | `0x1264` COMB_RGB_SRC | `00000084` |  |
| 112 | ESSENTIAL-3D | `0x126c` COMB_RGB_DST | `00400400` |  |
| 113 | ESSENTIAL-3D | `0x1264` COMB_RGB_SRC | `00000088` |  |
| 114 | ESSENTIAL-3D | `0x126c` COMB_RGB_DST | `00400400` |  |
| 115 | ESSENTIAL-3D | `0x1264` COMB_RGB_SRC | `0000008a` |  |
| 116 | ESSENTIAL-3D | `0x126c` COMB_RGB_DST | `00400400` |  |
| 117 | ESSENTIAL-3D | `0x1000` VTXBASE0 | `00000003` |  |
| 118 | ESSENTIAL-3D | `0x1040` VTXSTRIDE0_1 | `00000004` |  |
| 119 | ESSENTIAL-3D + BOOT-POINTER | DRAW op 0x97 | `97080001` | RELOCATE: 3 vertices, 5 dw each, from 0x16262414..0x16262450 |

Whole span, before reduction: 648 register writes  (ESSENTIAL-3D=500, 2D/X-DROPPABLE=53, BOOT-POINTER=19, SYNC/FENCE=76)

# Boot-specific absolute addresses

| sec7 offset | ring | dword | what |
|---|---|---|---|
| sec[3] | 0 | `1687a000` | FENCE_ADDR fence write-back bus address |
| sec[546] | 418 | `02000000` | SURF1_BASE surface base (VRAM byte offset, allocation-specific) |
| sec[588] | 473 | `01000000` | SURF1_BASE surface base (VRAM byte offset, allocation-specific) |
| sec[603] | 488 | `09000000` | SURF1_BASE surface base (VRAM byte offset, allocation-specific) |
| sec[618] | 503 | `018ea000` | SURF1_BASE surface base (VRAM byte offset, allocation-specific) |
| sec[685] | 566 | `076ac703` | WARP_ENTRY WARP entry -> bus 0x16405500 = (v&~3)*3, class 3 |
| sec[690] | 571 | `076ab703` | class-3 system-memory pointer inside the op-0x41 blob at sec[686] |
| sec[692] | 571 | `076aab03` | class-3 system-memory pointer inside the op-0x41 blob at sec[686] |
| sec[694] | 571 | `076ac103` | class-3 system-memory pointer inside the op-0x41 blob at sec[686] |
| sec[700] | 576 | `076ac903` | WARP_ENTRY WARP entry -> bus 0x16405b00 = (v&~3)*3, class 3 |
| sec[876] | 621 | `076acb03` | WARP_ENTRY WARP entry -> bus 0x16406100 = (v&~3)*3, class 3 |
| sec[890] | 626 | `076aab03` | class-3 system-memory pointer inside the op-0x41 blob at sec[877] |
| sec[917] | 646 | `076acd03` | WARP_ENTRY WARP entry -> bus 0x16406700 = (v&~3)*3, class 3 |
| sec[925] | 651 | `076acf3f` | WARP_STAGE2_ENTRY WARP entry -> bus 0x16406db4 = (v&~3)*3, class 3 |
| sec[926] | 651 | `076acf4f` | WARP_STAGE3_ENTRY WARP entry -> bus 0x16406de4 = (v&~3)*3, class 3 |
| sec[928] | 651 | `076acf3f` | WARP_STAGE4_ENTRY WARP entry -> bus 0x16406db4 = (v&~3)*3, class 3 |
| sec[929] | 651 | `076acf4f` | WARP_STAGE5_ENTRY WARP entry -> bus 0x16406de4 = (v&~3)*3, class 3 |
| sec[930] | 651 | `076ac103` | WARP_STAGE6_ENTRY WARP entry -> bus 0x16404300 = (v&~3)*3, class 3 |
| sec[977] | 686 | `02000000` | SURF0_BASE surface base (VRAM byte offset, allocation-specific) |
| sec[983] | 691 | `01000000` | SURF1_BASE surface base (VRAM byte offset, allocation-specific) |
| sec[1039] | 744 | `16262414` | DRAW vertex buffer start (absolute bus address) |
| sec[1040] | 744 | `16262450` | DRAW vertex buffer end (absolute bus address) |
| ring inline | 10 | `1c9b6020` | FENCE_ADDR fence write-back bus address |
| ring inline | 513 | `1c9b6020` | FENCE_ADDR fence write-back bus address |
| ring inline | 736 | `1c9b6020` | FENCE_ADDR fence write-back bus address |

# Completeness cross-check

## State a triangle needs, and whether the span writes it

| reg | name / role | written | final value |
|---|---|---|---|
| `0x1220` | SURF0_BASE   colour render target base | yes | `02000000` |
| `0x1224` | SURF0_CFG    colour pitch/format | yes | `32020780` |
| `0x1228` | SURF1_BASE   second surface base | yes | `01000000` |
| `0x122c` | SURF1_CFG    second surface pitch/format | yes | `36020780` |
| `0x12c8` | DEPTH_STENCIL depth test/write + stencil func | yes | `001000e4` |
| `0x12cc` | STENCIL_MASKS | yes | `000000ff` |
| `0x12d0` | STENCIL_WRITE depth write enable lives in bit8 | yes | `000000ff` |
| `0x12e4` | ALPHATEST | yes | `0000000e` |
| `0x12e8` | BLENDCTL | yes | `00800202` |
| `0x12fc` | RASTERMODE   cull select | yes | `0bb80024` |
| `0x1300` | DWGCTL       3D assembly word | yes | `00a60ec4` |
| `0x1324` | PLNWT        plane write mask | yes | `ffffffff` |
| `0x1328` | SCISSOR_MIN | yes | `00000000` |
| `0x132c` | SCISSOR_MAX | yes | `00f00140` |
| `0x10b0` | PRIMWORD     primitive type | yes | `0006305c` |
| `0x10b8` | ATTRDESC     vertex attribute descriptors | yes | `00000000` |
| `0x1100` | WARP_ENTRY   vertex program entry | yes | `076acd03` |
| `0x10f0` | WARP_MODE | yes | `00000002` |
| `0x1000` | VTXBASE0     vertex stream base + memory class | yes | `00000003` |
| `0x1060` | VTXCOUNT | yes | `000280cc` |
| `0x1064` | VTXFIRST | yes | `00000000` |
| `0x1264` | COMB_RGB_SRC fragment combiner | yes | `0000008a` |
| `0x126c` | COMB_RGB_DST fragment combiner | yes | `00400400` |
| `0x1200` | TEXUNIT0_CTL | yes | `0001000f` |
| `0x12f4` | GUARDBAND | yes | `42000000` |

## Surface descriptor trace (0x1220..0x123c)

| seq | ring | reg | value | decode |
|---|---|---|---|---|
| 24 | 33 | `0x1220` SURF0_BASE | `00000000` | base 0x00000000 | class 0 |
| 25 | 33 | `0x1224` SURF0_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 80 | 63 | `0x1220` SURF0_BASE | `00000000` | base 0x00000000 | class 0 |
| 81 | 63 | `0x1224` SURF0_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 147 | 123 | `0x1220` SURF0_BASE | `00000000` | base 0x00000000 | class 0 |
| 148 | 123 | `0x1224` SURF0_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 149 | 123 | `0x1230` SURF2_BASE | `00000000` | base 0x00000000 | class 0 |
| 150 | 123 | `0x1234` SURF2_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 247 | 258 | `0x1228` SURF1_BASE | `00000000` | base 0x00000000 | class 0 |
| 248 | 258 | `0x122c` SURF1_CFG | `30030780` | pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0 |
| 249 | 258 | `0x1228` SURF1_BASE | `00000000` | base 0x00000000 | class 0 |
| 250 | 258 | `0x122c` SURF1_CFG | `30030780` | pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0 |
| 276 | 283 | `0x1220` SURF0_BASE | `00000000` | base 0x00000000 | class 0 |
| 277 | 283 | `0x1224` SURF0_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 332 | 313 | `0x1220` SURF0_BASE | `00000000` | base 0x00000000 | class 0 |
| 333 | 313 | `0x1224` SURF0_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 444 | 413 | `0x1220` SURF0_BASE | `00000000` | base 0x00000000 | class 0 |
| 445 | 413 | `0x1224` SURF0_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 446 | 413 | `0x1230` SURF2_BASE | `00000000` | base 0x00000000 | class 0 |
| 447 | 413 | `0x1234` SURF2_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 448 | 418 | `0x1228` SURF1_BASE | `02000000` | base 0x02000000 | class 0 |
| 449 | 418 | `0x122c` SURF1_CFG | `30030780` | pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0 |
| 450 | 423 | `0x1238` SURF3_BASE | `00000000` | base 0x00000000 | class 0 |
| 451 | 423 | `0x123c` SURF3_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 474 | 473 | `0x1228` SURF1_BASE | `01000000` | base 0x01000000 | class 0 |
| 475 | 473 | `0x122c` SURF1_CFG | `30030780` | pitch 1920 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0 |
| 487 | 488 | `0x1228` SURF1_BASE | `09000000` | base 0x09000000 | class 0 |
| 488 | 488 | `0x122c` SURF1_CFG | `30030020` | pitch 32 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0 |
| 500 | 503 | `0x1228` SURF1_BASE | `018ea000` | base 0x018ea000 | class 0 |
| 501 | 503 | `0x122c` SURF1_CFG | `30030020` | pitch 32 px; type 3; swap 0; format 0x30=fmt 0x30; tiling 0 |
| 609 | 686 | `0x1220` SURF0_BASE | `02000000` | base 0x02000000 | class 0 |
| 610 | 686 | `0x1224` SURF0_CFG | `32020780` | pitch 1920 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 611 | 686 | `0x1230` SURF2_BASE | `00000000` | base 0x00000000 | class 0 |
| 612 | 686 | `0x1234` SURF2_CFG | `32020000` | pitch 0 px; type 2; swap 0; format 0x32=ARGB8888; tiling 0 |
| 613 | 691 | `0x1228` SURF1_BASE | `01000000` | base 0x01000000 | class 0 |
| 614 | 691 | `0x122c` SURF1_CFG | `36020780` | pitch 1920 px; type 2; swap 0; format 0x36=[INF] S8Z24 (32bpp class); tiling 0 |

## Scissor / clip trace (0x1328, 0x132c, 0x138c, 0x1304, 0x1308)

| seq | ring | reg | value | decode |
|---|---|---|---|---|
| 16 | 28 | `0x1324` PLNWT | `00000000` | plane write mask 0x00000000 |
| 19 | 33 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 20 | 33 | `0x132c` SCISSOR_MAX | `00010001` | (y=1, x=1) |
| 75 | 63 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 76 | 63 | `0x132c` SCISSOR_MAX | `00010001` | (y=1, x=1) |
| 132 | 98 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 133 | 98 | `0x132c` SCISSOR_MAX | `00000000` | (y=0, x=0) |
| 151 | 128 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |
| 225 | 232 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |
| 229 | 237 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 230 | 237 | `0x132c` SCISSOR_MAX | `10001000` | (y=4096, x=4096) |
| 231 | 237 | `0x1304` SRC_XY | `00000000` | (y=0, x=0) |
| 232 | 237 | `0x1308` DST_XY | `00000000` | (y=0, x=0) |
| 255 | 258 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |
| 258 | 258 | `0x1308` DST_XY | `00000000` | (y=0, x=0) |
| 259 | 258 | `0x138c` LEN | `00f00140` | height 240, width 320 |
| 268 | 278 | `0x1324` PLNWT | `00000000` | plane write mask 0x00000000 |
| 271 | 283 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 272 | 283 | `0x132c` SCISSOR_MAX | `00010001` | (y=1, x=1) |
| 327 | 313 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 328 | 313 | `0x132c` SCISSOR_MAX | `00010001` | (y=1, x=1) |
| 384 | 348 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 385 | 348 | `0x132c` SCISSOR_MAX | `00000000` | (y=0, x=0) |
| 454 | 433 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |
| 460 | 448 | `0x1308` DST_XY | `00000000` | (y=0, x=0) |
| 461 | 448 | `0x1304` SRC_XY | `00000000` | (y=0, x=0) |
| 462 | 448 | `0x138c` LEN | `04b00780` | height 1200, width 1920 |
| 478 | 473 | `0x1308` DST_XY | `00000000` | (y=0, x=0) |
| 479 | 473 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |
| 480 | 473 | `0x138c` LEN | `04b00780` | height 1200, width 1920 |
| 491 | 488 | `0x1308` DST_XY | `00000000` | (y=0, x=0) |
| 492 | 488 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |
| 493 | 488 | `0x138c` LEN | `00200020` | height 32, width 32 |
| 504 | 503 | `0x1308` DST_XY | `00000000` | (y=0, x=0) |
| 505 | 503 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |
| 506 | 503 | `0x138c` LEN | `00200020` | height 32, width 32 |
| 523 | 536 | `0x1328` SCISSOR_MIN | `00000000` | (y=0, x=0) |
| 524 | 536 | `0x132c` SCISSOR_MAX | `00f00140` | (y=240, x=320) |
| 615 | 696 | `0x1324` PLNWT | `ffffffff` | plane write mask 0xffffffff = all planes |

## Never written in this span

- **clear colour / clear of the render target** - no 2D fill or 3D clear targets SURF0 base 0x02000000 anywhere in the span; the span assumes the frame was already cleared before it
- **depth buffer contents** - SURF1 is bound as a surface but never cleared in this span
- **0x1220/0x1224 pair at the moment of the draw** - see the surface trace below - the pair is rewritten three times and the last writer wins

