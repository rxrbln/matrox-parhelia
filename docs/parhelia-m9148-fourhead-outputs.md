# M9148 outputs 0..3: DDC pins and modeset registers, measured

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0-only

This note closes the per-output display contract for the **Matrox M9148 LP**
(`102b:0540`, subsystem `102b:2240`) by snooping the vendor stack on the board
itself, with four displays attached — one per mini-DisplayPort connector.

Evidence tags:

* **MEASURED** — read or observed on this board this session.
* **CAPTURED** — present verbatim in a stored vendor trace or in the board ROM.
* **DERIVED** — inferred from measured/captured facts, not independently shown.
* **UNTESTED** — stated but not exercised.

Primary artifacts: `captures/m9x-vendor-fourhead-20260917/` (this session) and
`captures/m9x-cold-init-20260915-v1/xinit-mmio*.jsonl` (the earlier vendor-X
write trace).

## The board has four outputs, not five

`lspci` reports subsystem `102b:2240` = *M9148 LP PCIe x16*; the vendor X driver
enumerates exactly `mon0..mon3` and the board carries four mini-DP connectors.
**There is no output 4.**  "Outputs 2, 3 and 4" therefore means, at most, the
three connectors after the first: 0-based heads **1, 2 and 3**.  MEASURED.

## Instrumentation, and what does not work

* The vendor kernel module for this board is **`m9x.ko`** (char major 252), not
  `mtx.ko` — `mtx.ko`'s strings are the Parhelia ones and it does not serve
  `0x0540`.  MEASURED.
* **`mmiotrace` is unusable here**: the `2.6.30-dist` kernel is built with
  `# CONFIG_MMIOTRACE is not set`.  Even with it, `docs/parhelia-mmio-capture.md`
  already records that the vendor X driver maps the BARs from userspace, so a
  kernel `ioremap` tracer sees nothing.  MEASURED.
* What does work, and was used: read-only full-register snapshots
  (`tools/m9x-mmio-snapshot.c`), a read-only ordered sampler of the display
  register ranges (`tools/m9-display-watch.c`, new), the stored gdb-observer
  write trace from 2026-09-15, the board ROM, and direct HMC-arbitrated I2C
  reads (`tools/m9-dp-io.c`, new).

## Board PInS, read from this board's ROM

PInS 8.0 lives at ROM `0x7460` (u16 pointer at `0x7ffc`, signature `0x412e`).
CAPTURED:

```
+0x16  "KFQ19421"                     board serial
+0x30  "M9148-E1024LAF"               board name
+0x44  00014444                       1 GiB
+0x50  09 0a 0b 0c 0d 0e 0f 10        DDC GPIO map   -> MMIO 0x1c48
+0x6c  01 02 00 20 21 17 22 23        MISCIO GPIO map-> MMIO 0x1c4c
+0x5c  42001100 42001200 42001300 42001400   connector 0..3 routes
```

The brief's `09 0a 00 00 0d 0e 00 00` for `+0x50` is **wrong for this board**;
all eight physical bits carry a function ID, giving four complete DDC buses.

Connector route byte `0x11 + n` is the direct link code for link index `n`, so
**connector n uses link n**.  CAPTURED.

## DDC: one bit-banged I2C bus per connector on 0x1c48

MMIO `0x1c48`, data bit `n`, output enable bit `n+16`, open drain.  MEASURED and
CAPTURED (four independent bit pairs decode as clean I2C in the vendor trace,
and all four answer live):

| output | clock bit | data bit | OE bits | PInS function IDs |
|---|---|---|---|---|
| 0 | 0 | 1 | 16 / 17 | `09` / `0a` |
| 1 | 2 | 3 | 18 / 19 | `0b` / `0c` |
| 2 | 4 | 5 | 20 / 21 | `0d` / `0e` |
| 3 | 6 | 7 | 22 / 23 | `0f` / `10` |

The clock is the **even** bit of the pair and the data the odd one.  MEASURED.

Proof for the outputs this task is about: reading slave `0xa0` on bit pair
`(4,5)` returns the EDID of the sink the vendor driver calls **mon2**
(VCS, product `0x1145`, monitor name `Connector`), and on bit pair `(6,7)` the
EDID of **mon3** (GSM `0x5bf5`, `LG SDQHD`) — byte-identical to the EDIDs the
vendor X log prints for those outputs, reproduced across two boots.  MEASURED.
Outputs 0 and 1 did not answer `0xa0` in the firmware state because their
transmitter I2C bridges were left disabled; their pin pairs are DERIVED from the
PInS map plus the four identical per-bus transmitter initialisations in the
vendor trace.

Each connector has its **own external DisplayPort transmitter** on its own bus.
On this board the transmitter answers at 8-bit addresses `0x10` (main),
`0x12` and `0x14` — **not** the `0x72`/`0x76` recorded in
`parhelia-m9148-kms-pins-dp-re.md` from the saved binaries.  MEASURED.
The register fingerprint is nevertheless the documented one; the vendor's
per-connector sequence, CAPTURED verbatim from `xinit-mmio-part2.jsonl`, is:

```
S 10 00 / S 11 ..      read main reg 0
S 14 00 / S 15 ..      read page reg 0
S 12 00 / S 13 ..
S 12 7c / S 13 ..      HPD
S 10 74 20 ; S 10 74 a0 ; S 10 0a 10        blank
S 10 00 22 74 00 78 30 72 a9 27 30 30 32
     71 20 2f 82 86 f0 8b 07 02 b6 77 a3    common init
S 10 0a 00 24 01 20 21 76 ff 78 33          main pre
S 14 24 10 / 25 04 / 26 04                  page-3 init
```

### The fifth I2C bus is not a DDC bus

`0x1c4c` bit 1 = clock, bit 0 = data, OE at `+10` (PInS MISCIO IDs `01`/`02`)
carries a single 256-byte device at `0xa0`.  The vendor reads all 256 bytes of
it during driver init; reading it directly returns the **board identification
EEPROM** (`d3 25 08 00 …` then ASCII `KFQ19421`, the PInS serial).  It is not a
monitor EDID.  MEASURED.

