# M9148 PInS 8.0 connector, DDC, and external DisplayPort transmitter

This note records the clean-room display contract recovered from the saved
M9 Linux and Windows drivers.  It is a static result: no packets were executed
and no target was contacted.  Run `tools/m9-kms-pins-dp-evidence.py` to verify
the exact binaries, code ranges, constant tables, and executable decode rules.

Evidence labels are:

* **[LIN]** `xserver/1_13_0/m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* **[WIN]** `MSerX/MxMM.sys`, SHA-256
  `bf743961b4846e90f32cbdf868d69240d7c403fcbd0a8d9cd6d8783ef9bd62c6`;
* **[WIN-UM]** `MSerX/MxMDI.dll`, SHA-256
  `79c8fad0eb9e0effcb63411be39b8bacfad24dda4a0e60a22d28f506e319b2e9`.

Addresses are ELF virtual addresses for **[LIN]** and loaded virtual addresses
for **[WIN]**, whose image base is `0x10000`.

## Corrections required in the existing prototype

Several earlier descriptions are unsafe to carry into KMS:

* PInS `+0x50/+0x54` is not a lane-to-signal table.  It is an eight-byte
  function-ID-to-DDC-GPIO map.
* No PInS consumer in **[LIN]** reads `+0x120..+0x140` as fixed per-connector
  DDC records.  Connector-to-DDC ownership is built through resource objects.
* HMC acquisition clears an active-low request bit.  Release sets it.  The
  prototype in `src/parhelia_m9_dp.c` does the reverse and swaps the two buses.
* The vendor selects a transmitter from the PInS resource graph.  It does not
  probe all four raw GPIO pairs.  Driving unrelated pairs can contend with a
  monitor or another board device.
* The reset path first tries two combined MISCIO functions and only then uses
  the `0x20 + connector` fallback.  The prototype implements only the fallback.
* AUX completion is polled for 1000 I2C reads.  The saved code has no one-ms
  sleep between reads and validates every I2C operation and returned count.
* Training does use DPCD.  Linux verifies DPCD `0x100`; Windows verifies
  `0x202..0x204`.  A clean driver should perform both checks.

These corrections are prerequisites for touching a real M9148 board.

## PInS 8.0 connector construction

`m9x30009` at **[LIN]** `0x2e6210` builds four connector descriptors from
PInS dwords `+0x5c`, `+0x60`, `+0x64`, and `+0x68`.  For connector `n`, it
allocates a category-8 object of `0x34` bytes, records `n` at byte `+0`, sets
dword `+0x0c` to one, and initializes bytes `+3`, `+5`, and `+9` to `0xff`.
Only the low three bytes of each PInS dword participate in route resolution;
the high byte is not parsed by this constructor.

Each nonzero route byte is either decoded directly or joined to another
resource object:

* direct link codes are in the `0x11..0x14` family and represent link indices
  zero through three in the branches that store descriptor byte `+4` or `+5`;
* `m9x17758` at `0x2e4ef0` joins other function codes to category-10 resource
  objects.  Its two matching modes compare resource bytes `+0x1c/+0x1e` or
  `+0x1c/+0x1d`;
* `m9x7911` at `0x2e4e20` searches category-11 objects by comparing the three
  route candidates with object bytes `+0x1d/+0x1e`.  The returned object index
  is stored at connector byte `+6`.

The meaning of a route byte depends on both its position and connector number.
The constructor contains position-specific ranges and association modes.  It
is therefore incorrect to scan all three bytes for the first value in
`0x11..0x14`, and byte `+6` must not be advertised as a connector-technology
enum.  A new parser should reproduce the constructor's branch table or retain
the resource graph directly.  The evidence closes the object links; names for
every category-10/11 function code remain absent from the saved binaries.

## GPIO function maps

### DDC on MMIO `0x1c48`

`m9x12355` at **[LIN]** `0x2d3780` scans eight PInS bytes in physical-bit
order: bytes 0..3 at `+0x50`, then bytes 4..7 at `+0x54`.  A match for function
ID `id` at physical bit `n` returns:

```c
data_mask = BIT(n);
oe_mask   = BIT(n + 16);
```

Both masks address MMIO `0x1c48`.  `m9x21832` at `0x2d3820` resolves two IDs
and succeeds only when both exist.  `m9x11906` at `0x2f9fb0` maps an abstract
DDC selector through eight function-code pairs:

```text
(01,02) (03,04) (05,06) (07,08)
(09,0a) (0b,0c) (0d,0e) (0f,10)
```

The selector's exact ordering through that jump table is preserved in the
vendor object graph.  Do not assume connector number equals pair number.

The electrical operation is ordinary open-drain bit-banged I2C: assert low by
enabling the output while its data bit is low, and release high by disabling
the output.  Read the data bit only after release.  This GPIO encoding and the
I2C state machine are reusable from Parhelia.  The PInS function IDs and
connector/resource association are M9-specific.

### MISCIO on MMIO `0x1c4c`

`m9x31496` at **[LIN]** `0x2d36b0` scans PInS `+0x6c/+0x70` in the same
eight-byte order.  A function at physical bit `n` returns:

```c
data_mask = BIT(n);
oe_mask   = BIT(n + 10);
```

These masks address MMIO `0x1c4c`.  This map carries transmitter reset and
other board-control GPIOs; it is not interchangeable with the DDC encoding.

## HMC ownership

The board controller shares both GPIO registers.  **[LIN]** `m9x16716` at
`0x2f84d0` / `m9x6657` at `0x2f82f0` and **[WIN]** `0x341bb` / `0x342cb`
agree on this protocol:

| GPIO owner | request in `0x0318` | grant in `0x03b0` |
|---|---:|---:|
| DDC/I2C register `0x1c48` | `BIT(29)` | `BIT(1)` |
| MISCIO register `0x1c4c` | `BIT(28)` | `BIT(0)` |

Acquisition is active-low.  On the first acquire, clear the request bit in
`0x0318`, then poll the corresponding grant bit in `0x03b0` for at most 2000
iterations with a 1000-us delay.  Nested acquisition increments the bus's
reference count and performs no MMIO.  Nested release decrements it.  The last
release sets the request bit in `0x0318` and clears the software count.

On timeout the proprietary driver resets the HMC and fabricates a grant by
ORing the grant bit into `0x03b0`.  That recovery is not safe in a clean DRM
driver.  Set the request bit to release the failed claim and return `-ETIMEDOUT`.
All I2C, EDID, AUX, transmitter-reset, hotplug, modeset, suspend, and teardown
paths must use one ownership implementation and balanced unwind.

## External transmitter identity and reset

The transmitter has two eight-bit I2C addresses retained in its object:
main page `0x72` and page 3 `0x76`.  The saved drivers do not scan alternate
addresses and contain no chip-ID allow-list.  Detection is consequently:

1. resolve the connector's resource and DDC pair from PInS;
2. acquire the `0x1c48` HMC owner;
3. require successful accesses to both known slave addresses;
4. read main register `0x7c` and require HPD bit 0;
5. classify the live mode from main register 0 bit 2 and register `0x0c` bit 7.

The **[WIN]** classifier at `0x385a0` returns absent when `reg7c.bit0` is zero.
When HPD is set, `reg00.bit2` denotes DisplayPort mode (type 3).  Otherwise
`reg0c.bit7` distinguishes its two non-DP modes.  The silicon part number is
not present in the inspected binaries and remains unknown.

The exact reset at **[WIN]** `0x390e0` is:

1. acquire MISCIO HMC ownership;
2. for connector 0..3 resolve function `0x1c + connector`; if it exists, also
   resolve fixed function `0x08` and combine both data/OE masks;
3. if the first lookup fails, resolve function `0x20 + connector` alone;
4. read `0x1c4c`; set all resolved OE bits and clear all resolved data bits;
5. delay 10000 us; set the data bits; delay another 10000 us;
6. release MISCIO ownership on every exit.

Missing function IDs or ownership failure make the connector unavailable;
they are not permission to guess a physical bit.

## EDID ownership lifecycle

The transmitter places a bridge on the connector's DDC pins.  **[LIN]**
`m9x7701`, `m9x10336`, `m9x11752`, and `m9x19123` at
`0x2de6a0/0x2de740/0x2de4d0/0x2de660` establish the complete transaction:

1. serialize the connector bus and wait for an in-progress DPCD read to end;
2. acquire HMC ownership for `0x1c48` and remember that this object owns it;
3. read transmitter main register 0 and clear bit 1 to disable its I2C bridge;
4. run the standard EDID transaction to seven-bit slave `0x50` (eight-bit wire
   address `0xa0/0xa1`) with the two PInS-derived DDC masks;
5. in a `finally` path, set main-register-0 bit 1 to enable the bridge;
6. if main register `0x76` has fault bit 3, inspect `0x79`; when `0x79.bit0` is
   set pulse register `0x75` from `0x80` to zero, then acknowledge with
   `reg76 = 0x08`;
7. clear the ownership flag and release HMC.

Every transmitter-register and GPIO-I2C failure aborts the EDID read, but the
bridge-enable and HMC-release unwind still runs.  AUX and EDID transactions are
mutually exclusive on this object.  EDID block parsing is standard DRM code and
can be shared with the Parhelia path; bridge control and resource selection are
M9148-only.

## AUX native transactions

**[LIN]** `m9x23057` at `0x2dd110` and **[WIN]** `0x394ef` agree:

```text
reg 7d = address bits  7:0
reg 7e = address bits 15:8
reg 7f = command | address bits 19:16
reg 80 = length - 1                       length 1..16
reg 81 = write FIFO
reg 82 = read FIFO
reg 83 = 1                                start
reg 76 = 0x20                             clear/ack completion
reg 84 = result                           bits 7:5 error, bits 4:0 count
```

Native write command is `0x80`; native read is `0x90`.  Program the address and
length, push all write bytes, clear stale completion, and start.  Poll
`reg76 & 0x20` for at most 1000 successful register reads.  The saved drivers
do not delay between polls.  Reject `reg84 & 0xe0`, require
`(reg84 & 0x1f) == length`, pull exactly `length` read bytes, then acknowledge
with `reg76 = 0x20`.

A clean implementation must reject zero length, length above 16, and any range
crossing the 20-bit `0x80000` boundary.  It should use a bounded scheduler-aware
deadline while retaining the vendor's 1000-read cap, and must propagate every
I2C error rather than reusing an old status byte.

## Transmitter initialization and training

The Windows tables at `0x464d8..0x465ff` and their Linux duplicates are exact
register/value pairs.  Main-page common initialization is:

```text
00=22 74=00 77=a3 78=30 72=a9 27=30
30=32 71=20 2f=82 86=f0 8b=07 02=b6
```

Register `0x77` is selected dynamically in some paths (`a3` or `b2`).  The
remaining tables are:

```text
main pre:       0a=00 24=01 20=21 76=ff 78=33
page 3 init:    00=18 03=06 24=10 25=04 26=04
main attempt:   2c=00 2d=00 2e=0c 76=ff 78=33 75=f8 75=00
                87=70 5f=0c 71=20 5e=04 87=74
