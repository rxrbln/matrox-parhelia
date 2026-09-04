# Parhelia hardware cursor — captured upload format

Captured 2026-09-05 on a live original Parhelia 0x0528 running the vendor 2009
driver with `Using hardware cursor` / `Initializing HW cursor` confirmed in the
X log. Instrument: `tools/parhelia-cursorcap.c`, which sets a cursor whose every
row encodes its own row index, then dumps the cursor registers and the VRAM the
cursor lives in. Because row *i* carries the value *i*, the row pitch and plane
layout are read directly off the dump instead of inferred.

Raw captures: `captures/cursor-core-0528.txt`, `captures/cursor-argb-0528.txt`.

## The core (mono) upload format — SOLVED

Source image used: 64x64, row *i* has source byte 0 = *i*, remaining source
bytes 0, mask all ones. What lands in VRAM:

```
0x000: 00 00 00 00 00 00 00 00  ff ff ff ff ff ff ff ff   row 0: plane0 | plane1
0x010: 00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00   16 bytes padding
0x020: 01 00 00 00 00 00 00 00  ff ff ff ff ff ff ff ff   row 1
0x030: 00 ... (pad)
0x040: 02 ...                                             row 2
```

Row index found at 0x000, 0x020, 0x040 … 0x160 for rows 0..11 — twelve
consecutive rows, so this is not a coincidence.

| Property | Value |
|---|---|
| **Row pitch** | **0x20 = 32 bytes** |
| plane 0 (source / AND) | row base + 0x00, 8 bytes, MSB-first |
| plane 1 (mask / XOR) | row base + 0x08, 8 bytes, MSB-first |
| padding | row base + 0x10, 16 bytes, zero |
| cursor size | 64 x 64 |
| format register `0x1ad0` | `0x05000000` for this mode |
| base register `0x1acc` | raw byte offset (see caveat below) |

**This is almost certainly the long-standing "row 0 renders but rows > 0 never
land" bug.** A 64-wide mono cursor with two 8-byte planes looks like it should
pack at 16 bytes per row; the hardware actually strides **32**. With a 16-byte
assumption row 0 lands correctly and every later row lands at half its proper
offset — exactly the reported symptom. The driver currently sidesteps this by
converting core cursors into the 16-colour surface (128-byte stride) because
"the 2-colour mode never worked"; with the pitch above the 2-colour path should
be fixable.

## Caveat: aperture vs surface — the image is NOT where 0x1acc points

The X log reports two locations, and they are different:

```
Cursor Aperture location 0xec0000
Cursor Surface location  0x7f60000
```

`0x1acc` reads `0x07f60000`, matching the **Surface** exactly — which confirms
that register holds a **raw byte offset**, not a shifted one. But the uploaded
image was found at the **Aperture**, `0xec0000`. The surface address contained
unrelated data that did not change between two different cursors.

So the CPU-side upload path and the address the cursor registers name are not
the same allocation. Which one the scanout hardware actually reads is **not
established** by this capture, and a driver should not assume. Resolve before
relying on either.

## Registers observed

```
0x1acc = 0x07f60000   base (raw byte offset; = "Cursor Surface" from the X log)
0x1ad0 = 0x05000000   format, core/mono cursor
0x1ad0 = 0x04000000   format, after setting an ARGB cursor
0x1ac4 = 0x00000000   control - reads back zero even though the HW cursor is on
0x1ac8 = 0x00000000   position - likewise
0x1a88 = 0x00000000   head bind - likewise
```

The control, position and bind registers reading zero while the hardware cursor
is demonstrably active is consistent with the register-readback unreliability
already seen elsewhere on this chip. Treat these as write-only; verify cursor
state through VRAM effects, not readback.

## The ARGB upload format — SOLVED (planar, 16 colours + 5 alpha planes)

The first attempt failed and the failure was instructive: an arbitrary marker
byte planted in every pixel appeared **zero times**. The hardware has no 32-bit
cursor — the vendor **quantises ARGB to a 16-entry palette plus alpha planes**,
which destroys an arbitrary colour by design. Re-capturing with an image built
from only 16 distinct colours (colour index = row & 15) and a per-row alpha ramp
makes the format fall straight out.

