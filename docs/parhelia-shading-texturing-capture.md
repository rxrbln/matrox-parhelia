# Gouraud and texturing, captured and diffed against the flat triangle

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

`tools/gltri.c` now has three modes selected by `GLTRI_MODE`, each drawing the
same triangle with exactly one GL feature changed, so the traces diff cleanly:

| mode | what |
|---|---|
| 0 | flat, one `glColor3f`, `GL_FLAT` (the original baseline) |
| 1 | Gouraud, `GL_SMOOTH`, red/green/blue per vertex |
| 2 | textured, 64x64 procedural checkerboard, `GL_NEAREST`, per-vertex `glTexCoord2f` |

Captured on the original Parhelia 0x0528 (PCI-X, 256 MB), vendor 2009 mtx stack,
`libGL` "1.3 ICD v1.03", X 1920x1200x24 with the `mtx` driver, all three in one
session with one binary:

    captures/gltri-flat-0528-trace.txt
    captures/gltri-gouraud-0528-trace.txt
    captures/gltri-textured-0528-trace.txt

The archived `captures/gltri-0528-trace.txt` is an earlier flat run and is used
below only as an independent cross-check.

## Verified: all three draws happened

Measured, not assumed. `glReadPixels` hangs this stack, but the X server reads
the same pixels back through the 2D path, so `GLTRI_XCAP=1` does an `XGetImage`
of the window and histograms it. `GLTRI_SWAP=1` is needed for that (see the trap
below), so the verification runs carry a trailing `glXSwapBuffers` that the
capture runs do not; everything else is the same binary and the same draw.

    GLTRI_MODE=0 GLTRI_SWAP=1 GLTRI_XCAP=1 ./gltri
      xcap 76800 px, 2 distinct colours
        000000 48800        00ff00 28000

    GLTRI_MODE=1 GLTRI_SWAP=1 GLTRI_XCAP=1 ./gltri
      xcap 76800 px, 25040 distinct colours
        000000 48800     (28000 non-black, spread over 25039 colours)

    GLTRI_MODE=2 GLTRI_SWAP=1 GLTRI_XCAP=1 ./gltri
      xcap 76800 px, 7 distinct colours
        000000 48800
        00ffff 7000   00fffe 3517   00feff 3480     cyan  family 13997
        ff00ff 6975   fe00ff 3515   ff00fe 3513     magenta family 14003

**28000 non-black pixels in every mode**, which is exactly
`(320-40) * (240-40) / 2`, the analytic area of the requested triangle. Flat
produces one colour, Gouraud 25039, textured two colour families that sum to
28000 and split almost exactly in half, as a 4x-repeated 8-texel checkerboard
across this triangle should.

The 1-LSB variants (`00fffe`, `fe00ff`, …) are the `GL_MODULATE` multiply by
`glColor3f(1,1,1)` rounding down in the combiner's fixed-point format. They are
a side observation, not part of the claim.

### Two instruments, and which one to trust

`3dtri --findcolor` over VRAM was the instrument the flat capture originally
used. It is *not* reliable here:

* Without a swap, the triangle is **not in the first 16 MB at all** — a fresh,
  never-used colour scans as `none in 16 MB`. The visual this program gets is
  double buffered (see the trap below) and the back buffer sits high in VRAM.
* After a swap, a fresh colour scans as **36178 px** in the front-buffer region,
  reproducibly and with an identical byte range for two different fresh
  colours — 8178 more than the 28000 the window actually contains. So the linear
  scan sees the colour somewhere beyond the window rectangle as well. What that
  second population is has not been established.

The `XGetImage` count is exact, repeatable and lands on the analytic area three
times over, so it is the measurement used throughout. The VRAM numbers are
recorded here as an observation, not as a verification.

## Shape of the captures

Both untextured modes are 19 doorbells, identical structure to the archived flat
capture. Texturing adds three:

| doorbell | flat / Gouraud | textured |
|---|---|---|
| 0-16 | context bring-up, identical in all three | identical |
| 17 | `cmd 0x01` RENDER — **the draw** | `cmd 0x01` RENDER — texture bind, 408 B stream |
| 18 | `cmd 0x1e` detach | `cmd 0x0c` **buffer create — the texture** |
| 19 | | `cmd 0x01` RENDER — **the draw** |
| 20 | | `cmd 0x0d` |
| 21 | | `cmd 0x1e` detach |

