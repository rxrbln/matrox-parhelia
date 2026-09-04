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

## ARGB — NOT captured

The 32-bit ARGB image did not reach the aperture in its own format: the
distinctive marker byte (`0xa5` in the green channel of every pixel) appears
**zero times** in the dump, and the uploaded data is shaped like a mono cursor.
Either the X server converted the ARGB cursor down before upload, or ARGB
cursors use a different memory area. The only ARGB-specific evidence is that
`0x1ad0` changed from `0x05000000` to `0x04000000`.

The ARGB upload format therefore remains **uncaptured**. Do not infer it from
the format-register change alone.