page 3 retry:   24=10 25=04 26=04
```

The bandwidth limit table, in kHz, is:

```text
rate 1.62: 45000  85000 125000 165000
rate 2.70: 85000 165000 248000 340000
```

The indices correspond to the transmitter lane-capability index used by the
saved driver.  Validate a legal one-, two-, or four-lane result before indexing;
do not use a raw register value as an array index.

The shared training core is:

1. blank the output; write main `ac=e4`; clear the low nibble of `aa`; read
   `53`; write `5c=0` for DP monitor type 3 or `0x10` otherwise;
2. read lane capability from `52 & 0x1f`; reject zero, validate/clamp it for
   the bandwidth table, and write it to `5e` only after validation;
3. choose link rate `0x06` by default.  When main `51 == 0x0a`, choose `0x0a`
   if the requested pixel clock exceeds the corresponding first-rate limit;
4. write `5d=rate`, `5e=lanes`, `60=0`, `76=2`; clear and then set bit 0 of
   `5f` to launch training;
5. poll main `76.bit2` for at most 1000 register reads; acknowledge with
   `76=2`; write `8f=4`, read `8d`, and require `(8d & 0x70) == 0x40`;
6. require the transmitter link mirror (`65.bit0`, plus lane-dependent masks
   in `63/64`) and validate the sink over AUX.

Linux performs up to three training attempts, then reads DPCD `0x100` and
requires the selected rate.  Windows's training leaf uses two inner attempts;
its reinitialize caller retries the larger sequence up to three times, then
requires DPCD `0x204.bit0`, lane-status masks in `0x202`, and for four lanes
also `0x203`.  These checks cover different failure modes.  The clean success
condition is the transmitter status, DPCD `0x100 == selected_rate`,
`DPCD[0x202] & 0x07 == 0x07` for one lane, `& 0x77 == 0x77` for two lanes,
the same `0x77` test on both `0x202` and `0x203` for four lanes, and
`DPCD[0x204].bit0`.

**[WIN]** `0x38b94` gates the transmitter output by writing main `74=0x20`,
then `74=0xa0`, then `0a=0x10` when disabled or `0x30` when enabled, followed
by 1000 us delay.  The Linux routine at `0x2dce30` uses a different RMW helper;
keep these version-specific sequences distinct.

The Windows `Reinitialize` routine at `0x3922d` provides the lifecycle:

1. determine monitor type and take outer bus ownership;
2. disable output;
3. in DP mode, read DPCD `0x600`; if it is 2, write 1 and reread;
4. apply the common, main, and page-3 initialization;
5. in DP mode perform up to three initialize/train/verify attempts;
6. on any failure blank the output, acknowledge stale transmitter status,
   release ownership, and leave the connector disabled.

The reinitialize leaf does not program the GPU display controller.  At KMS
integration level, program the GPU timing, PLL, and digital feed while the
transmitter is blank, invoke reset/reinitialize/training, and apply the `0x30`
output gate only after the complete validation succeeds.  This ordering joins
the M9148 transmitter leaf to the already recovered common digital-feed path;
it is not an extra register sequence inside **[WIN]** `0x3922d`.

The GPU timing/DC/PLL programming, generic GPIO bit-banging, EDID parser, and
DRM connector state machine can share the Parhelia implementation.  PInS 8.0
resource construction, HMC ownership, reset GPIO selection, transmitter tables,
AUX engine, training, and interrupt status are M9148-only.

## Hotplug, interrupt, suspend, and teardown

**[LIN]** `m9x14575` at `0x2dee20` and the Windows handler at `0x38bf5` read
main register `0x77`, acknowledge through `0x76`, and process at most eleven
passes.  Relevant `0x76` status bits are:

* bit 5: AUX completion, acknowledge `0x20`;
* bit 3: fault, acknowledge `0x08`, inspect `0x79`, and pulse `0x75` if needed;
* bit 2: link event, acknowledge `0x04`, then verify or retrain;
* bit 6: HPD/monitor transition, schedule reprobe/reinitialize;
* bit 4: acknowledge `0x10` and inspect `0x80`;
* bits 0 and 1: acknowledge their respective bits.

An IRQ handler must not run the bit-banged I2C/training lifecycle inline.  It
should latch the GPU interrupt, mask or acknowledge the source under the bus
lock, and queue a worker.  The worker rechecks HPD, serializes against EDID and
AUX, blanks before retraining, caps the event loop at eleven passes, and emits
the DRM hotplug event after connector state is stable.  Unplug and suspend must
cancel and flush that worker, disable the output, release both HMC owners, and
discard pending AUX state.  Resume repeats reset/detection/init/modeset/training
from cold state; transmitter registers are not assumed to survive.

## Remaining static gaps

The saved drivers now provide enough low-level behavior for a guarded M9148
connector bring-up.  The smallest remaining gaps are:

1. names for every category-10/11 PInS function code and a complete clean-room
   transcription of `m9x30009`'s position-specific branch table;
2. the transmitter vendor/part number and its public electrical limits;
3. a board ROM fixture proving the category-object route for each physical
   mini-DP socket;
4. the exact GPU-interrupt source/mask bit that announces transmitter IRQ on
   each M9148 board revision;
5. live validation of reset polarity, HPD transitions, lane count/rate, EDID,
   failure recovery, and suspend/resume.

No static gap justifies probing arbitrary pins or fabricating HMC ownership.
Keep a connector unexposed until its PInS resource and both transmitter slave
addresses resolve successfully.

## Cursor-plane consequence

The existing `parhelia-m9-cursor-gamma-re.md` closes M9 cursor register banks,
control values, allocation sizes, and the vendor's intended upload.  It also
records a live contradiction: config `0xcc000000`/control 5 accepts a nominal
64x64 linear 32-bit image, but no tested stride produced an undistorted cursor.
The byte order and actual fetch swizzle remain unclosed.  Therefore a real DRM
M9 cursor plane is not yet safe to expose.

A 64x64 ARGB dumb BO is nevertheless a normal userspace allocation.  Rejecting
that BO because M9 cursor hardware is gated breaks modesetting's software
cursor path and can lead to a null map in Xorg.  KMS should accept and map the
small dumb BO while leaving the M9 hardware cursor plane disabled.  Hardware
enable requires the five live gates listed in `parhelia-m9-cursor-gamma-re.md`;
the saved binaries cannot resolve the silicon-layout contradiction alone.

## Live validation on an M9148-E1024LAF (2026-09-17)

Measured on the board, with four displays attached; see
`docs/parhelia-m9148-fourhead-outputs.md` and
`captures/m9x-vendor-fourhead-20260917/`.

* The PInS `+0x50`/`+0x54` map and the `0x1c48` `BIT(n)` / `BIT(n+16)` encoding
  are confirmed: this board reads `09 0a 0b 0c 0d 0e 0f 10`, giving four buses
  at bit pairs `(0,1) (2,3) (4,5) (6,7)`, the **even** bit being the clock.
  Connector `n` uses bus `n`, proven by EDID content for connectors 2 and 3.
* The MISCIO map `+0x6c`/`+0x70` reads `01 02 00 20 21 17 22 23`.  The
  `0x20 + connector` reset fallback is what this board uses (IDs `0x20..0x23`
  at data bits 3, 4, 6, 7); no `0x1c + connector` function exists here.  The
  `01`/`02` pair at bits 0 and 1 is a further I2C bus carrying the board
  identification EEPROM at `0xa0`, not a connector DDC bus.
* The HMC ownership table above is confirmed by the vendor trace:
  `0x1c48` = request `BIT(29)` / grant `BIT(1)`, `0x1c4c` = `BIT(28)` / `BIT(0)`,
  active low.
* The transmitter register fingerprint and the common/main/page-3 initialisation
  tables are confirmed verbatim, but the **slave addresses on this board are
  `0x10` (main), `0x12` and `0x14`**, not `0x72`/`0x76`.  A driver must not
  hard-code the `0x72`/`0x76` pair.
* Connector construction from PInS `+0x5c..+0x68` is `42001100 42001200
  42001300 42001400`: route byte `0x11 + n`, i.e. connector `n` uses link `n`,
  and the link's source field encodes controller `n` as `0x00/0x08/0x01/0x09`.
