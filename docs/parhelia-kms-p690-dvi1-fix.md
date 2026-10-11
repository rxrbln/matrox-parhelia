# P690 DRM KMS DVI-1 bring-up

## Failure

The kernel KMS path currently advertises a synthetic `VGA-1`, drives display
controller 0 from FS2, and calls only the DAC2 output helper.  It never restores
the P690 DVO board configuration after `parhelia_engine_init()` writes
`0x1c24 = 7`.  That write powers down both serializers and their common block
and replaces the upper PInS configuration bits.

The live failing state on the 0x2539 test board at 1024x768@60 was:

```
0xe38  = 0x01000000        FS2 locked
0x1800 = 0x00040201        head 0 running
0x1908 = 0x053e03fe        1344 total, 1024 active
0x1c24 = 0x00000007        DVO1, DVO2 and common all powered down
0x1c54 = 0x68000000        stale head-1/-H/-V format
0x1c70 = 0x00000000        PInS payload absent
```

The option ROM in `tools/p690.rom` has PInS at `0x76c0` and supplies:

```
PInS+0x190 = 0xaac00000
PInS+0x194 = 0x00000010
PInS+0x1a1 = 0x14
```

These are also the values used by the recovered vendor sequence in
`src/parhelia_mode.c`.

## Minimal single-pipe routing

DRM currently has one CRTC, one vblank counter, and one scanout plane.  Keep it
on head 0/FS2 and crossbar-route the physical DVI-1 serializer to head 0.  This
avoids moving gamma, page flips, and the established head-0 vblank IRQ while
fixing the wire output.

For P690, after timing, surface, and DAC shutdown but before video reset:

1. Clear `0x08f4` bit 5, the output/TMDS power gate.
2. Power down the target serializer: set `0x1c24` bit 1.
3. Gate its auxiliary clock with `0x1c04` bit 14, select FS2 with
   `0x1c04[17:15] = 3`.
4. Restore the board words: write `(0xaac00000 & ~7) | (old_1c24 & 7)` to
   `0x1c24`, then `0x10` to `0x1c70`.
5. Write `0x1c54`: head-select bit 30 clear (controller A/head 0), HSync
   inversion at bit 27, VSync inversion at bit 29, and dual-link bit 25 only
   above 165 MHz.
6. Mirror those format bits into `0x1c20[30:25]`, preserving all other bits.
7. Release `0x1c04` bit 14 and clear `0x1c24` bits 1 and 2 to power up the
   serializer and its common block.
8. Apply the head-0 video reset, then write `DC_CTRL_ON` again.  A live reset
   test showed `0x1800` bit 0 cleared after the reset pulse even though it had
   been set beforehand.

The exact live 1024x768 -HSync -VSync readback after this sequence was:

```
0x1c04 = 0x00018082
0x1c20 = 0x28002810
0x1c24 = 0xaac00001
0x1c54 = 0x28000000
0x1c70 = 0x00000010
0x1800 = 0x00040201
```

The kernel connector must consequently be created as
`DRM_MODE_CONNECTOR_DVID`, with a `DVI-1` userspace name.  Head-0 vblank remains
bit 1 (`0x2`) because the scanout timing generator remains head 0.

## Vendor-disassembly correction after the first live test

The register sequence above restored plausible DVO state but was not proven to
produce a wire-level image.  A subsequent instruction-by-instruction check of
the exported Linux vendor routines found two omissions in the earlier DDX
translation.

`HSLPAREnableDVO2` (`mtx_drv.so` 7.0, `0xd87a0`) handles an LX/P690 clock
argument as follows:

```
0x1c04[9:6]   = clock & 0xf
0x1c04[17:15] = clock & 7
0x1c04        |= 0x00030000
0x1c04        &= ~(1 << 14)
```

Thus clock argument 3 (FS2, used by head 0) produces the combined clock fields
`0x000380c0`, plus preserved mux/control bits.  On the test board that gives
approximately `0x000380c2`; the first KMS implementation left `0x1c04` at
`0x00018082`.  It omitted both the DCM selector update and the vendor's
unconditional `0x00030000` bits.