**Row stride is 0x80 = 128 bytes**, and each row is a set of **8-byte bitplanes**
(64 pixels, one bit per pixel, MSB-first):

| Plane | Row offset | Meaning |
|---|---|---|
| 0 | +0x00 | colour index bit 0 |
| 1 | +0x08 | colour index bit 1 |
| 2 | +0x10 | colour index bit 2 |
| 3 | +0x18 | colour index bit 3 |
| 4 | +0x20 | alpha bit 3 (LSB of the retained alpha) |
| 5 | +0x28 | alpha bit 4 |
| 6 | +0x30 | alpha bit 5 |
| 7 | +0x38 | alpha bit 6 |
| 8 | +0x40 | alpha bit 7 (MSB) |
| — | +0x48..0x7f | padding |

So **9 planes x 8 bytes = 72 bytes used of a 128-byte row**, giving a 4-bit
palette index and **5 bits of per-pixel alpha — the top 5 bits of the 8-bit
alpha; the low 3 bits are discarded.**

Verification, read directly off the capture (`captures/cursor-argb-0528.txt`):

```
row  colidx alpha | planes 0..8
  1     1      4  | FF .. .. ..  .. .. .. .. ..     index bit0
  2     2      8  | .. FF .. ..  FF .. .. .. ..     index bit1, alpha bit3
  3     3     12  | FF FF .. ..  FF .. .. .. ..     alpha 12 -> bit3 kept, bit2 dropped
  4     4     16  | .. .. FF ..  .. FF .. .. ..     index bit2, alpha bit4
  8     8     32  | .. .. .. FF  .. .. FF .. ..     index bit3, alpha bit5
 11    11     44  | FF FF .. FF  FF .. FF .. ..     idx 0b1011, alpha 0b101100
```

Every row matches its colour index and its alpha bit pattern exactly, including
the deliberate truncation (alpha 12 and 28 both lose bit 2). This **confirms on
hardware** the static RE note that the ARGB path is a 16-colour surface with a
128-byte row stride and 5 alpha planes.

Format register `0x1ad0` = `0x04000000` for this mode, versus `0x05000000` for
the core mono mode.

**The 16-entry palette — NOT open after all.** It was already recovered in
`src/parhelia_reg.h` and I flagged it open without checking there first. The
cursor block exposes an **indexed LUT port** at the DC cursor base:

| Register | head 1 | Role |
|---|---|---|
| `PARHELIA_CURSOR_COLOR` (+0x000) | 0x1ac0 | LUT colour data, RGB888 |
| `PARHELIA_CURSOR_CTL` (+0x004) | 0x1ac4 | `(lut_index << 8) \| mode` |

So an entry is written by setting `0x1ac4 = (index<<8)|mode` and then writing
the RGB888 value to `0x1ac0`; the driver uploads up to 16 entries this way. That
also explains why `0x1ac4` read back as 0 in the capture — it is an index/mode
port, not a readable control word.

**The capture confirms the mode constants on hardware.** The values observed in
`0x1ad0` match the driver's existing definitions exactly:

```
PARHELIA_CURSOR_CFG_2COLOR   0x05000000   <- observed for the core mono cursor
PARHELIA_CURSOR_CFG_16COLOR  0x04000000   <- observed for the ARGB cursor
```

with `PARHELIA_CURSOR_CTL_2COLOR` = mode 3 and `PARHELIA_CURSOR_CTL_16COLOR` =
mode 4. Previously these rested on disassembly alone.

## Summary of the two formats

| | core / mono | ARGB (16-colour + alpha) |
|---|---|---|
| row stride | **32 bytes** | **128 bytes** |
| planes per row | 2 (source, mask), 8 bytes each | 9 (4 index + 5 alpha), 8 bytes each |
| bytes used per row | 16 of 32 | 72 of 128 |
| `0x1ad0` | `0x05000000` | `0x04000000` |
| colour | fg/bg registers | 16-entry palette (location not captured) |
| alpha | none | 5 bits per pixel (top 5 of 8) |