The seven mmap'd regions are the same set in all three runs (cookies differ per
run): control page 4 KB, a second 4 KB page, record stream 0x22000, a third 4 KB
page, vertex ring 0x320000, command ring 0xa8000, and a 4 MB region.

**The context bring-up is byte-identical across all three modes.** Submit 5, the
1148-dword context-arm stream, compares equal dword for dword between flat,
Gouraud and textured. Every mode-specific bit of state appears only in the draw
submit — the ICD defers all of it.

The declared stream length at `control+0x230` matches the record parse exactly in
all five state submits, so nothing below is a mis-slice:

| submit | `control[0x230]` | records parsed to |
|---|---|---|
| flat 17 | 4496 B = 1124 dw | 1124 dw |
| Gouraud 17 | 4488 B = 1122 dw | 1122 dw |
| textured 17 | 408 B = 102 dw | 102 dw |
| textured 19 | 4216 B = 1054 dw | 1054 dw |

## The draw packet does not change at all

Command ring, offset 0, in all three modes:

```
+0x00  8f000001   draw, index mode
+0x04  00000000   first index
+0x08  00000003   3 vertices
+0x0c  00000002   register record: 2 dwords
+0x10  21212425   indices LSB first: 0x25, 0x24, pad, pad
+0x14  00000002     idx 0x25 -> 0x1094 = 2
+0x18  <cookie>     idx 0x24 -> 0x1090 = write-back address (per-run)
```

Only `+0x18` differs, and only because it is that run's write-back page address.
Shading and texturing are entirely a matter of vertex layout and state records.

## Gouraud: the vertex already carried the colour

Vertex ring, base offset `0x40`, from the traces:

```
flat      +0x40  41a00000 41a00000 00000000 3f800000 00000000     20 20  0 1 0
          +0x54  43960000 41a00000 00000000 3f800000 00000000    300 20  0 1 0
          +0x68  43200000 435c0000 00000000 3f800000 ...         160 220 0 1 0

Gouraud   +0x40  41a00000 41a00000 3f800000 00000000 00000000     20 20  1 0 0
          +0x54  43960000 41a00000 00000000 3f800000 00000000    300 20  0 1 0
          +0x68  43200000 435c0000 00000000 00000000 3f800000    160 220 0 0 1
```

**The stride does not grow. It is 20 bytes = 5 dwords in both modes**, and the
layout is

    struct vertex { float x, y, r, g, b; };

Dwords 2..4 are the colour, in **R, G, B order**, as float 0..1. The Gouraud run
pins this beyond doubt: vertex 0 is `(1,0,0)`, vertex 1 `(0,1,0)`, vertex 2
`(0,0,1)`, exactly the three `glColor3f` calls the client makes, in order.

There is no z and no w in the vertex — the client uses `glVertex2f`, and the
vertex program supplies the rest.

### Correction to `parhelia-minimal-triangle.md`

That document states "No per-vertex colour appears: with `glShadeModel(GL_FLAT)`
and one `glColor3f`, the colour travels in the state stream, not the vertex."
**That is wrong.** The flat vertex carries the colour too; it is simply the same
in all three vertices, so most of it shows up `.`-flagged (unchanged from the
previous submit) and was read as padding.

The archived capture proves it on its own: that run used `GLTRI_RGB=0xff00ff`
and its vertices are `(20, 20, 1.0, 0.0, 1.0)` — magenta. The fresh flat capture
here used the default `0x00ff00` and its vertices are `(20, 20, 0.0, 1.0, 0.0)` —
green. The colour tracks `GLTRI_RGB` in the vertex.

### The state delta for Gouraud is a single slot

The two record streams are identical except for **one** `(id, value)` pair in the
key-`0x0c` flat-state record, whose count drops 37 -> 36 and shifts everything
after it by 2 dwords:

    flat draw:     0x0c id 0x09 = 1
    Gouraud draw:  (absent)

The context-arm submit (5) sets `id 0x09 = 2` in both runs. So:

**key `0x0c`, id `0x09` is the shade model: 1 = `GL_FLAT`, 2 = `GL_SMOOTH`.**
Gouraud is the ICD's initialised default and simply is not re-sent.