The remaining MISCIO functions on this board are the four **transmitter reset**
lines: IDs `0x20`/`0x21`/`0x22`/`0x23` (= `0x20 + connector`) at data bits
3, 4, 6 and 7, output enables 13, 14, 16 and 17.  CAPTURED from PInS and
confirmed by exactly those bits toggling in the vendor trace.

### HMC ownership, corrected

`0x0318` request bits are active low, grant polled in `0x03b0`:

| bus | request bit in `0x0318` | grant bit in `0x03b0` |
|---|---:|---:|
| `0x1c48` (DDC) | 29 | 1 |
| `0x1c4c` (MISCIO) | 28 | 0 |

CAPTURED: every `0x1c48` run in the vendor trace is bracketed by
`0x318 = 0x10000003` / `0x30000003` and every `0x1c4c` run by
`0x60000003` / `0x70000003`.  `docs/parhelia-m9.md` has these two swapped.

## Modeset register map for the four heads

All four heads were programmed by the vendor X server in one run and all four
rasters were then observed advancing.  Bases, MEASURED — each block carries its
own head's mode, identified by matching the programmed timing and pixel clock
against that output's EDID mode:

| head | DC control | DC timing | clock select | watermark | link | cursor |
|---|---|---|---|---|---|---|
| 0 | `0x1800` | `0x1900` | `0x1c00` | `0x1c14` | `0x1c9c` | `0x18c0` |
| 1 | `0x1a00` | `0x1b00` | `0x1c08` | `0x1c1c` | `0x1ca0` | `0x1ac0` |
| 2 | `0x1980` | `0x1e00` | `0x1c04` | `0x1c18` | `0x1ca4` | `0x18e0` |
| 3 | `0x1b80` | `0x1f00` | `0x1c0c` | `0x1c20` | `0x1ca8` | `0x1ae0` |

**The fixed-stride hypothesis does not hold.**  Neither the DC bases
(`0x1800`, `0x1a00`, `0x1980`, `0x1b80`) nor the timing bases (`0x1900`,
`0x1b00`, `0x1e00`, `0x1f00`) nor the clock-select registers are a uniform
stride, and the naive continuation `0x1c00`/`0x1d00` for head 2 would land on
the clock-select and output-routing block.  Heads 2 and 3 occupy the upper half
of heads 0's and 1's timing pages (`0x1980` = `0x1900+0x80`, `0x1b80` =
`0x1b00+0x80`), which is why their DC blocks are only `0x80` long and have no
`+0x88`/`+0x90` hold pair of their own.