Sync polarity is programmed by a separate routine, `HSLPARSetSyncPolDVO`
(`mtx_drv.so` `0xd7e50`).  For LX/P690 it reads the format shadow at `0x1c54`,
adjusts mask `0x28000000` (bits 27 and 29), and writes the resulting polarity
word to `0x1c58`.  The mirror in `0x1c20` is in **byte 1**, mask `0x00002800`,
not byte 3.  The vendor VGA-mode call and the valid POST state both leave those
bits clear for a standard negative-HSync/negative-VSync mode.  Thus 1024x768
-HSync -VSync on head 0 uses:

```
0x1c54 = 0x00000000
0x1c58 = 0x00000000
0x1c20 = (old & ~0x00002800)       /* preserve board/global low bits */
```

The initial KMS translation put the polarity bits directly in `0x1c54`, treated
set bits as negative polarity, and mirrored them into the wrong byte of
`0x1c20`.  Keep DVO enable/head selection and sync polarity as separate
operations, matching the vendor code.

The standalone `HSLPAREnableDVO2` helper does not assert the `0x1c04` bit-14
clock hold before changing these fields; it only clears the bit after the final
write.  The generic full `HSLPAREnableDVO` path conditionally asserts the hold
during reconfiguration.  Explicitly asserting it while DVO2 is powered down,
preserving it through the field writes, and clearing it last follows the full
modeset path safely.

These corrections supersede the proposed `0x1c04=0x00018082` and direct
polarity-in-`0x1c54` values above.  A physical-monitor test is still required;
NanoKVM is not connected to this output and cannot validate it.

For a later dual-head implementation, physical DVI-1 can instead use head 1,
FS3 (`0xe3c`) and the head-1 vblank mask `0x10000000`.  That is unnecessary for
the current single-pipe driver and increases the size of the modeset change.

## 1920x1200 high-clock follow-up

The DRM driver was subsequently moved to the physical DVI-1 route used by the
working DDX: controller B/head 1, FS3 (`0x0e3c`), and vendor DVO2.  Live vblank
measurement at 1920x1080 proved that this P690 board's SUN90 FS reference is
27 MHz: the former 24 MHz model produced 67.5 Hz from a 60 Hz mode, an exact
27/24 ratio.  Both 148.5 MHz and 154 MHz are representable exactly:

```
b0=30 b1=11 b2=10 b3=3 -> 148500 kHz, word 0x00024a9d
b0=44 b1=14 b2=18 b3=3 -> 154000 kHz, word 0x00028b6b
```

The intermediate tap is 66 MHz and both VCOs are in range (1188 MHz and
924 MHz), so this mode does not need dual-link and is not an FS range-edge
case.  The kernel's bit-21 + bit-31 initialization strobe, plain-word write,
1 ms settle, and bit-24 lock poll match the hardware-tested DDX sequence.

One clock handoff was nevertheless missing from DRM.  The vendor/DDX sequence
selects the programmable synthesizer in VGA MISC bits 3:2 (`0x1fcc` read,
`0x1fc2` write, value 2) in addition to routing the head's DCM mux to FS3.
These are distinct selectors: the DCM field chooses the individual FS, while
VGA MISC chooses the programmable-FS family instead of retaining the POST VGA
clock.  DRM now performs both operations.  It also clears `DC_XPAN` and
`DC_YPAN` on every full mode set, matching `HSLPARSetDCParameters`/the working
DDX instead of carrying unspecified POST pan state into a larger timing.

Do not reverse the current DRM sync-polarity encoding as a high-clock fix.
The Linux vendor disassembly shows that LX/P690 keeps head/format in `0x1c54`,
writes polarity to `0x1c58`, and mirrors polarity in `0x1c20` byte 1.  DRM's
current split follows that result.  The earlier DDX code that placed the bits
directly in `0x1c54` predates this correction.