Nothing else moves. The key-`0x46` combiner program object is identical between
flat and Gouraud apart from items 411..413, which the record-stream document
already marks as "not read on any traced path" and which differ run to run —
uninitialised tail, not state. The key-`0x05` WARP bind is `(0x13,1) (0x14,2)` in
both: **the same vertex program serial drives flat and smooth shading.**

## Textured: upload, bind, sampler, vertex

### The vertex grows by two floats

```
textured  +0x40  41a00000 41a00000 3f800000 3f800000 3f800000 00000000 00000000
                 20 20   1 1 1   s=0    t=0
          +0x5c  43960000 41a00000 3f800000 3f800000 3f800000 40800000 00000000
                 300 20  1 1 1   s=4    t=0
          +0x78  43200000 435c0000 3f800000 3f800000 3f800000 40000000 40800000
                 160 220 1 1 1   s=2    t=4
```

**Stride 0x1c = 28 bytes = 7 dwords**, base offset `0x40` unchanged:

    struct vertex { float x, y, r, g, b, s, t; };

The texture coordinates are appended *after* the colour, and they are exactly the
three `glTexCoord2f` calls: `(0,0)`, `(4,0)`, `(2,4)`. The colour stays present
and is `(1,1,1)`, the `glColor3f` the textured mode sets so `GL_MODULATE` passes
the texel through.

The stride is declared to the kernel in the **key `0x08`** record, payload dword
4:

    flat / Gouraud   key 0x08  +4 = 0x00000014   =  20
    textured         key 0x08  +4 = 0x0000001c   =  28

and dwords 2..3 of the same record are the 64-bit kernel pointer to the vertex
buffer. The attribute set is declared in **key `0x07`**, payload dword 0:

    flat / Gouraud   key 0x07  +0 = 0x00000010
    textured         key 0x07  +0 = 0x00020010

so **bit 17 (`0x00020000`) is the texcoord-present bit** in the vertex format
word. Bit 4 (`0x10`) is set in every mode.

### The texture image: where it goes and what it looks like

The image is written by the client into the **4 MB region** — the one created at
bring-up by the in-band request `cmd 0x04`, and never touched at all in the flat
and Gouraud runs. In the textured run it takes **4096 dwords at offset 0**, all
`D`-flagged (client-authored, no kernel write-back):

    64 * 64 * 4 = 16384 bytes = 4096 dwords, exactly

Decoded from the trace:

* **linear, row major, 64 texels per row, pitch 256 bytes, no padding and no
  tiling or swizzling.** Row 0 and row 1 are identical; runs along row 0 and down
  column 0 are 8,8,8,8,8,8,8,8 — the 8-texel checkerboard cell, in both axes.
* Two distinct values only, 2048 texels each: `0xff00ffff` and `0xffff00ff`.
* The client wrote source bytes R,G,B,A. The stored dwords are
  `A<<24 | R<<16 | G<<8 | B`, i.e. **byte order B, G, R, A — ARGB8888 little
  endian**, the same format code the display surface uses.

Nothing new is mmap'd for the texture. The image lands in the pre-existing 4 MB
arena, which is therefore the client-visible texture upload region.

### The texture buffer create

Doorbell 18 is `control+0x00 = 0x0c`, the in-band buffer create, with
`control+0x10` pointing at a request struct in the client heap. The tracer dumps
it:

```
HEAPREQ cmd=0000000c ptr=0x8a1b20
  00000000 00000003 00000015 00000040 00000040 00000000 00000006
   type=0   count=3  fmt=0x15  w=64     h=64             flags=6
```

Compare the two bring-up creates from the same trace:

```
  00000081 00000001 00000015 00000780 000004b0 00000001 00000001   1920x1200 fmt 0x15
  00000002 00000001 0000004b 00000780 000004b0 00000000 00000000   1920x1200 fmt 0x4b
```

So the request struct is `{type, count, format, width, height, ...}` as
`parhelia-minimal-triangle.md` records, the width/height fields carry the texture
dimensions verbatim, and **format `0x15` is ARGB8888** — the same code the
primary surface uses, which independently confirms the texel byte order decoded
above. `count = 3` and `flags = 6` are not identified; a single-level 64x64
texture with `count = 3` argues against `count` being a mip level count, but this
capture cannot settle it.