Sub-offsets are the same as heads 0 and 1 (`docs/parhelia-m9.md` "Display
controller"), with these measured differences for heads 2 and 3:

* `DC+0x10` is written (same value as `DC+0x0c`) and `DC+0x64 = 0x80010000`;
  neither is written for heads 0 and 1.  MEASURED, meaning not established.
* heads 2 and 3 have no `+0x88`/`+0x90` hold registers.  Instead the vendor
  writes **`0x1888` (DC0 hold) inside the head-2 sequence and `0x1a88`
  (DC1 hold) inside the head-3 sequence**, both `0x00000300`.  So head 2 is
  paired with head 0 and head 3 with head 1 for the hold protocol.  MEASURED.
* `0x190c` is written in the head-2 sequence and `0x1b0c` in the head-3
  sequence, both `(width-1)<<16` of that head's mode.  MEASURED.

Controllers 2 and 3 are **not** secondary planes of heads 0 and 1: each has its
own timing block, its own synthesiser, its own link and its own advancing line
counter.  This contradicts the `[M9WIN]` reading in `docs/parhelia-m9.md`.

### Pixel clocks: selected, not fixed per head

There is no per-head pixel-clock register.  The controller's clock-select word
picks one synthesiser:

```
select   = (clksel >> 1) & 0xf        ; bit 0 = hold
register = 0x0e38 + 4 * select
```

MEASURED, four independent confirmations in one run — the divider word in each
selected register decodes, with the published
`Fout = 27000 * b0 * b1 / b2 / (2*b3)`, to exactly that head's mode clock:

| head | clksel reg | value | select | synthesiser | decoded | mode |
|---|---|---|---|---:|---|---|
| 0 | `0x1c00` | `0x02000008` | 4 | `0x0e48` = `0x0104d6f6` | 154000 kHz | 1920x1200 |
| 1 | `0x1c08` | `0x02000002` | 1 | `0x0e3c` = `0x0102e739` | 261000 kHz | 1792x1344@75 |
| 2 | `0x1c04` | `0x00000004` | 2 | `0x0e40` = `0x0104d6b6` | 148500 kHz | 1920x1080 |
| 3 | `0x1c0c` | `0x00000006` | 3 | `0x0e44` = `0x010375df` | 172800 kHz | 1920x1080 |

`0x0e4c` and `0x0e50` hold `0x000fb370` / `0x000f5b1e` throughout and are
**not** pixel clocks; the brief's "head 1 clock `0x0e4c`" is refuted.  MEASURED.
`0x0e30/34/38` are the system clocks and all three carry the same word.

Bit 25 (`0x02000000`) is set in the clock-select words of heads 0 and 1 and
clear for heads 2 and 3.  MEASURED, meaning not established.

### Links

`0x1c9c + 4*link`, link `n` on connector `n`.  MEASURED final values:

```
0x1c9c = 0x0040a800   source 0x00 = controller 0
0x1ca0 = 0x0010a808   source 0x08 = controller 1
0x1ca4 = 0x0000a801   source 0x01 = controller 2
0x1ca8 = 0x0010a809   source 0x09 = controller 3
```

This confirms the `0x00 / 0x08 / 0x01 / 0x09` controller encoding in bits[4:0]
on live silicon, and that bit 18 (path off, `0x00040000`) is cleared to enable.
The base `0xa800` is the PInS register-patch value `0x0004a800` with bit 18
cleared.

Bit 20 is set for the two modes with negative hsync and clear for the two with
positive hsync — MEASURED, rule holds 4/4.  Bit 22 is set for head 0's negative
vsync but **clear for head 2's, which is also a negative-vsync mode**; the
vsync-inversion rule is therefore **not** established.  Since DisplayPort
carries sync as packets, this may not matter downstream.  UNTESTED.

Companion halves, MEASURED: `0x1c38 = 0x11140114` holds links 0 (low half) and
1 (high half); `0x1c3c = 0x31142114` holds links 2 and 3.  Each half is
`(link << 12) | 0x114` on this board, written inside that link's own sequence.

`0x1cac` walks `0xff -> 0xf0 -> 0x05 -> 0x07 -> 0x0f` as the four links come up,
one bit appearing to gate each pad group; the exact bit assignment is not
established.  MEASURED values, DERIVED meaning.

### Order of the vendor's four-head bring-up

From `captures/m9x-vendor-fourhead-20260917/watch-disp.csv`, MEASURED:

1. tear down head 0's firmware mode: clear `dct0`, `clksel0`, `0x1c10`, DACs,
   `dvo0`, `0x1cac = 0xff`, power down the oscillator and every synthesiser
   (`0x0e24/30/34/38/48/4c/50 = 0x80000000`), clear `dc0`;
2. restore `0x0e4c`/`0x0e50`, `0x1cac = 0xf0`, `0x1c28 = 0x0f021084`;
3. put all four controllers on hold (`0x1c00/0x1c08 = 0x02000001`,
   `0x1c04/0x1c0c = 0x00000001`), `0x1c10 = 2`, `0x1c24 = 0x101`,
   DACs `0x01001400`, all four links `0x00040000` (path off);
4. oscillator `0x70000000`, system clocks `0x0e30/34/38`;
5. then, per head in the order **0, 1, 2, 3**: DC geometry and scanout start,
   that head's synthesiser word, its `clksel` (hold released, select set), its
   watermark, `DC ctrl = 0x33040001`, `DC pitch`, the seven timing writes,
   the `0x1c38`/`0x1c3c` companion half, and finally its link word.

The sampler polls, so a value overwritten between two passes can be missed; the
snapshots are authoritative for final state and the trace for order.

## The PInS must come from the shadow BIOS, not the option ROM BAR

The clean-room kernel driver read `09 0a 00 00 0d 0e 00 00` at PInS `+0x50` and
built two DDC buses, while reading this board's ROM at the same PInS base
`0x7460` gives `09 0a 0b 0c 0d 0e 0f 10`.  The two reads differ in more than
those four bytes: the driver also reported 644 patch-table bytes where the
captured image has 596, so the images themselves are different, not the read.

`captures/m9x-vendor-fourhead-20260917/vbios-shadow.bin` is the copy POST left
at `0xc0000`.  Its expansion-ROM image is `0xf000` bytes, PCIR `102b:0540`,
last-image bit set, and both the image checksum and the PInS's own 0x400-byte
checksum are zero, so the four-bus map is the board's own and internally
consistent.  MEASURED.  (The `55 aa` image at `0xf000` in that dump is the next
option ROM the system BIOS packed into the C segment, PCIR `10ec:8168`, and not
part of the Matrox ROM.)

The vendor X driver does not read the ROM BAR at all.  `m9x_drv.so` calls
`mmap(NULL, 0x20000, …, fd, 0xc0000)` and aborts with *"Cannot map Shadow
BIOS"* / *"M-Series Shadow BIOS detected"* / *"BIOS was not run?"* — it takes
the PInS only from the POSTed RAM copy.  CAPTURED (disassembly at
`0x288f4d`).

So the flash image is generic and the VBIOS specialises the shadow for the
board it finds; the flash PInS carries the two-bus map of the smaller M9
boards.  A driver that reads the ROM BAR silently caps itself at two heads.
DERIVED — the flash image itself was not dumped, and the alternative that some
component patched the RAM copy after POST is not excluded; the driver now logs
which source it used along with the PInS board name and serial, which settles
it in one boot.  If both sources ever disagree on a board, the identification
EEPROM at `0xa0` on the MISCIO bus carries the same serial as PInS `+0x16`
(`KFQ19421` here) and can pick the right image.

## What a driver change needs

`drivers/gpu/drm/m9x` currently brings up head 0 only.  To extend it:

1. replace the single-head register constants with the table above, indexed by
   head, and keep the DC block length at `0x80` for heads 2 and 3 so the hold
   pair of head 0 / head 1 is used for them;
2. drive the pixel clock through the select rule rather than a per-head
   register: choose a free synthesiser, program `0x0e38 + 4*select`, then write
   `clksel = (select << 1)` with bit 0 cleared to release the hold;
3. build the DDC buses from PInS `+0x50`/`+0x54` rather than from the classic
   `+0x1b4` pin table, with the even ID of each pair as clock; take
   `0x0318` bit 29 / `0x03b0` bit 1 for `0x1c48` and bit 28 / bit 0 for
   `0x1c4c`, and correct the swapped assignment in `docs/parhelia-m9.md`;
4. take the transmitter slave address from the board rather than from the
   `0x72`/`0x76` constant — this board answers at `0x10`/`0x12`/`0x14`;
5. per-head vblank IRQ sources for heads 1..3 remain unknown; the per-head line
   counter at `DCT+0x24` is a usable fallback and is confirmed to advance on all
   four heads.  UNTESTED as an IRQ source.

All five were landed in `drivers/gpu/drm/m9x` together with the shadow-BIOS
PInS source above; the per-head vblank sources for heads 1..3 are still unknown
and default to no interrupt.

## All four rasters run under the KMS driver

MEASURED 2026-09-17 on `7.2.4-t2` with `m9x_drm` built from this tree
(`m9_kms=1 m9_kms_modeset=1 m9_kms_scanout=1`), setting 1024x768 on one CRTC at
a time and sampling `DCT+0x24` on all four heads:

```
probe                1924=0000037a 1b24=00000000 1e24=00000000 1f24=00000000
after CRTC 1 enabled 1924=00000422 1b24=0000018f 1e24=00000000 1f24=00000000
after CRTC 2 enabled 1924=0000049c 1b24=000000ea 1e24=00000283 1f24=00000000
after CRTC 3 enabled 1924=0000003f 1b24=00000046 1e24=000001df 1f24=000001e9
```

Each head's counter starts advancing as its CRTC is enabled and keeps advancing
after that CRTC is disabled again, since only `DC+0x00` bit 0 is cleared and the
timing block keeps running.  The heads that "did not raster" in the earlier
report had simply never had a CRTC enabled: the driver programs timing from
`m9_pipe_enable`, and the probe-time line-counter log is taken before that.  No
change to the write order was needed, and none of the global preamble words
(`0x1c28`, `0x1c24` bit 8, the DACs, the `0x1fcc/0x1fd4/0x1fdc` VGA words) is
required for a head to raster.

Still at their firmware values after four heads are up, and still unproven as
picture requirements: `0x1cac = 0xe1` (vendor ends at `0x0f`), `0x1c28 = 0`
(vendor `0x0f021084`), `0x1c80/0x1c84 = 0x03001400` (vendor `0x01001400`).  The
`m9x.pad_gate=` parameter writes `0x1cac` for the first of these.

## EDID: two sinks answer the pins, two need the AUX engine

MEASURED 2026-09-17, four displays attached, transmitter main register 0 read
per bus:

| bus | main reg 0 | direct `0xa0` read | native AUX (DPCD 0) |
|---|---|---|---|
| 0 | `0x00` | address NAK | times out |
| 1 | `0x08` | 13 bytes of zeros, then NAK | times out |
| 2 | `0x20` | full EDID, VCS "Connector" | times out |
| 3 | `0x24` | full EDID, LG SDQHD | `12 14 c4 81 01 00 01 80 …` |

So connectors 2 and 3 carry sinks reachable on the DDC pins and the driver reads
them repeatably; connector 3 additionally answers native AUX.  Connectors 0 and
1 answer neither in the firmware state, although both EDIDs *were* read once
each on 2026-09-17 - connector 1 by the two-head build after the transmitter had
been poked by hand, connector 0 by this build's bridged retry immediately after
a vendor-resolution X session had been running on it.  Neither is reproducible
from a cold transmitter.

The transmitter AUX engine itself is confirmed working exactly as
`parhelia-m9148-kms-pins-dp-re.md` "AUX native transactions" describes
(`0x7d..0x84` on the main slave, completion in `0x76` bit 5): it returns a valid
receiver-capability block on bus 3.  On buses 0, 1 and 2 it never asserts
completion, including after pulsing that connector's MISCIO transmitter-reset
line and applying the vendor's common / main-pre / page-3 tables verbatim
(`tools/m9-dp-io.c` `aux`, `auxedid`, `dump`, `block` subcommands).  Reading
EDID on connectors 0 and 1 therefore needs the full transmitter bring-up -
reset, detect, initialize, train - not a bridge toggle.  UNTESTED as a remedy.

Two driver defects were found and fixed while measuring this:

* the hotplug byte was read from the transmitter's **main** address, where it is
  meaningless; the vendor reads it from the next seven-bit address.  All four
  buses logged `hotplug 0x00`; they now log `0x13 0x07 0x07 0x07`.
* the EDID path only disabled the transmitter's I2C bridge when it found bit 1
  of main register 0 already set, which no connector on this board ever is, so
  the documented lifecycle never ran.  The bridge is now cleared for the read,
  the read is repeated through the bridge when the pins give nothing, and the
  register is restored.

## Lighting several heads at once (MEASURED, 2026-09-17)

All four connectors drive a panel simultaneously.  Measured with all four
attached, each head programmed independently:

| connector | head | mode | pixel clock | link register |
|---|---:|---|---:|---|
| DP-1 | 0 | 1920x1200@60 | 154000 kHz | `0x1c9c = 0x0040a800` |
| DP-2 | 1 | 1024x768@60 | 65000 kHz | `0x1ca0 = 0x0000a808` |
| DP-3 | 2 | 1920x1080@60 | 148500 kHz | `0x1ca4 = 0x0040a801` |
| DP-4 | 3 | 1024x768@60 | 65000 kHz | `0x1ca8 = 0x0000a809` |

`modetest` cannot do this: given several `-s` options it allocates **one**
framebuffer spanning all outputs, and the m9x pipe refuses it.  The pipe is a
`drm_simple_display_pipe`, whose plane always shows the whole framebuffer, and
`m9_kms.c` requires `fb->width == mode.hdisplay && fb->height ==
mode.vdisplay`; `mode_config.max_width` and `max_height` are both 2048, so a
wider screen is rejected before that.  `tools/m9-heads.c` gives each head its
own dumb buffer from one DRM master instead, and paints a pattern that names
the connector and encodes each pixel's coordinates.

The same limit shapes what X can do today.  A `modesetting` screen wider than
one head cannot be scanned out, so the four heads run **cloned**:
`tools/xorg-m9x-clone.conf` pins the virtual size to a mode every panel carries
(1024x768), which keeps all outputs at `+0+0`, and `tools/m9x-clone-x.sh`
starts the server and enables them.  Measured: four monitors in
`xrandr --listmonitors`, `OpenGL renderer string: Matrox M9 (clean-room
bring-up)`, direct rendering, glxgears 663 fps.

An extended desktop needs the pipe replaced by a full `drm_crtc`/`drm_plane`
pair so each head can scan out a window of a larger shared framebuffer at its
own offset - the hardware side is only a per-head `M9_DC_START` of
`base + y * pitch + x * 4` with the shared pitch in `M9_DC_PITCH`, whose mask
holds 14 bits of pixels.  NOT IMPLEMENTED.

DP-3 read `disconnected` on one probe immediately after DP-2 was attached, then
`connected` with valid EDID on the next X start without anything being
replugged.  One transient, not reproduced, cause not established.

## Do all four heads sweep, and does vblank reach the interrupt path?

Two separate questions, answered by two independent paths with
`tools/m9-scanout-probe.c`: the free-running line counter in each head's
timing bank (`DCT + 0x24`) says whether the display controller is sweeping,
and `drmCrtcGetSequence` says whether that head's vertical blank reaches the
kernel's vblank counter.  MEASURED 2026-09-17 with DP-1, DP-3 and DP-4 held by
`tools/m9-heads.c` (DP-2 was disconnected for this run):

| head | line counter | range | wraps | rate | vtotal | source of the timing |
|---:|---|---|---:|---:|---:|---|
| 0 | `0x1924` | 0..1234 | 60/s | 60.00 Hz | 1234 | DP-1 1920x1200, ours |
| 1 | `0x1b24` | 0..805 | 60/s | 60.00 Hz | 805 | firmware's posted 1024x768 |
| 2 | `0x1e24` | 0..1124 | 60/s | 60.00 Hz | 1124 | DP-3 1920x1080, ours |
| 3 | `0x1f24` | 0..805 | 60/s | 60.00 Hz | 805 | DP-4 1024x768, ours |

**All four sweep**, each at exactly the rate and vtotal of its own timing.
Head 1 was not programmed in this run at all: it is still running the
timing the firmware posted, whose `vblank 0x032502ff` is the same word the
warm snapshot in `m9_kms.c` recognises.  Sampled again after the modes were
dropped, **all four still advance at 60 Hz** - releasing the framebuffer does
not stop a timing generator.

Vblank interrupts are a different story:

```
crtc 37 (index 0)  sequence 314 -> 374  +60 in 1.00s  60.00 Hz
crtc 44 (index 1)  no vblank: Invalid argument
crtc 51 (index 2)  no vblank: Invalid argument
crtc 58 (index 3)  no vblank: Invalid argument
```

Only head 0 has one, and it is exact.  Heads 1..3 report none **by
construction**, not by failure: `m9_vblank_mask[]` carries only
`M9_IRQ_LOW_VBLANK0`, so those heads stay out of the interrupt path rather
than being given a guessed bit.

The interrupt status word gives the next step and its hazard.  Sampled
read-only (never acknowledged, so the running driver is undisturbed):

```
irq low raw 0x08e0: ever high 0xfc03ff6f  changing 0x00000000  mask 0x00000000
irq low raw 0x08e0: ever high 0xfc03ff6f  changing 0x00000001  mask 0x00000001
```

The word is W1C and the handler consumes only `raw & mask & owned`, so an
unmasked source stays latched high forever and a masked one never toggles.
**Polling cannot identify the remaining bits** - only bit 0 changes, and only
while the driver has it unmasked.  Twenty-one further bits are latched high
(`0xfc03ff6f` less bit 0) and any of them could be a head's vertical blank, as
could a bit of the high interrupt word reached through
`M9_IRQ_HIGH_LOW_CASCADE`; that the low word carries heads 1..3 at all is an
assumption, not a measurement.

### All four sources, found by frequency (MEASURED 2026-09-17)

Walking the mask was not needed.  The status word latches a source whether or
not it is unmasked, and it is write-1-to-clear, so `tools/m9-irq-scan`
acknowledges whatever is set and times how long each bit takes to come back -
no mask change, no interrupt enabled, no kernel change.  With every head on a
different refresh rate, each source's frequency names its head.  Two runs,
with the rates permuted so that no bit keeps its rate by coincidence:

| bit | run A | run B | head |
|---:|---:|---:|---:|
| 0 | 45.50 | 36.75 | 0 (raced with the driver's own acknowledge) |
| 1 | 60.00 | 60.00 | 1 |
| 2 | 75.00 | 59.75 | 0 |
| 3 | 60.00 | 60.00 | 1 |
| 8 | 50.00 | 24.00 | 2 |
| 9 | 30.00 | 50.00 | 3 |
| 10 | 50.00 | 24.00 | 2 |
| 11 | 30.00 | 50.00 | 3 |
| 12 | 75.00 | 60.00 | 0 |
| 13 | 60.00 | 60.00 | 1 |
| 14 | 50.00 | 24.00 | 2 |
| 15 | 30.00 | 50.00 | 3 |
| 16 | 10.00 | 10.00 | unrelated |
| 17 | 10.00 | 9.75 | unrelated |

Head rates were 75 / 60 / 50 / 30 in run A and 60 / 60 / 24 / 50 in run B;
head 1 stays at 60 in both because DP-2 was disconnected and it runs the
firmware's posted timing.  **Every head raises three interrupts per frame**,
grouped (0,1)+(8,9), (2,3)+(10,11), (12,13)+(14,15) across the head pairs.
Bit 0's measured rate is low in both runs because the running driver
acknowledges it too, which is itself a check that bit 0 is the driver's.

The lowest group is taken as the vblank source, since it contains the
documented head 0 bit: `M9_IRQ_LOW_VBLANK0..3 = BIT(0), BIT(1), BIT(8),
BIT(9)`.  Which of a head's three is the vertical blank edge proper is NOT
established.

### One head was masking the others

Delivery has to be measured by queueing real vblank events.  The DRM sequence
counter is extrapolated from the mode's frame duration while the source is
masked, so it reports a plausible rate for a head that is receiving nothing -
it reported 60.00 Hz for a head whose true delivery was 43 Hz.

With the bits set, delivery was still short on three heads.  The cause was in
the driver: `m9_kms_irq_desc()` filled the descriptor's `vblank_mask` with the
**union** of every head's bit, and that field is exactly what
`matrox_kms_enable_vblank` ORs into the mask register and what disable clears.
One head's `vblank_put` therefore masked every other head's source.  Enable
and disable now act on one head's bit, while the handler keeps the union,
which is what it must consume.

Measured over 2 s with heads at 75 / 60 / 50 / 30 Hz, counting queued events:

| crtc | head | union mask | per-head mask |
|---:|---:|---:|---:|
| 37 | 0 | 75.19 Hz | **75.50 Hz** |
| 44 | 1 | 43.36 Hz | **60.29 Hz** |
| 51 | 2 | 39.17 Hz | **50.20 Hz** |
| 58 | 3 | 24.05 Hz | **30.34 Hz** |

All four now deliver at their own refresh rate.  Head 1 delivers although
DP-2 is disconnected and no mode was set on it: its timing generator is still
running what the firmware posted, and the interrupt follows the generator, not
the CRTC.

## Page flipping on the vertical blank (MEASURED 2026-09-17)

With every head owning a source, `drmModePageFlip` retires on that head's own
vertical blank on every active head.  `tools/m9-heads -p SECS` flips each head
between two buffers and counts completions.

Vblank is tied to the active scanouts by the mask register alone: a head's bit
is unmasked only between `drm_crtc_vblank_on` at pipe enable and `vblank_off`
at pipe disable.  With three connectors attached and one unplugged, delivery
was 60.56 / 60.15 / 60.20 Hz on the three and 0 on the fourth, whose line
counter also read STATIC.

**A dumb-buffer flip is not an address change.**  Direct scanout needs a BO
allocated tiled (`PARHELIA_M9_BO_TILED`), which only the driver-specific create
sets; `m9_dumb_create` never does.  So every dumb flip runs
`m9_scanout_publish`, a whole-frame de-tiling copy into the head's single
scanout surface, and the start register never moves.  The cost is exactly
linear in pixels - one head, four modes, five-second windows:

| mode | pixels | flips/s | pixels/s |
|---|---:|---:|---:|
| 640x480 | 307200 | 1.34 | 412k |
| 800x600 | 480000 | 0.99 | 475k |
| 1024x768 | 786432 | 0.58 | 456k |
| 1920x1200 | 2304000 | 0.19 | 438k |

**~440k pixels/s, about 1.75 MB/s.**  So a dumb-buffer flip cannot reach the
refresh rate at any useful size, and the number to improve is that copy, not
the interrupt.

Two things this measurement caught that event counting alone would have hidden:

* An earlier run reported 361 and 360 flips in six seconds at 1920x1200 and
  1920x1080 - a clean 60 Hz - with the scanout address never moving and no
  copy that could have kept up.  `m9_program_scanout` returns silently when
  `m9_scanout_publish` fails, so the flip completed on the vertical blank and
  presented the previous frame.  A failed publish should be reported.  NOT
  FIXED.
* X here does not page flip at all.  `PageFlip: enabled` in the log is the
  option being accepted, not a flip happening: glxgears measures 527 fps with
  `vblank_mode=0` and 525 with `vblank_mode=1`, on one head and on three, so
  swaps are not synchronised to anything.  The server initialises **DRI2**
  (`(II) modeset(0): [DRI2] DRI driver: m9x`), where glamor swaps by copying;
  Present-based flipping needs DRI3.  `tools/xorg-m9x-dri3.conf` exists and is
  UNTESTED against this.

## The pad gate at 0x1cac is status, not a gate (MEASURED 2026-09-17)

Read with no driver loaded, on a freshly POSTed board:

```
0x1cac = 0x000000e1      pad gate
0x1c9c = 0x0000a800      head 0 link  - programmed
0x1ca0 = 0x00000000      head 1 link  - zero
0x1ca4 = 0x00000000      head 2 link  - zero
0x1ca8 = 0x00000000      head 3 link  - zero
0x1c38 = 0x111d011d      links 0,1 format halves
0x1c3c = 0x30002000      links 2,3 format halves
```

POST brings up exactly one link, and the pad gate's low nibble has exactly one
bit.  Reproducible across reboots.

**The register does not accept writes.**  Walking it from userspace through
BAR2, every value reads back as `0x00000001`:

| written | reads back |
|---|---|
| 0x0f | 0x00000001 |
| 0xff | 0x00000001 |
| 0x0e | 0x00000001 |
| 0x02 | 0x00000001 |
| 0xe1 | 0x00000001 |
| 0xf0 | 0x00000001 |
| 0x05 | 0x00000001 |
| 0x07 | 0x00000001 |

The first write moves it from `0xe1` to `0x01` and it never moves again, not
even back to its own POST value; a reboot restores `0xe1`.  So
`m9x.pad_gate=` cannot enable anything, and the earlier note that the vendor
"walks" this register was reading a **progression, not a sequence of writes**:
the vendor's own sequence in `parhelia-m9.md` writes `0x1cac |= 0xf0`, the
high nibble only.

That gives a consistent DERIVED reading, which is **not established**: the high
nibble is a per-pad enable *request*, the low nibble per-pad *up* status, and a
request bit clears when its pad comes up.  POST's `0xe1` is then pad 0 up with
pads 1, 2 and 3 requested and never up; the vendor's final `0x0f` is all four
up with no request outstanding.  It agrees with what reaches the panels - one
output of four - and it says the hardware was asked for four pads and three did
not come up.

The consequence for the dark heads: the missing enablement is **not** a
register bit in the display controller.  `parhelia-m9.md` names the likely
place - the mini-DP connectors are driven by an external DisplayPort 1.1
transmitter on a bit-banged I2C bus, trained by the VBIOS at POST, and nothing
writable in the GPU register file brings that link up.  DP-1 works because POST
trained it.

## Why only one connector shows a picture: the pad gate (MEASURED 2026-09-17)

Everything upstream of the connector was healthy on all four heads - EDID,
pixel clock, raster timing, line counter, vertical blank interrupt - and three
connectors were still dark.  The cause is `0x1cac`, and it needed a probe that
reads what leaves the chip before it could be seen.

### The probe: the transmitter's main-stream attributes

The external transmitter's main-page registers `0x10..0x1f` are eight
little-endian 16-bit fields - htotal, hstart, hactive, vtotal, vstart,
vactive, hsync width, vsync width - with the sync polarity in bit 15 of the
two widths.  **Nothing writes them.**  CAPTURED: every bit-banged I2C
transaction on `0x1c48` in `captures/m9x-cold-init-20260915-v1/
xinit-mmio-part2.jsonl` was decoded, and a whole vendor X run contains no
write anywhere in that range.  They report the video the display controller is
feeding that transmitter.

Calibrated three ways, MEASURED:

* connector 0 reported `1344/1024/806/768/136/6` while head 0 ran 1024x768, and
  `2200/1920/1125/1080/44/5` after X moved head 0 to 1920x1080;
* blanking connector 0's transmitter (`0x0a = 0x10`) dropped every field to
  zero and unblanking it (`0x0a = 0x00`) brought the mode straight back;
* connectors 1, 2 and 3 reported the 640x480 power-on default
  (`800/640/525/480/96/2`) while their heads were rastering something else.

Two further indicators move with it: main `0x79` reads `0x01` while the
transmitter is not driving and `0x00` while it is, and bit 2 of the hot-plug
byte on the second slave address sets while it is not driving.

**This is necessary, not sufficient.**  A connector whose transmitter reports
the right attributes is being fed the right video and is sending it; whether
the sink locks is a separate question that only link training and the sink's
own DPCD answer.  Connector 2 was brought to this state and the panel still
showed nothing.

### `0x1cac` is a per-pad gate

MEASURED.  The high nibble gates, **active high for off**, one bit per pad:

| value | meaning |
|---|---|
| `0xe1` | what POST leaves: pad 0 ungated, pads 1, 2 and 3 held off |
| `0x07` | all four pads ungated |
| `0xc3` | pads 0 and 1 ungated |
| `0x83` | pads 0, 1 and 2 ungated |

That single register is the whole reason a freshly posted M9148 lights exactly
one connector.  It was missed twice because of a precondition: **the gate only
acts while that pad's transmitter is initialised and its output gate open.**
Written to a dark, uninitialised pad it does nothing, and a writability walk on
such a board reads back values that look like the register is ignoring writes.
Clearing a bit starts the pad; setting it again does not stop one already
running.

The decisive measurement: with connector 2's transmitter initialised and
`0x0a = 0x00`, its attributes read all zeros; writing `0x1cac = 0x07` made them
read `1344/1024/806/768` with the negative-sync flags of head 2's mode in the
same operation, and `0x79` and the hot-plug byte flipped with them.  Connector
0 was untouched throughout.

The low nibble is read-only and changes as pads come up, but its meaning is
**not established**: it has been observed set for a connector with no panel
attached at all, and clear for one that was demonstrably sending.  It is
therefore neither hot-plug detect nor a "this pad is driving" flag, and nothing
in the driver gates on it.

### The transmitters are not reset by a reboot

Only the firmware's own connector is initialised at POST.  MEASURED on a
freshly posted board, connector 0 against connectors 1..3: main `0x02` `b6`/`35`,
`0x20` `21`/`20`, `0x24` `01`/`00`, `0x27` `30`/`00`, `0x2f` `82`/`c2`, `0x30`
`32`/`12`, `0x71` `20`/`00`, `0x72` `a9`/`29`, `0x77` `b2`/`00`, `0x78`
`30`/`00`, `0x87` `74`/`b0`, and page 3 `0x00` `18`/`00`, `0x03` `06`/`00`,
`0x24` `10`/`00`, `0x25` `04`/`00`, `0x26` `04`/`00`.  Every left-hand value is
a vendor table entry.

A warm reboot does **not** restore this: the transmitters keep whatever
registers were last written to them, because nothing power-cycles them.  A
"clean POST state" claim covers the GPU registers only.  Use the MISCIO reset
line (PInS function `0x20 + connector`) to get a transmitter back to defaults.

### Output gate `0x0a`, and what it costs to blank

MEASURED on the connector the firmware trained: `0x00` lets the stream out;
both `0x10` and `0x30` blank it.  The saved binaries read as if `0x30` were
"enabled"; the vendor's captured sequence passes through `0x30` and closes on
`0x00`, and the closing write is the one that matters.

`0x30` is additionally destructive.  Reading the capability mirror and running
a real AUX transaction at each step on the connector with a sink:

```
0a=0x00 -> 51=14 52=c4 63=77 65=01   AUX ok
0a=0x10 -> 51=14 52=c4 63=77 65=01   AUX ok
0a=0x30 -> 51=14 52=c4 63=00 65=80   AUX fails
0a=0x00 -> 51=14 52=c4 63=00 65=00   AUX ok, lane status still gone
```

So the AUX gate is bit 5, not bit 4, and `0x30` drops a trained link: the lane
status at `0x63..0x65` does not come back when the gate reopens, only a fresh
bring-up restores it.  This is why earlier probing read `0x51`, `0x52`, `0x53`,
`0x5c`, `0x63`, `0x64`, `0x65` and `0x8d` as zero even on the trained
connector - the readings were taken after a blank.  On an untouched trained
connector they read `14 c4 01 10 77 77 81` and `0x8d = 0x44`, which satisfies
the documented `(0x8d & 0x70) == 0x40`.

### What the driver does now

`drivers/gpu/drm/m9x` brings each head's connector up from `m9_pipe_enable`:
`m9x_tx_stream` first, and if the transmitter is not already sending this
head's mode, `m9x_tx_enable` runs the MISCIO reset, the vendor's common,
main-pre and page-3 tables, opens the output gate, runs `m9x_dp_train`, and
clears that head's bit in `0x1cac`.  It runs again on every mode change,
because the transmitter samples the video it is fed.

MEASURED with three heads held by `tools/m9-heads`:

```
head 0: was sending 1024x768 1344/806 -> already correct, skipped
head 1: was sending 640x480 800/525 -> brought up -> sending 1024x768 1344/806, pad gate 0xc3
head 2: was sending nothing          -> brought up -> sending 1024x768 1344/806, pad gate 0x83
```

Link training reported failure on both, on connectors with no panel attached.

### Training against a real sink (MEASURED 2026-09-17)

The only connector with a panel was the one the firmware had already trained,
where the driver correctly skips the bring-up.  Blanking it with `0x0a = 0x10`
first - which stops the stream but leaves AUX answering and the capability
mirror intact - forces the skip to fail, so a following modeset has to bring
the transmitter up and train it from cold against a live receiver:

```
head 0: was sending -67 0x0 0/0
head 0: DP link up, 4 lanes at 1.62 Gbps
head 0: sending 1920x1080 htotal 2200 vtotal 1125, pad gate 0x83
```

That success is **sink-side**, not the transmitter describing itself:
`m9_dp_train_once` only reports success after reading DPCD `0x202..0x204` from
the receiver over AUX and requiring align-done with clock recovery, channel
equalisation and symbol lock on every lane, and after reading DPCD `0x100`
back and requiring the rate it selected.  The transmitter's own mirror agrees
afterwards: `0x63 = 0x77`, `0x64 = 0x77`, `0x65 = 0x81`.

Four lanes at 1.62 Gbps is what the sink's capability gives - DPCD `0x001` is
`0x14` and `0x002` is `0xc4` - and matches the rate the firmware chose.

### A mode change does not drop the link

MEASURED, refuting `docs/parhelia-m9.md`: head 0 was retimed 1024x768 ->
1920x1080 -> 1024x768 with the link left alone, and the transmitter's
attributes followed each mode within the modeset while `0x63/0x64/0x65/0x8d`
stayed `77/77/01/44` throughout.  Re-initialising and re-training on every
retime is therefore not required.  The driver still checks: it reads what the
transmitter is sending and only brings the connector up again when that no
longer matches the mode, which costs one 16-byte I2C read on a modeset that
needs nothing.

### Open

Only DP-1 has a panel attached, so heads 1..3 have been taken as far as
"transmitter sending this head's mode with its pad ungated" and no further -
their training attempts fail for want of a receiver, which is correct
behaviour, and **no second panel has confirmed a picture**.  The path is
per-head and identical, so the test is one command once a panel is on another
port.

## A second connector drives a picture (CONFIRMED 2026-09-17)

The user confirmed a correct image on DP-3 while DP-1 kept its own.  Two heads
of four now reach a panel, where a freshly POSTed board reaches one.

The run, MEASURED:

```
kernel detect   DP-3 connected  520x320 mm  24 modes
m9-heads        DP-3 crtc 51 1920x1200@60
head 2 link     0x1ca4 = 0x0040a801, format 0x1c3c = 0x30002114
head 2 mode     1920x1200 60 154000 kHz
head 2          DP link training failed (-110)
head 2          sending 1920x1200 htotal 2080 vtotal 1235, pad gate 0xa5
transmitter MSA 20 08 70 00 80 07 d3 04 20 00 b0 04 20 00 06 80
                htotal 2080  hstart 112  hactive 1920  vtotal 1235
                vstart 32  vactive 1200  hsync 32  vsync 6 negative
```

**The picture arrives although link training reported a timeout**, and that is
not a contradiction.  This sink answers EDID but never answers AUX:

| | DP-1 | DP-3 |
|---|---|---|
| EDID | 13 modes | 24 modes |
| DPCD over AUX | `12 14 c4 01 01 00 01 80` | `-6`, no completion |
| lane status `0x202` | `77 77 01 01` | unreachable |
| mirrors `0x51`/`0x52` | `14`/`c4` | `00`/`00` |
| training | up, 4 lanes at 1.62 Gbps | timeout `-110` |

EDID and AUX are different paths: EDID comes through the transmitter's I2C
bridge, AUX is the native DisplayPort channel.  A sink that passes the first
and not the second is a dual-mode consumer - a passive adapter or a DP++
dongle - which carries no AUX channel and never link-trains.  DP-1, a native
DisplayPort 1.2 sink, answers both.  DERIVED from the measurements above; the
adapter type was not inspected.

So the driver's choice to **open the pad even when training fails** is what
makes this work, and it is the right default: a link the transmitter can drive
without training must not be held down by a training attempt that could not
run.

Two costs this leaves, neither fixed:

* Training spends about **3.4 s** timing out on such a sink, on every modeset
  that brings that head up (`51.77 s` to `55.14 s` in the log above).  The
  dual-mode adapter identifier is readable over DDC and would let the driver
  skip training rather than wait for it.
* `head 2: scanout publish failed (-19)` still fires once on the first
  modeset at a mode larger than the firmware's, before the surface is grown.
  The frame after it is correct; the ordering is not.

## Confirmed again on a second pair of connectors (2026-09-17)

After a target reset with panels moved to DP-1 and DP-2, the user confirmed a
picture on **both**.  MEASURED:

```
DP-1  crtc 37  1920x1200   MSA htotal 2080 hactive 1920 vtotal 1235 vactive 1200
DP-2  crtc 44  1920x1080   MSA htotal 2200 hactive 1920 vtotal 1125 vactive 1080
pad gate 0xc1   pads 0 and 1 ungated, pads 2 and 3 held off
hotplug         ddc0=03 ddc1=03 driving, ddc2=06 ddc3=06 empty
head 1          DP link training failed (-110)
head 1          sending 1920x1080 htotal 2200 vtotal 1125, pad gate 0xc3
```

This is a different head and a different pad from the earlier DP-3 confirmation,
so the encoder is per-head in fact and not only in shape.  Both connectors were
brought up from a clean POST, where only pad 0 is ungated.

**Neither sink answers AUX in this configuration** - both report `-6` with all
mirrors zero - so both are dual-mode consumers and the `-110` training timeouts
are the correct outcome rather than a failure.  Note that DP-1 answered DPCD and
trained at four lanes in the earlier session with a different panel on it, so
the property belongs to the attached sink, not to the connector.

The link-training path therefore cannot be re-exercised while only dual-mode
sinks are attached.  The earlier verification stands on its own evidence - `DP
link up, 4 lanes at 1.62 Gbps`, reported only after DPCD `0x202..0x204` read
back over AUX with align-done and per-lane clock recovery, equalisation and
symbol lock - but it is not reproducible from the current configuration.

Outstanding on this path: a dual-mode sink costs about **3.4 s per bring-up**
timing out on training that cannot succeed.  The DP++ adapter identifier is
readable over DDC and would let the driver skip training outright.  NOT DONE.
