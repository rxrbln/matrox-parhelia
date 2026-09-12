# Original Parhelia 8X (102b:0528) under our driver, POSTed

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Measured on a PCI-X retail Parhelia 8X in a second machine whose firmware does
POST it, unlike the box where it was first tried (see
`parhelia-cold-post-bringup-re.md` for the un-POSTed case and the cold sequence
that would be needed there).  Board: `102b:0528` rev 06, 256 MiB, two DVI-I
connectors and no VGA socket, PInS revision `0x60a`, DDC pin table
`0x05070608`, i.e. ids `{8,6,7,5}` at GPIO bits `{0,1,2,3}`.

## 1. What the driver does with it

Binds unchanged, with the right silicon profile and BAR layout:

```
Parhelia-8X: mapped 0x0000000010000000-byte VRAM BAR0 and MMIO BAR1,
tile layout 0, MC HSLPAR, clock HSLPAR, output 0, reset HSLPAR
PInS 0x60a at 0x7800: DDC pin table 0x05070608 (VBIOS)
WARP firmware staged: 9 complete resident/setup images in owned DMA heap
[drm] Initialized parhelia 1.0.0 for 0000:07:00.0 on minor 0
```

`--info` reports `chip=0x0528 tile_layout=0 texture_units=4
texture_pipe_mode=0`, so the four texture units and the 64x32/8 KiB tile
layout of this generation are both picked up, against the P690's two units and
64x16/4 KiB tiles.

## 2. Memory controller: correct

The firmware-programmed HSLPAR controller works through our driver untouched.
2-D fills and their read-back are exact:

```
PASS: GPU clear 0x5a39c7 mismatches=0/65536
PASS: GPU clear 0x204060 mismatches=0/65536
```

Zero engine lock-ups across the whole session.  No frame-buffer access was
refused, which is the difference from the un-POSTed board, where one dword
written to BAR0 resets the machine.

## 3. Tile swizzling: correct

`--swizzle-probe` and `--render` both pass on the 64x32-pixel, 8192-byte tile
order this generation uses (`p3d_tile_offset()`, layout 0), including the tile
row parity on x bit 5:

```
PASS: gouraud covered=65536 expected=65536 mismatches=0 magenta=0 cyan=0
PASS: gouraud covered=18336 expected=18336 mismatches=0 magenta=0 cyan=0
ramp y error mean by row mod 4: 0.000 -0.250 0.000 -0.250
```

A known-geometry draw therefore lands where the address model says, over a
full 256x256 surface and over a partial triangle pair.  The residual quarter-
LSB ramp bias on odd rows is interpolation, not addressing.

## 4. Gap: the transfer ioctls are P690-only

```
--transfer-probe: set up transfer probe: Operation not supported
```

`parhelia_transfer_render_ioctl()` refuses anything but `0x2539`, because the
8 KiB aperture window it copies through exists for the P690's *translated*
BAR0.  This generation's BAR0 is a linear view of VRAM, so the same ioctls can
be served by a direct copy, and until they are, every probe built on
UPLOAD_RENDER/DOWNLOAD_RENDER - the texture format, mip address, wrap and
volume batteries - cannot run on this card at all.  That is the single largest
coverage hole for this generation.

## 5. Outputs: connector types fixed, DDC still silent

The board has two DVI-I connectors.  The descriptor table declared the first
one VGA, which also suppressed its DVI-I subconnector property and forced
`edid_digital` false before any EDID read; both are DVI-I now, and the second
one is exposed rather than hidden, so its DDC bus is registered:

```
i2c-1: Parhelia DDC line 0 (GPIO 3/1)
i2c-2: Parhelia DDC line 1 (GPIO 2/0)
```

Those pin pairs are what the vendor resolver produces for this board's table
(`(5,6)` for line 1 and `(7,8)` for line 2, ids resolved through the byte
index), and they match the saved original-Parhelia ROM row of
`parhelia-ddc-complete.md` §2.  Even so, with a monitor attached through a
DVI-I to VGA adapter:

* both connectors read `disconnected`, no EDID;
* a direct `I2C_RDWR` to address `0x50` on `/dev/i2c-1` and `/dev/i2c-2`
  returns `ENXIO`, so no device acknowledges on either line;
* analog load sensing on DAC2 reports nothing either, which is why
  `parhelia_connector_get_modes()` returns no modes at all - without EDID it
  needs `analog_sensed` before it will offer the no-EDID mode list.

So this is not the mode list or the connector type: nothing answers on the
bus.  What has not been tested yet, in the order worth trying:

1. Whether `0x1c48` responds at all on this chip - drive each OE bit and read
   the input bit back.  Everything above assumes the register works here
   because it is documented as common to every Parhelia variant.
2. The second GPIO bank in bits 8..15 of the same register, which
   `parhelia-ddc-complete.md` §1 calls the likely home of a secondary DDC
   channel on boards that populate it, and which no hardware has exercised.
3. Whether a DVI-I connector gates its DDC buffer behind output power on this
   board, unlike the integrated boards the register map came from.

Note that DDC lines 3 and 4 of the vendor resolver need not be tried: for this
board's table they resolve to the same two pin pairs as lines 1 and 2.

## 6. Gap: only one output can be driven

`parhelia_program_output()` and `parhelia_kms_head()` both take
`pdev->outputs[0]` unconditionally, so whichever output is first in the table
is the only one the single display pipe can drive, and the second connector is
exposed for DDC alone (`ready = false`, no encoder attached).  Driving the
other connector needs the programmed output to follow the atomic state's
encoder instead of the array index.  That is a prerequisite for using this
board's second DVI-I at all, and for dual-head later.