The kernel replies in the control page at **`control+0x18`**, `K`-flagged: `4`
for this create, against `1` for the first bring-up create. A small ascending
handle, not an address.

### The texture bind stream (doorbell 17)

408 bytes. It is the standard prologue — key `0x0d` opener, `0x35`, the 37-entry
key `0x0c` flat state (with `id 0x09 = 1`, still `GL_FLAT` in this mode), key
`0x12` rect — plus **one record the untextured runs never emit**:

    key 0x06  count 1     payload  00060000  000003e8

Under the slot-word split documented in `parhelia-record-stream.md` (unit in the
low half, field in the high half — which the context-arm stream confirms, since
it enumerates fields 1..9 for units 0,1,2,3 in that order) this is **unit 0,
field 6 = 0x3e8 = 1000**. Field 6's context-init value is `447a0000` = 1000.0f,
so the same field is being rewritten with the *integer* 1000. The slot is
identified; the value's meaning is **not resolved** and should not be guessed.

### The sampler state in the draw (doorbell 19)

Two **key `0x0e`** records, the per-texture-unit state, one at the head of the
stream and one just before the buffer descriptors:

    key 0x0e  00000000 = 00000004      unit 0, field 0x00 = 4
    key 0x0e  00180000 = 00000002      unit 0, field 0x18 = 2

Both fields are 0 in the context-arm stream, so these two writes are the entire
sampler delta for `GL_NEAREST` + `GL_REPEAT` texturing on unit 0. Units 1..3 are
not touched. Field 0 and field 0x18 are not further identified here — a
filter/wrap sweep against `tools/parhelia-texsweep.c` is the way to name them.

### The combiner program turns texturing on

Key `0x46`, the 414-dword combiner program object, differs from flat in exactly
four meaningful items (411..413 are the run-to-run junk tail):

| item | flat | textured | meaning |
|---|---|---|---|
| 3 | `0` | `3` | combiner block 0, RGB half dword 0 |
| 7 | `0x10` | `0` | RGB half dword 4 |
| 10 | `0xff` | `0x10` | RGB half dword 7 |
| **375** | `0` | **`1`** | **texture-unit descriptor 0, dword 0** |

Item 375 is the first dword of the first 9-dword texture-unit descriptor.
`parhelia-record-stream.md` predicted this from an earlier `tex_tri_nearest`
capture ("texdesc[0].dw0 = 1 when textured"); this is that prediction confirmed
on a draw verified pixel by pixel. Descriptors 1..3 stay all-zero.

The RGB half of combiner block 0 as a whole:

```
flat       0x00 0x04 0x00 0x01 0x10 0x00 0x00 0xff 0x00 0x00 0xff 0x00 0x00
textured   0x03 0x04 0x00 0x01 0x00 0x00 0x00 0x10 0x00 0x00 0xff 0x00 0x00
```

dword 0 goes 0 -> 3, and the operand token `0x10` moves from dword 4 to dword 7,
with dword 4 becoming 0. Read against the fixed-function texenv lowering in
`parhelia-fragment-isa.md`, that is `GL_MODULATE` being built: dword 0 selects
the source pair, source A becomes texture unit 0 (`0`) and the interpolated
colour (`0x10`) moves to source B, displacing the `0xff` constant that the
untextured program multiplied by. **That reading is inference from the shape of
the change; only the item positions and values above are measured.** The ALPHA
half is byte-identical between the two.

The **key `0x05` WARP bind is unchanged**: `(0x13,1) (0x14,2)`, program serial 2,
the same one flat and Gouraud use. So the vertex program is not respecialised for
texturing — the extra two floats are described to the hardware by the key `0x07`
format word and the key `0x08` stride, not by a different microcode.

### Where the texture object is bound

The draw stream opens with **key `0x0a`** (72-byte item, "inline ring/fence
emit" in the key inventory), which the untextured draws do not carry:

```
+0  0150a004 ffffc200   kernel pointer 0xffffc200_0150a004
+4  00000004
+9  00000040   width  64
+10 00000040   height 64
+11 00000001
+15 00000025
+17 00000002
```

The width and height are the texture's, so this record binds the texture object
created at doorbell 18. The other kernel pointers in the same stream are the
vertex buffer (key `0x08`, `0xffffc200_01431004`) and key `0x1b`
(`0xffffc200_0144a004`), all from the same kernel object table.

## Summary of the deltas

| | flat | Gouraud | textured |
|---|---|---|---|
| vertex stride | 20 B | **20 B** | **28 B** |
| vertex layout | x y r g b | x y r g b | x y r g b **s t** |
| key `0x07` format word | `0x10` | `0x10` | **`0x20010`** |
| key `0x08` stride field | `0x14` | `0x14` | **`0x1c`** |
| key `0x0c` id `0x09` | **1** | **absent (2)** | 1 |
| key `0x0e` sampler | — | — | **unit0 f0=4, f0x18=2** |
| key `0x06` | — | — | **unit0 f6=1000** |
| key `0x46` item 375 | 0 | 0 | **1** |
| key `0x46` block0 RGB | `00 04 00 01 10 …` | same | **`03 04 00 01 00 … 10 …`** |
| key `0x05` WARP bind | `(13,1)(14,2)` | same | same |
| draw packet | 7 dwords | identical | identical |
| doorbells | 19 | 19 | **22** |

## Traps added by this work

* **`glTexImage2D` spins forever unless the client links `-lm`.** The vendor
  `libGL` calls `ceilf` in its texture path; without `libm` among the
  executable's direct dependencies the symbol binds into a `libstdc++` PLT stub
  that never returns, and the process burns CPU in `ceilf@plt` indefinitely. A
  `gdb -p ... -ex bt` on the spinning process shows it in one line. The link is
  now `-lGL -lX11 -lm -Wl,--allow-shlib-undefined`. This is the same
  `GLIBCXX_3.4` versioning mismatch that already forced
  `--allow-shlib-undefined`; that flag hides the problem instead of fixing it.
* **The visual is double buffered, whatever `parhelia-minimal-triangle.md` says.**
  `glXChooseVisual` is asked for `GLX_RGBA` without `GLX_DOUBLEBUFFER`, which is
  a preference, not a requirement, and this server has no single-buffered RGBA
  visual to give. The draw therefore lands in a back buffer high in VRAM, an
  `XGetImage` of the window returns a uniform background colour, and a VRAM scan
  of the low 16 MB finds nothing. Add `glXSwapBuffers` before verifying.
* **`XGetImage` works on this stack and `glReadPixels` does not.** It is the
  cheapest exact instrument available here and it needs no VRAM offset guessing.
  `GLTRI_XCAP=1` uses it; `GLTRI_XCAP_PPM=<path>` also writes the window out.
* A VRAM colour scan cannot tell which surface it is looking at. Two different
  fresh colours both scanned as 36178 px over the same byte range against a
  window that demonstrably held 28000 — reproducible, and still unexplained.

## Tooling changes

* `tools/gltri.c`: `GLTRI_MODE` (0 flat / 1 Gouraud / 2 textured), the
  procedural checkerboard (`GLTRI_TEXDIM`, `GLTRI_TEXCELL`, `GLTRI_TEXREP`,
  `GLTRI_TEXA`, `GLTRI_TEXB`, `GLTRI_TEXIFMT`, `GLTRI_TEXFMT`), and
  `GLTRI_XCAP` / `GLTRI_XCAP_PPM` window read-back with a colour histogram.
* `tools/parhelia-3dtri.c`: `--colorhist [N]` and `--histrange lo hi`, a top-N
  24-bit colour histogram over the same VRAM scan `--findcolor` uses. A flat draw
  puts one colour on screen and the other two modes put many, so the shape of the
  histogram is what separates them — but see the caveat above about which surface
  a VRAM scan is actually reading.

## Next

* Name key `0x0e` fields 0 and 0x18 by sweeping filter and wrap modes with
  `tools/parhelia-texsweep.c` against this same verified harness.
* Resolve key `0x06` unit 0 field 6 = 1000; the integer-into-a-float-field
  reading is unexplained.
* Establish what `count = 3` and `flags = 6` mean in the texture create request,
  by capturing a mipmapped and a non-power-of-two upload.
* Explain the 36178-vs-28000 VRAM scan discrepancy, or retire VRAM scanning as a
  verification instrument in favour of `XGetImage`.
