<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
# M9148 DisplayPort transmitter: initialisation and link training

This note is a static reverse engineering of the external DisplayPort
transmitter bring-up and link-training sequence used by the Matrox M9148
(PCI `102b:0540`).  Everything here comes from the vendor Linux X driver
binary; the register tables have since been confirmed against a live board by
a parallel capture, and the points where the binary and the board disagree are
called out rather than papered over.

Confidence labels, as in `docs/parhelia-m9-3d-static-re.md`:

* **[BIN]**: directly visible in the binary or its disassembly.
* **[DER]**: a deterministic result derived from binary instructions.
* **[INF]**: a likely meaning which still needs a hardware test.

Additional labels used for facts that did not come from this analysis:

* **[MEASURED]** / **[CAPTURED]**: supplied by the hardware-probing work; kept
  verbatim so the two sources can be told apart.

## Binary identity and why it is readable

| file | role |
|---|---|
| `m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/1_15_0/m9x_drv.so` | Linux X driver, 16.5 MB, ELF x86-64 |
| `m9-vbios.rom` | `MATROX/M9100 FAMILY VGA/VBE BIOS (V1.7) b47`, 60 KB 16-bit option ROM |

The ROM is the **second, independent implementation** of the same sequence, and
§10 decodes it.  Where the two agree the claim is as strong as static analysis
gets; where they differ, both are given.

`file` reports the image as stripped and there is no DWARF, but that
understates what is available [BIN]:

* `.dynsym` holds **35503** entries, of which **31044** are *defined* `FUNC`
  symbols with an address **and a size**.  The names are obfuscated
  (`m9x30415`, `m9x24150`, ...), so they carry no meaning, but they give exact
  function boundaries for the whole `.text`.  Reading the driver is therefore
  disassembly plus naming, not boundary reconstruction.
* No relocations point into the `LLDP` code and no function-pointer table
  references it: every call is direct, so a call graph falls out of the
  disassembly [DER].
* `.data.rel.ro` carries a **numeric event-id to name table** used by the trace
  macro.  Its entries are `struct { u64 id; u64 name; }`.  Two such tables
  exist: one of display strings (`"LLDP: PerformLinkTraining"`) at
  `0x1214138`, one of event names (`"LLDP_PerformLinkTraining"`) at
  `0x12156xx` [BIN].  Call sites pass the *id*, not the string, e.g.
  `mov $0x4214,%esi`, so the ids are what locate the functions [DER].

All addresses below are `.text` virtual addresses of `xserver/1_15_0/m9x_drv.so`
unless stated otherwise.  The other ABI builds under `xserver/*/` are the same
code at different addresses; `1_15_0` was used throughout and no other build was
needed - the symbol table makes this one perfectly workable.

### LLDP event ids

`0x4201`..`0x422c` name the module's routines and its failure points [BIN]:

| id | name | id | name |
|---|---|---|---|
| 4201 | `LLDP_DPCDAccess` | 4217 | `..._LLDP_HI_Path_Down` |
| 4202 | `LLDP_DisableEncoderOutput` | 4218 | `HandleInterrupt_HPD2` |
| 4203 | `LLDP_DisableI2cBridge` | 4219 | `HandleInterrupt_DeAssert` |
| 4204 | `LLDP_Enable` | 421a | `HandleInterrupt_Assert` |
| 4205 | `LLDP_EnableI2cBridge` | 421b | `HandleInterrupt_AuxCmdFinished` |
| 4206 | `LLDP_GetDongleType` | 421c | `HandleInterrupt_TrainingAuxError` |
| 4207 | `LLDP_GetLinkStatus` | 421d | `..._TrainingAuxErrorStatus` |
| 4208 | `LLDP_GetMaxPixClock` | 421e | `..._TrainingAuxLinkIRQ5` |
| 4209 | `LLDP_GetMonitorType` | 421f | `HandleInterrupt_HPD_Toggle` |
| 420a | `LLDP_HandleInterrupt` | 4220 | `HandleInterrupt_LinkTraining` |
| 420b | `LLDP_InitCommon` | 4221 | `..._Max_Loop_count_reach` |
| 420c | `LLDP_InitInDisplayPortMode` | 4222 | `PerformLinkTraining_Master_Failed` |
| 420d | `LLDP_InitPowerOn` | 4223 | `PerformLinkTraining_Timeout` |
| 420e | `LLDP_IsAuxChannelEnabled` | 4224 | `PerformLinkTraining_Lane_Count` |
| 420f | `LLDP_PerformLinkTraining` | 4225 | `WaitDPCDReadFinished_Timeout_reach` |
| 4210 | `LLDP_ReadDPCDByte` | 4226 | `Reinitialize_Monitor_was_in_D3_state` |
| 4211 | `LLDP_Reinitialize` | 4227 | `DisableI2cBridge_PLLlockLost` |
| 4212 | `LLDP_ReleaseDDCBusAccess` | 4228 | `PerformLinkTraining_Slave_Failed` |
| 4213 | `LLDP_RequestDDCBusAccess` | 4229 | `PerformLinkTraining_Bandwidth_Failed` |
| 4214 | `LLDP_ResetPll` | 422a | `GetMaxPixClock_PixClockOverridden` |
| 4215 | `LLDP_WaitDPCDReadFinished` | 422b | `GetMaxPixClock_PixClockChanged` |
| 4216 | `LLDP_WriteDPCDByte` | 422c | `DPCDAccess_NACK_received` |

Only a subset is emitted in this build; the emitted ids anchor the function
identifications below.

### Function map

The module occupies `.text` `0x2dc030`..`0x2df4b3`.  Identifications are [DER]
unless noted; the evidence column says what fixes each one.

| address | size | name | evidence |
|---|---|---|---|
| `0x2dcd00` | 161 | `LLDP_Enable` / `DisableEncoderOutput` core | writes reg `0x0a` |
| `0x2dcdb0` | 134 | page-3 strobe (reg `0x26` bit 0) | matches `0x26=05/04` in the capture |
| `0x2dce40` | 408 | `LLDP_GetLinkStatus` | logs `0x4207` |
| `0x2dcfe0` | 931 | `LLDP_DPCDAccess` | logs `0x422c` NACK |
| `0x2dd390` | 756 | `LLDP_InitInDisplayPortMode` | its two tables match the captured main-pre and page-3 writes |
| `0x2dd690` | 328 | pre-training reset (`0x76`/`0x78`/`0x00` bit 3) | matches `0x00=2a` then `0x22` in the capture |
| `0x2dd7e0` | 183 | `LLDP_WriteDPCDByte` | logs `0x4216`, AUX command `0x8` |
| `0x2dd8a0` | 231 | `LLDP_ReadDPCDByte` | logs `0x4210`, AUX command `0x9` |
| `0x2dd9f0` | 190 | `LLDP_GetMonitorType` | HPD + reg `0x00` bit 2 + reg `0x0c` |
| `0x2ddab0` | 136 | `LLDP_GetDongleType` | reg `0x55` (DPCD `DOWNSTREAM_PORT_PRESENT`) |
| `0x2ddb40` | 443 | `LLDP_WaitDPCDReadFinished` | logs `0x4215`, `0x4225` |
| `0x2ddd00` | 1281 | `LLDP_PerformLinkTraining` | logs `0x4222`/`3`/`4`/`8`/`9` |
| `0x2de210` | 396 | `LLDP_InitPowerOn` | mode/format table |
| `0x2de3a0` | 389 | `LLDP_DisableI2cBridge` | logs `0x4227` |
| `0x2de530` | 64 | `LLDP_ReleaseDDCBusAccess` | thin wrapper on the above |
| `0x2de570` | 157 | `LLDP_EnableI2cBridge` | clears reg `0x00` bit 1 |
| `0x2de610` | 141 | `LLDP_RequestDDCBusAccess` | logs `0x4213` |
| `0x2de6a0` | 503 | `LLDP_InitCommon` | 11-entry table + reg `0x77` |
| `0x2de8a0` | 354 | `LLDP_GetMaxPixClock` | logs `0x422a` |
| `0x2dea10` | 211 | `LLDP_DisableEncoderOutput` (blank) | matches captured `0x74=20`,`0x74=a0`,`0x0a=10` |
| `0x2deaf0` | 501 | `LLDP_Reinitialize` | logs `0x4226`; the whole bring-up |
| `0x2decf0` | 1987 | `LLDP_HandleInterrupt` | logs `0x420a` and all `0x4218`..`0x4221` |

Two helpers outside the module do all the bus traffic [DER]:

| address | signature | meaning |
|---|---|---|
| `0x2b4090` | `(ctx, u32 i2c_id, u8 reg, u8 *buf, u32 len)` | transmitter register **read**; logs `CommRead` |
| `0x2b4280` | `(ctx, u32 i2c_id, u8 reg, const u8 *buf, u32 len)` | transmitter register **write**; logs `CommWrite` |

and `0x2b9100` is `ClientWaitMicroSeconds(ctx, usec)` [DER] - the id `0x0a05`
resolves to `ClientWaitMicroSeconds` in the same name table.

## 1. The transmitter

### Bus and addressing

Each connector has its own transmitter on its own bit-banged DDC pin pair;
register access is 8-bit sub-address, one byte per transaction [DER] - the read
and write helpers are always called with `len = 1`.

The transmitter answers on **three** consecutive 7-bit addresses [DER].  The
`LLDP` device object carries three separate I2C object handles:

| field | use | source |
|---|---|---|
| `dev[0x00]` | **main page** - everything below unless stated | device id of the DP transmitter object |
| `dev[0x24]` | **page 2** - hot plug only (reg `0x7c` bit 0, reg `0x80`) | resource lookup, kind `0x18` |
| `dev[0x28]` | **page 3** - PHY/system block (regs `0x00`,`0x03`,`0x24`,`0x25`,`0x26`) | resource lookup, kind `0x19` |

The lookup is `0x2b3830(ctx, kind, nth, &out)`, which walks the 32-entry I2C
device table looking for `obj[0] == kind && obj[0x18] == 1` [BIN].  **No I2C
slave address appears anywhere in the `LLDP` code** [BIN]: the table is
allocated empty at `0x2b4d60` with 32 slots of 0x24 bytes, each tagged `-1`,
and filled from board configuration.  The brief's `0x72`/`0x76` is therefore
**not** a hard-coded constant in this module; it is whatever the board data
says.  This is consistent with the board answering at 8-bit `0x10`/`0x12`/`0x14`
[MEASURED] - same `+0/+2/+4` stride, different base.  `0x16` also answering
[MEASURED] fits a fourth page the `LLDP` module never touches [INF].

So: **probe for the base, then use base, base+1, base+2 as 7-bit addresses.**
The existing `m9_tx_candidates[]` probe in `m9_ddc.c` is the right shape; it
needs `tx_addr + 2` added for page 3.

### Register map

Recovered from every call site in the module [BIN for the offsets and the
values; the *meanings* are [DER] where the code's use of them fixes them and
[INF] where it does not].

| reg | page | direction | meaning |
|---|---|---|---|
| `0x00` | main | r/w | bit 1 = **I2C bridge disabled** (1 = AUX owns the link, 0 = DDC passes through to the sink) [DER]; bit 2 = sink/DP state, see §7 [BIN]; bit 3 = pulsed 1 then 0 as a soft reset [DER] |
| `0x02` | main | w | `0xb6` from `InitCommon` [BIN]; meaning [INF] |
| `0x0a` | main | r/w | **two gates**, see §11: bit 4 set blanks the stream and leaves AUX and the link alone, bit 5 set kills AUX and the link and needs a re-train; the X driver writes bits[3:2] = `0b11` for DisplayPort, the VBIOS writes `0x00` [BIN] |
| `0x0c` | main | r | bit 7 distinguishes the two non-DP monitor types [DER] |
| `0x10`..`0x1f` | main | r | main-stream attributes of the video actually reaching the transmitter, LE 16-bit pairs: `0x10` htotal, `0x12` hstart, `0x14` hactive, `0x16` vtotal, `0x18` vstart, `0x1a` vactive, `0x1c` hsync width, `0x1e` vsync width [MEASURED]. The `LLDP` module never touches them [BIN] |
| `0x15`,`0x1c`,`0x1e`,`0x1f`,`0x20`,`0x21`,`0x22`,`0x24`,`0x25`,`0x26`,`0x27`,`0x2c`,`0x2d`,`0x2e`,`0x2f`,`0x30` | main | w | configuration written from tables, see §2 |
| `0x48`,`0x49`,`0x4a` | main | r/w | DVI/TMDS side; `0x4a` read returns a mode byte whose values `0x01` and `0x81` are special-cased [BIN] |
| `0x50`..`0x5b` | main | r | **mirror of DPCD `0x00000`..`0x0000b`** (receiver capability) [DER]: `0x51` = `MAX_LINK_RATE`, `0x52` = `MAX_LANE_COUNT` (`&0x1f` lanes, bit 7 `ENHANCED_FRAME_CAP`), `0x53` = `MAX_DOWNSPREAD`, `0x55` = `DOWNSTREAM_PORT_PRESENT` |
| `0x5c` | main | w | `0x00` when the sink is a native DP sink, `0x10` when it is behind a dongle [BIN] |
| `0x5d` | main | r/w | **`LINK_BW_SET`**: `0x06` = 1.62 Gbps, `0x0a` = 2.7 Gbps [DER] |
| `0x5e` | main | r/w | **`LANE_COUNT_SET`**, bit 7 = enhanced framing [DER] |
| `0x5f` | main | r/w | link-training control; **bit 0 written 0 then 1 starts the hardware training sequence** [DER].  Other bits preset to `0b1100` by the init table |
| `0x60` | main | w | written `0x00` before training [BIN]; `TRAINING_PATTERN_SET`-like [INF] |
| `0x63`,`0x64` | main | r | **mirror of DPCD `0x00202`/`0x00203`** lane status; `(v & 0x77) == 0x77` means CR done, EQ done and symbol locked for that lane pair [DER] |
| `0x65` | main | r | **mirror of DPCD `0x00204`**; bit 0 = `INTERLANE_ALIGN_DONE` [DER] |
| `0x71` | main | w | `0x20` from the DP init table [BIN] |
| `0x72` | main | w | `0xa9` from `InitCommon` [BIN] |
| `0x74` | main | w | `0x20` then `0xa0` to blank the encoder, `0x00` in `InitCommon` [BIN]; bit 7 = output off [INF] |
| `0x75` | main | r/w | **PHY PLL control**; bit 7 written 1 then 0 resets the PLL [DER] |
| `0x76` | main | r/w1c | **interrupt status**, see below |
| `0x77` | main | r/w | interrupt enable / handled-source mask: `0xb2` or `0xa3` [BIN] |
| `0x78` | main | r/w1c | link-path status; init `0x33`, bits 0 and 1 are the error flags the driver clears [DER] |
| `0x79` | main | r | PLL-lock detail; bit 0 set means lock lost [DER] |
| `0x7c` | **page 2** | r | bit 0 = **hot-plug detect state** [DER] |
| `0x7d` | main | w | AUX address `[7:0]` [DER] |
| `0x7e` | main | w | AUX address `[15:8]` [DER] |
| `0x7f` | main | w | bits[7:4] AUX command, bits[3:0] address `[19:16]` [DER] |
| `0x80` | main | w | AUX transfer length **minus one** [DER] |
| `0x80` | **page 2** | r/w | hot-plug latch; read, then written back masked with `0x77` [BIN] |
| `0x81` | main | w | AUX write-data FIFO, one byte per write [DER] |
| `0x82` | main | r | AUX read-data FIFO, one byte per read [DER] |
| `0x83` | main | w | write `0x01` = **start AUX transaction** [DER] |
| `0x84` | main | r | AUX reply: bits[7:5] reply code (0 = ACK, non-zero = NACK/DEFER), bits[4:0] byte count actually moved [DER] |
| `0x85` | main | r | AUX/training error detail; `(v & 3) == 2` is the benign "I2C to DPCD error" [DER] |
| `0x86` | main | w | `0xf0` from `InitCommon` [BIN] |
| `0x87` | main | w | `0x70` then `0x74` across the DP init table [BIN] |
| `0x8b` | main | w | `0x07` from `InitCommon` [BIN] |
| `0x8d` | main | r | selected status readback; `(v & 0x70) == 0x40` after training means the transmitter's own state machine succeeded [DER] |
| `0x8f` | main | w | status selector; written `0x04` immediately before reading `0x8d` [BIN] |
| `0xaa` | main | r/w | low nibble cleared at the head of link training [BIN]; meaning [INF] |
| `0xac` | main | w | `0xe4` at the head of link training [BIN]; meaning [INF] |

Page 3 registers used: `0x00` (`0x18`), `0x03` (`0x06`), `0x24` (`0x10`),
`0x25` (`0x04`), `0x26` (`0x04` and a bit-0 strobe) [BIN].

#### Interrupt status, register `0x76`

Write-1-to-clear.  Bit meanings from `LLDP_HandleInterrupt` at `0x2decf0`,
where each branch logs a named event [DER]:

| bit | event | handler |
|---|---|---|
| `0x01` | I2C-bridge / DPCD transaction finished | clear, mark work done |
| `0x02` | **link training finished** | clear |
| `0x04` | HPD toggle (short IRQ pulse) | clear, log `HPD_Toggle`, re-check link status |
| `0x08` | training / AUX error | clear, read `0x85`; if `(v & 3) == 2` treat as benign, else set `0x0a |= 0x30`, pulse `0x75` bit 7, unblank |
| `0x10` | HPD de-assert | clear, read page-2 `0x80`, write it back `& 0x77` |
| `0x20` | **AUX command finished** | clear; this is the bit `DPCDAccess` polls |
| `0x40` | HPD assert | clear, re-detect and re-initialise the output |
| `0x80` | not handled by the driver [BIN]; reads `0x80`/`0xc0` on untrained connectors [MEASURED] |

## 2. `LLDP_InitInDisplayPortMode` (`0x2dd390`)

Two straight register tables and one read-modify-write.  Table entries are
`struct { u32 reg; u32 val; }` in `.rodata`; the loop is fully unrolled in the
binary, so the order below is the emission order [BIN].

**Main page, `.rodata` `0xe98b40`, 12 entries, in order:**

```
  0x2c = 0x00
  0x2d = 0x00
  0x2e = 0x0c
  0x76 = 0xff      /* clear every pending interrupt */
  0x78 = 0x33      /* clear both link-path error flags */
  0x75 = 0xf8      /* PHY PLL: assert                 */
  0x75 = 0x00      /* PHY PLL: release                */
  0x87 = 0x70
  0x5f = 0x0c      /* training control, bit 0 clear   */
  0x71 = 0x20
  0x5e = 0x04      /* lane count default 4            */
  0x87 = 0x74
```

**Then, main page:** read `0x00`, write it back with **bit 3 cleared** [BIN].

**Then page 3, `.rodata` `0xe98ba0`, 5 entries, in order:**

```
  0x00 = 0x18
  0x03 = 0x06
  0x24 = 0x10
  0x25 = 0x04
  0x26 = 0x04
```

Finally `dev->state (offset 0x2c) = 2` [BIN].

No delays appear anywhere in this function [BIN]: each entry is one I2C
transaction and the bus timing is the only pacing.

Two tables belong to the steps that run before it.

**`LLDP_InitCommon` (`0x2de6a0`), main page, `.rodata` `0xe98c20`, 11 entries:**

```
  0x00 = 0x22
  0x74 = 0x00
  0x78 = 0x30
  0x72 = 0xa9
  0x27 = 0x30
  0x30 = 0x32
  0x71 = 0x20
  0x2f = 0x82
  0x86 = 0xf0
  0x8b = 0x07
  0x02 = 0xb6
```

followed by `0x77 = 0xb2` when board config item `0x15` is non-zero, else
`0x77 = 0xa3` [BIN].

**`LLDP_InitPowerOn` (`0x2de210`), main page:** `0x0a = 0x0c` for a
DisplayPort sink, `0x0a = 0x00` otherwise; then `.rodata` `0xe98be0`, 4
entries [BIN]:

```
  0x24 = 0x01
  0x20 = 0x21
  0x76 = 0xff
  0x78 = 0x33
```

and, only when the head's display object reports type `0x0a`, additionally
`0x20 = 0x41` and `0x24 = 0x0d` [BIN].

**Pre-training reset (`0x2dd690`), page 3 then main, `.rodata` `0xe98c00`:**

```
  page3 0x24 = 0x10
  page3 0x25 = 0x04
  page3 0x26 = 0x04
  main  0x76 = 0xff
  main  0x78 = 0x33
  main  0x00: read, write |= 0x08, write &= ~0x08     /* soft reset pulse */
```

**Page-3 strobe (`0x2dcdb0`):** read page-3 `0x26`, write `|= 0x01`, write
`&= ~0x01` [BIN].

**Blank (`LLDP_DisableEncoderOutput`, `0x2dea10`):** when board config item
`mode` resolves into `6..10`, write main `0x74 = 0x20` then `0x74 = 0xa0`;
then read main `0x0a` and write it back as `(v & ~0x20) | 0x10`; then
`ClientWaitMicroSeconds(1000)` [BIN/DER].

**Unblank (`LLDP_Enable`, `0x2dcd00` with the enable argument non-zero):**
read main `0x0a`, write back `v & ~0x30` [BIN].

### Cross-check against the board

Every one of these table entries appears in the measured difference between
the connector POST trained and the three it did not [MEASURED]: main `0x02`
`b6`/`35`, `0x20` `21`/`20`, `0x24` `01`/`00`, `0x27` `30`/`00`, `0x2f`
`82`/`c2`, `0x30` `32`/`12`, `0x71` `20`/`00`, `0x72` `a9`/`29`, `0x77`
`b2`/`00`, `0x87` `74`/`b0`, `0x5f` `0c`/`00`, and page 3 `0x00` `18`/`00`,
`0x03` `06`/`00`, `0x24` `10`/`00`, `0x25` `04`/`00`, `0x26` `04`/`00`.  The
left-hand value of each pair is exactly the table value recovered above.  That
is independent confirmation that POST runs this same sequence.

`0x78` reads `0x30` on the trained connector although the tables write `0x33`
[MEASURED]: the two low bits are the link-path error flags, which the interrupt
handler clears once the link is up [DER].

`0x5e` reads `0x02` on the trained connector against the X driver table's
`0x04` [MEASURED].  **This is not a training signature**: the VBIOS's copy of
the same table seeds `0x5e = 0x02` directly (§10), and so does `0x5f = 0x0c`.
So `0x5e`/`0x5f` reading `02`/`0c` rather than `04`/`00` proves only that *the
DisplayPort mode table ran on that connector*, not that training succeeded.
An earlier reading of this difference as the "clearest training-state
signature" is withdrawn.

What the pair does still prove is narrower and useful: the VBIOS writes
`0x5e = lanes` from register `0x52` unconditionally during training, with no
zero guard (§10).  `0x5e` reading `0x02` after POST therefore means either the
seed survived untouched, or register `0x52` returned `0x02`.  It cannot mean
`0x52` returned zero and training still ran.

## 3. `LLDP_PerformLinkTraining` (`0x2ddd00`)

The essential finding: **the transmitter contains the training state machine.**
The driver does not run a clock-recovery loop, does not step voltage swing or
pre-emphasis, and does not write `TRAINING_PATTERN_SET` per phase.  It hands
the chip a rate and a lane count, sets one bit, and waits.  The vendor's four
distinct failure strings are four *outcomes* of that one hardware attempt, not
four phases [DER] - this is the whole of `0x2ddd00`, 1281 bytes, and nothing in
it writes DPCD `0x102`, `0x103`..`0x106` or reads `0x206`/`0x207`.

`dev` is the `LLDP` device object; `dev[0x1d]` is the 1-based connector index.

### Prologue - run once

```
 1.  write main 0xac = 0xe4
 2.  read  main 0xaa -> v ; write main 0xaa = v & 0xf0
 3.  read  main 0x53               /* value discarded */
 4.  dongle = LLDP_GetDongleType()
 5.  write main 0x5c = (dongle == 3) ? 0x00 : 0x10
 6.  read  main 0x52 -> lane
 7.  if ((lane & 0x1f) == 0)  ->  fail "Lane Count = 0 Error", return 0
 8.  lane_index = ((lane & 0x1f) < 5) ? (lane & 0x1f) - 1 : 3
 9.  read  main 0x5d -> rate
10.  pixclk = 0
     if (board revision word != 0xff && dev[0x1d] != 0):
         board = board object;  target = board[0x0c + dev[0x1d] - 1]
         pixclk = display_target(target)[0x10]        /* kHz */
11.  if (rate == 0):                      /* nothing latched yet */
         read main 0x51 -> maxrate
         if (maxrate == 0x0a && pixclk > rbr_table[lane_index])
             rate = 0x0a                  /* 2.7 Gbps  */
         else
             rate = 0x06                  /* 1.62 Gbps */
```

Steps 1-3 have no counterpart elsewhere in the module and are the only writes
that precede the first read of the DPCD-mirror block [BIN].  See §8 for why
that matters.

`rbr_table` is `.rodata` `0xe98b20`, eight `u32` of kHz [BIN]:

| lanes | 1.62 Gbps (offset 0) | 2.7 Gbps (offset 16) |
|---|---|---|
| 1 | 45000 | 85000 |
| 2 | 85000 | 165000 |
| 3 | 125000 | 248000 |
| 4 | 165000 | 340000 |

`LLDP_GetMaxPixClock` at `0x2de8a0` indexes the same table with
`(rate == 0x06 ? 0 : 16) + lane_index * 4` [BIN], which is what fixes the
column meaning.  For a non-DP sink it returns 340000 when the detect result is
0 and 165000 otherwise [BIN].

### Attempt loop - at most three iterations, identical parameters

```
for (attempt = 0; attempt < 3; attempt++) {
 12.  write main 0x5d = rate
 13.  write main 0x5e = lane        /* sink's MAX_LANE_COUNT byte verbatim,
                                       bit 7 = enhanced framing, preserved */
 14.  write main 0x60 = 0x00
 15.  write main 0x76 = 0x02        /* clear "training finished"            */
 16.  read  main 0x5f -> t
      write main 0x5f = t & ~0x01
      write main 0x5f = t |  0x01   /* 0 -> 1 edge starts training          */
 17.  poll:  read main 0x76, up to 1000 times, until bit 1 is set
             (no delay between reads: each read is one I2C transaction)
             on exhaustion -> log "Link training Timeout", next attempt
 18.  write main 0x8f = 0x04
      read  main 0x8d -> s
      write main 0x76 = 0x02        /* clear again                          */
 19.  if ((s & 0x70) != 0x40) -> log "Link training failed - Master",
                                  next attempt
 20.  if (!LLDP_GetLinkStatus())   -> log "Link training failed - Slave",
                                  next attempt
 21.  if (!ReadDPCDByte(0x00100, &bw)) -> log "failed - Bandwidth", next
 22.  if (bw != rate)                   -> log "failed - Bandwidth", next
 23.  dev->state = 1 ;  return 1      /* success */
}
dev->state = 1 ;  return 0
```

There is **no fallback inside this function** [DER]: the retry target is the
top of step 12 and neither `rate` nor `lane` is modified.  Rate and lane count
are chosen once, in the prologue.

### The receiver capability this board actually reports

With the link healthy, connector 0's sink reports [MEASURED]:

```
  0x51 = 0x14     MAX_LINK_RATE  = 5.4 Gbps (DP 1.2 HBR2)
  0x52 = 0xc4     MAX_LANE_COUNT = 4 lanes, bit 7 enhanced framing,
                                   bit 6 TPS3 supported
```

Two implementation notes fall out of that [DER]:

* **The rate code must be compared, not masked.**  The VBIOS computes
  `(0x51 & 0x0f) == 0x0a`, which turns `0x14` into `4` and silently selects
  1.62 Gbps.  The X driver's `rate == 0` path has the same shape.  Link rate
  codes are monotonic, so `0x51 >= 0x0a` is the correct test, capped at
  2.7 Gbps because neither vendor implementation ever writes `0x14`.
* **The lane byte goes out raw.**  Both implementations write `0x52`'s whole
  value into `0x5e`, so bit 6 (TPS3, meaningless in `LANE_COUNT_SET`) is
  written too [BIN].  That is vendor behaviour and this note keeps it; if
  training fails with "Master" on such a sink, masking `0x5e` to `0x9f` is the
  first thing to try [INF].

### The four failure outcomes, and what each means

| log | test | meaning |
|---|---|---|
| `Link training Timeout` | `0x76` bit 1 never set in 1000 polls | the transmitter's state machine never finished |
| `Link training failed - Master` | `(0x8d & 0x70) != 0x40` | the transmitter's own state machine reports failure |
| `Link training failed - Slave` | `LLDP_GetLinkStatus()` false | the sink's lane status never reached CR+EQ+lock |
| `Link training failed - Bandwidth` | DPCD `0x00100` read fails, or reads back something other than the requested rate | the sink did not accept the link rate |

### `LLDP_GetLinkStatus` (`0x2dce40`)

```
 read main 0x65 ; require bit 0 set         /* DPCD 0x204 interlane align  */
 read main 0x5e ; lanes = v & 0x07
 switch (lanes):
   1: read main 0x63 ; require (v & 0x07) == 0x07
   2: read main 0x63 ; require (v & 0x77) == 0x77
   4: read main 0x63 ; require (v & 0x77) == 0x77
      read main 0x64 ; require (v & 0x77) == 0x77
   default: fail
```

`0x07` per lane is `CR_DONE | CHANNEL_EQ_DONE | SYMBOL_LOCKED`, which is what
identifies `0x63`/`0x64` as the DPCD `0x202`/`0x203` mirror [DER].

## 4. DPCD access

### `LLDP_DPCDAccess` (`0x2dcfe0`)

`(ctx, dev, u32 address, u32 length, u8 command, u8 *buffer)`.
`command` is the AUX command already shifted into the high nibble: `0x80`
native write, `0x90` native read [DER].

```
 1.  if (address > 0x7ffff): address = 0x008000 and the top nibble is 0
     (a 20-bit address is all the register file can carry)
 2.  if (address + length >= 0x80001) length = 0x80000 - address
 3.  write main 0x7d = address & 0xff
 4.  write main 0x7e = (address >> 8) & 0xff
 5.  write main 0x7f = command | ((address >> 16) & 0x0f)
 6.  n = min(length, 16)
 7.  write main 0x80 = n - 1
 8.  if (command == 0x80)                       /* native write */
         for (i = 0; i < n; i++) write main 0x81 = buffer[i]
 9.  write main 0x76 = 0x20                     /* clear AUX-done   */
10.  write main 0x83 = 0x01                     /* start            */
11.  poll: read main 0x76 up to 1000 times until bit 5 is set;
     on exhaustion, fall through once more and give up -> return 0
12.  read  main 0x84 -> reply
13.  write main 0x76 = 0x20                     /* clear AUX-done   */
14.  if (reply & 0xe0)   -> log "DPCD Access : Nack received", return 0
15.  if (command == 0x90):                      /* native read  */
         if ((reply & 0x1f) != n) return 1      /* short reply, buffer kept */
         memset(buffer, 0, n)
         for (i = 0; i < n; i++) buffer[i] = read main 0x82
         return 1
16.  return (command == 0x80)
```

`LLDP_ReadDPCDByte(ctx, dev, address, u8 *out)` is
`DPCDAccess(address, 1, 0x90, out)`; `LLDP_WriteDPCDByte` is
`DPCDAccess(address, 1, 0x80, in)` [BIN].  Both are one-byte wrappers, but the
core handles up to 16 bytes per transaction, which is the DisplayPort AUX
limit [DER].

### `LLDP_WaitDPCDReadFinished` (`0x2ddb40`)

This is **not** about native AUX - it waits for the transmitter's *I2C bridge*
(DDC-over-AUX) to settle, and it services a PLL fault while waiting [DER]:

```
 pll_resets = 0 ; polls = 0
 loop:
   read main 0x76 -> s
   if (pll_resets <= 4 && (s & 0x08)):          /* PLL lock lost */
        read  main 0x75 -> p
        write main 0x75 = p | 0x80
        write main 0x75 = p & ~0x80
        write main 0x76 = 0x08
        pll_resets++ ; polls = 0 ; continue
   polls++
   if (polls > 999) -> log "Timeout_reach", fall through to done
   if (!(s & 0x01)) continue
 done:
   write main 0x76 = 0x01
   dev->state = 1
```

## 5. DDC bus arbitration

The sink's EDID is read through the transmitter's I2C bridge, which must be
open while the host bit-bangs DDC and closed while AUX is in use [DER].

```
LLDP_RequestDDCBusAccess (0x2de610):
    if (dev->state == 2) LLDP_WaitDPCDReadFinished()
    LLDP_EnableI2cBridge()

LLDP_EnableI2cBridge (0x2de570):
    read main 0x00 ; write main 0x00 = v & ~0x02
    (when the caller is inside an interrupt, dev[0x30] = 1 first)

LLDP_ReleaseDDCBusAccess (0x2de530) -> LLDP_DisableI2cBridge (0x2de3a0):
    read  main 0x00 ; write main 0x00 = v | 0x02
    read  main 0x76 -> s
    if (s & 0x08):                              /* PLL fault while bridged */
        read main 0x79 -> p
        if (p & 0x01):
            log "Disable I2C bridge PLL lock lost"
            write main 0x75 = 0x80
            write main 0x75 = 0x00
        write main 0x76 = 0x08
```

The kernel driver's existing `m9x_ddc_read_edid()` already does the `0x00`
bit-1 dance with a 20 ms settle; it should gain the `0x76` bit-3 check on
release.

## 6. The whole bring-up: `LLDP_Reinitialize` (`0x2deaf0`)

`(ctx, dev, mode)`.  This is the function every caller uses, and it is the
per-head encoder entry point.

```
 1.  kind = LLDP_GetMonitorType()              /* 0, 1, 2 or 3 */
 2.  in_isr = (driver is inside its interrupt handler)
     if (in_isr) take the client lock
 3.  LLDP_DisableEncoderOutput(dev, mode)      /* blank; 0x74=0x20,0xa0;
                                                  0x0a |= 0x10; 1 ms      */
 4.  if (kind == 3):                           /* DisplayPort sink        */
 4a.     if (ReadDPCDByte(0x00600, &p) && p == 0x02):     /* sink in D3   */
             WriteDPCDByte(0x00600, 0x01)                 /* D0           */
             ReadDPCDByte(0x00600, &p)
             log "Monitor_was_in_D3_state"
 4b.     LLDP_InitCommon(dev)                  /* 11-entry table + 0x77   */
 4c.     LLDP_InitPowerOn(dev, mode, 3)        /* 0x0a=0x0c + 4-entry tbl */
 4d.     pre-training reset (0x2dd690)         /* page-3 tbl, 0x76, 0x78,
                                                  0x00 bit-3 pulse        */
 4e.     for (try = 0; try < 3; try++):
             LLDP_InitInDisplayPortMode(dev)   /* §2                      */
             LLDP_WaitDPCDReadFinished(dev)
             page-3 0x26 bit-0 strobe (0x2dcdb0)
             if (LLDP_PerformLinkTraining(dev)) break    /* §3            */
 5.  else:                                     /* DVI / dongle            */
         LLDP_InitCommon(dev)
         LLDP_InitPowerOn(dev, mode, kind)
         pre-training reset
         page-3 0x26 bit-0 strobe
 6.  if (in_isr) release the client lock
```

So the maximum is **nine** training attempts: three inner, three outer, with
a full re-initialisation between outer attempts.

`LLDP_GetMonitorType` (`0x2dd9f0`):

```
   read page2 0x7c ; if (!(v & 0x01)) return 0          /* no hot plug    */
   read main  0x00 ; if (v & 0x04)   return 3           /* DisplayPort    */
   read main  0x0c ; return (v & 0x80) ? 1 : 2
```

`LLDP_GetDongleType` (`0x2ddab0`), used only for register `0x5c`:

```
   if (LLDP_GetMonitorType() != 3) return 0
   read main 0x55 ; i = (v & 0x07) - 1 ; if (i > 6) return 0
   return table_0xe98c80[i]        /* {2, 0, 3, 0, 1, 0, 4} */
```

`0x55` is the DPCD `DOWNSTREAM_PORT_PRESENT` mirror, which is what fixes the
`0x50`-block base [DER].

## 7. Contradictions with the board that this analysis does not resolve

These are flagged, not explained away.

**a. The AUX engine at `0x7d`..`0x84` did not complete, and `0x51`, `0x52`,
`0x53`, `0x5c`, `0x63`, `0x64`, `0x65` and `0x8d` all read zero [MEASURED].**
Resolved: §11 has it.  All eight are downstream of main `0x0a` bit 5, and the
register window recovered here is confirmed - with the link healthy the same
registers read `0x51 = 0x14`, `0x52 = 0xc4`, `0x63 = 0x64 = 0x77`,
`0x65 = 0x81`, `0x8d = 0x44` and AUX answers [MEASURED].

**b. Main register `0x00` bit 2.**  `LLDP_GetMonitorType` returns "DisplayPort
sink" when this bit is **set** [BIN] - the test is `testb $0x4, v ; jne`.  The
board shows it **clear** on the connector carrying a stream and **set** on
connectors with a sink attached but dark [MEASURED], and the vendor's own
captured writes are `0x22` where there is a sink and `0x26` where there is not
[CAPTURED].  Those three statements cannot all be true of a single static bit.
The most likely resolution is that the bit is hardware status meaning "a sink
is present and no stream is locked to it", which reads set exactly in the state
`GetMonitorType` is called in - before a stream exists - and is cleared by the
chip once one does [INF].  `docs/parhelia-m9148-kms-pins-dp-re.md` calls it
"DisplayPort mode"; that name is at best incomplete and should not be used as
an anchor.  The implementation does not gate anything on it: it uses page-2
`0x7c` bit 0 for presence and treats every connector as DisplayPort, because
all four are mini-DisplayPort.

## 8. The GPU side

The X driver's M9 display HAL is `.text` `0x2ee500`..`0x2f6xxx`, reached through
`cmp $0x540` / `cmp $0x2539` device-ID thunks; `0x2fe...` is the P690 twin
[BIN].  The trace-id table names its routines the same way it names the `LLDP`
ones, so the functions below carry their real names [BIN]:

| address | name | id |
|---|---|---|
| `0x2f4030` | `PhxEnableDVO` | `0x500a` |
| `0x2f1520` | `PhxEnableDVOSyncs` | `0x500b` |
| `0x2f01e0` | `PhxDisableDVOSyncs` | `0x500e` |
| `0x2f2d80` | `PhxEnableDataOutputPath` | `0x500c` |
| `0x2f3360` | `PhxEnableSecondDVO` | `0x500d` |
| `0x2f3af0` | `PhxForceBlankOutput` | `0x5019` |
| `0x2f4920` | `PhxEnableTransmitterDevice` | `0x5012` |
| `0x2f35a0` | `PhxEnableDisplayController` | `0x5008` |
| `0x2f2a00` | `PhxSetMonitorDPMState` | `0x5003` |
| `0x2b62a0` | `H2OCeApplyAndReleaseConfig` | `0x0209` |
| `0x2b3b90` | `CommReExecuteDeviceInitSequences` | `0x1009` |
| `0x2e2ae0` | `LLMonReadValidEdid` | - |
| `0x2eb4e0` | `MLBoard_HandleHPDEvent` | - |

MMIO goes through `0x2b89a0` = `read32(dev, offset, u32 *)` and `0x2b88f0` =
`write32(dev, offset, value)` [DER].

**Nothing in the `LLDP` module touches MMIO at all** [BIN]: its only hardware
access is the two I2C helpers.  The link registers belong to the display HAL,
and the ordering between the two is what matters.

### Per-head dispatch

`LLDP_HandleInterrupt` is reached per head through `MLBoard_HandleHPDEvent` at
`0x2eb4e0`, which is the clearest statement in the binary that this is a
per-head path [BIN]:

```
 if (head > 3) error                             /* four heads            */
 map     = object(type 9, instance 0, size 4)    /* head -> display id    */
 display = object(type 8, instance map[head], size 0x34)
 if (display[0x07] == 0xff) return 0             /* no output attached    */
 output  = object(type 0xa, instance display[0x07], size 0x34)
 if (output[0x04] != 2) return 0                 /* not a DisplayPort tx  */
 return LLDP_HandleInterrupt(output, head)
```

`output[0x04] == 2` is the class tag for `LLDP` [DER]; the constructor at
`0x2e5ebb` writes it literally, and also fills `output[0x1d]` with the
connector index, `output[0x14]` with the maximum pixel clock (default
`0x53020` = 340000 kHz) and `output[0x24]`/`output[0x28]` with the page-2 and
page-3 I2C handles [BIN].

### `0x1cac`: two nibbles, two meanings

**Bits [3:0]: one bit per connector, read only, meaning not established.**
`0x2ee500` exists for nothing else [BIN]:

```
  m9x15840(ctx, index):        /* index 0..3 */
      if (index > 3) return 0
      read 0x1cac -> v
      return (v & 0x0f) >> index & 1
```

Its one caller, `0x2e96e0`, resolves the bit index from PInS and then reads it
[BIN]: on PInS 8 it calls `0x2f94c0` with function id `index + 1` against
`PInS+0x58`, which on this board is `01 02 03 04`, so the mapping is the
identity; on PInS 7 it calls `S90GetHpdPinState` instead - a GPIO path that
does not touch `0x1cac`.  The name of that PInS 7 sibling is the only evidence
for what the HAL believes these bits are, and it says hot-plug detect.

**That evidence does not carry, and an earlier [DER] claim that the low nibble
is per-connector hot-plug detect is withdrawn.**  Two reasons:

* `0x2e96e0` **is never called** [BIN].  It has no call site anywhere in
  `.text`, no entry in any relocation, no function-pointer table, and it is not
  imported by `libglx.so` or `libGL.so.2.0.0` beside it.  It is a `GLOBAL`
  symbol of the vendor's shared hardware library, exposed and unused by this X
  driver, so it says nothing about the M9148.
* The board contradicts it directly [MEASURED]: after three heads came up the
  register reads `0x83`, so bits 0 and 1 are set - but connector 1 has no panel
  attached at all, and bit 2 is clear although connector 2's transmitter was
  demonstrably sending 1024x768.  It is neither hot-plug detect nor "this pad
  is driving".

So: the HAL contains a routine that would read this nibble as an HPD source on
a PInS 8 board; nothing in this driver takes that path; and what the bits mean
on the M9148 is **not established** [INF].  Nothing should gate on them.

**Bits [7:4]: one gate per link, set turns that link off** [BIN].  Two sites
fix this:

* `0x2f2ae0`, the display-block teardown that runs at the top of every
  modeset, saves `0x1c00`, `0x1c04`, `0x1c08`, `0x1c0c`, `0x1c24` and `0x1cac`,
  then writes each back with its disable bit set - and for `0x1cac` the
  disable value is `old | 0xf0`, followed by a 10 us delay.  Four links, four
  bits, `0xf0` to park them all.
* `0x2ee540`, a precondition helper for DAC load sensing, treats
  `(0x1cac & 0xf0) == 0xf0` as "all four links parked" and refuses to proceed
  otherwise [BIN].

That matches the board: POST leaves `0xe1`, so link 0 is live and links 1, 2
and 3 are parked - one working output of four, and the nibble steps
`0xe` -> `0xc` -> `0x8` as a driver brings heads up [MEASURED].  Bit `4 + n`
parks link `n`, active high [DER].

### `0x1c9c`/`0x1ca0`/`0x1ca4`/`0x1ca8`: the link word

Written by `PhxEnableDVO` at `0x2f4030`, one link per call, and initialised to
`0x00040000` (path off, everything else zero) for all four by the display-block
init at `0x2f5f90` [BIN].  The base value is the PInS patch, `0x0004a800` on
this board; the fields the code then sets are [BIN unless noted]:

| bits | meaning |
|---|---|
| [4:0] | source display controller: `0x00`, `0x08`, `0x01`, `0x09` for connectors 0..3 |
| [17:13] | format / bit depth; the patch gives `0xa800`, deep colour forces `(v & ~0x0003f800) \| 0x00012000` |
| [18] | **path off / blank; 0 = output on** |
| [19], [21] | the rest of the disable group: `PhxDisableDVOSyncs` sets 18, 19 and 21 together (`\|= 0x002c0000`), `PhxEnableDVOSyncs` clears 19 and 21 |
| [20] | HSync polarity invert, set when the mode's positive-HSync flag is clear |
| [22] | VSync polarity invert, set when the mode's positive-VSync flag is clear |
| [25] | dual-link gang; set only for a dual-link connector above 165000 kHz |

There is no runtime head-to-link mux [DER]: the connector's primary link index
comes from PInS `+0x5c + 4n` byte 1 (`0x11`..`0x14` here, decoded through a
jump table into 0..3), and the source-controller code in bits [4:0] is chosen
from the connector slot number alone.  Connector `n` drives link `n`.

### `0x1c38`/`0x1c3c`: the format companions

Written only by `PhxEnableDVO`, once per link, and only in that link's own
16-bit half [BIN].  Link 0 and 2 own the low half, links 1 and 3 the high half;
the read-modify-write preserves the other half explicitly.  The value is board
data from the clock-conditional PInS entry, decoded in §10:

```
  0x1c38 : pixel clock >  100000 kHz -> 0x11140114    (link 0 half 0x0114, link 1 half 0x1114)
           pixel clock <= 100000 kHz -> 0x111d011d
  0x1c3c : pixel clock >  100000 kHz -> 0x31142114    (link 2 half 0x2114, link 3 half 0x3114)
           pixel clock <= 100000 kHz -> 0x311d211d
```

Per half the layout is `bits[15:12] = link index`, `bits[11:0] = 0x114` above
100 MHz and `0x11d` at or below [DER]; the electrical meaning of that low
nibble is not established [INF].  The condition is evaluated per link with
that link's own clock, so a mixed configuration legitimately produces halves
from both columns.  These words are not derivable from first principles and
must come from the PInS table; `(link << 12) | (khz > 100000 ? 0x114 : 0x11d)`
reproduces this board.

## 9. When re-training is required

The top-level modeset is `H2OCeApplyAndReleaseConfig` at `0x2b62a0`, and it
runs three passes over connectors 0, 1, 2, 3 [BIN]:

```
  teardown (0x2f2ae0)
      per head: LLDP_DisableEncoderOutput (blank) or PhxDisableDacSyncs
      two vsync waits, 0x1c10 &= ~1, 0x1fdc |= BIT(16), 0x1c10 |= 2,
      0x1c00 |= 1, 0x1cac |= 0xf0
  pass A   per connector: blank the encoder, PhxDisableDVOSyncs
                          (link |= 0x002c0000), PhxDisableDisplayController
  pass B   per connector: PhxSetDisplayStart, PhxEnableDisplayController
                          (DC timing and synthesiser), then PhxEnableDVO -
                          the link word, the format companion, 0x1cac, 0x1cb0
  pass C   per connector: PhxEnableTransmitterDevice -> LLDP_Reinitialize,
                          retried up to three times with 100 ms between
                          attempts (0x2dca70, ClientWaitMicroSeconds(100000)
                          at 0x2dcc68, loop bound `cmp $0x2` at 0x2dcc78)
```

So the order is unambiguous and a driver must follow it: **display-controller
timing and clock first, then the GPU link word, companion and pad gate, then
the transmitter reset, initialisation and training** [DER].  The transmitter
samples the video it is fed, so re-timing the controller under a running link
leaves it sending the old attributes.

`LLDP_Reinitialize` has seven call sites, and between them they are the whole
retraining policy [BIN]:

| site | caller | when |
|---|---|---|
| `0x2b3dea`, `0x2b3df9`, `0x2b3e10`, `0x2b3e25` | `CommReExecuteDeviceInitSequences` | connectors 3, 0, 1, 2 - a full device re-init |
| `0x2dcc5b` | `PhxEnableTransmitterDevice` | **mode set**, pass C above, 3 attempts 100 ms apart |
| `0x2e5f6a` | `H2OLibInitLibraryContext` | driver init, right after the `LLDP` object is created |
| `0x2eb4c7` | `LLMonReadValidEdid` | EDID read / monitor detect |

Two paths deliberately do **not** re-train [BIN]:

* **Hot plug**: `MLBoard_HandleHPDEvent` calls `LLDP_HandleInterrupt`, which
  re-initialises and re-trains only for the events that need it (§1), and
  touches no GPU register.
* **DPMS**: `PhxSetMonitorDPMState` tail-calls `PhxDisableDVOSyncs` for a
  digital output, which only sets link bits 18, 19 and 21.  Unblanking goes
  through `PhxEnableDataOutputPath`, which clears bit 18 alone.  Neither
  disturbs the transmitter.

## 10. The VBIOS: the second implementation

`/home/ai/tmp/m9-vbios.rom`, 60 KB, `55 AA`, size byte `0x78` = 61440 bytes
[BIN].  PCIR at `0x0060`: vendor `0x102b`, device **`0x0540`**, class
`0x030000`, indicator `0x80` = last image, so there is **one** image [DER].
All offsets below are file offsets, which equal segment offsets.

Register access from the ROM is the Parhelia-style PCI-config indirect window
(config `0x44` = MMIO offset, config `0x48 + (off & 3)` = data); the helper
entry points for *this* ROM are `wr8 0x5dbc`, `wr16 0x5de2`, `wr32 0x5e08`,
`rd8 0x5e2e`, `rd16 0x5e52`, `rd32 0x5e76` [BIN].  `tools/vbios.py` carries the
Parhelia constants and finds nothing in this image; it needs an M9 profile.

I2C from the ROM is `0x64ca` (write: `CL` = register, `CH` = value, `DL` =
7-bit slave) and `0x655d` (read, same convention) over the bit-banged engine at
`0x5491`, with vtables at `0x4b16` for `0x1c48` (DDC) and `0x4b22` for `0x1c4c`
(MISCIO) [BIN].  `0x52f6` with `DX` = microseconds is the delay [DER].

**The transmitter writes are inline code, not a table** - `mov cl,reg;
mov ch,val; call 0x64ca` triples - which is why a byte search for the register
tables finds nothing [BIN].

### PInS

Near pointer at `0x7ffc` = **`0x7460`** [BIN].  Signature `0x412e`, length
`0x0400`, version **`0x0803`** (PInS 8.3).  Board name at `0x748d` is
`UM9148-E1024LAF`, serial `KFQ19421` at `0x7476`.

| PInS off | file off | bytes | meaning |
|---|---|---|---|
| `+0x0c` | `0x746c` | `44 01` | output-descriptor table start -> `0x75a4` |
| `+0x0e` | `0x746e` | `ac 01` | its end -> `0x7604`, and the patch-table start |
| `+0x44` | `0x74a4` | `44 44 01 00` | memory config, four 4-bit bank codes = 1024 MB |
| `+0x50` | `0x74b0` | `09 0a 0b 0c` | DDC function ids, physical bits 0..3 |
| `+0x54` | `0x74b4` | `0d 0e 0f 10` | DDC function ids, physical bits 4..7 |
| `+0x5c` | `0x74bc` | `00 11 00 42` | connector 0 route, link code `0x11` |
| `+0x60` | `0x74c0` | `00 12 00 42` | connector 1, link code `0x12` |
| `+0x64` | `0x74c4` | `00 13 00 42` | connector 2, link code `0x13` |
| `+0x68` | `0x74c8` | `00 14 00 42` | connector 3, link code `0x14` |
| `+0x6c` | `0x74cc` | `01 02 00 20` | MISCIO function ids, physical bits 0..3 |
| `+0x70` | `0x74d0` | `21 17 22 23` | MISCIO function ids, physical bits 4..7 |

All of the above were read out of the ROM and checked against the decoders the
ROM itself uses [BIN]: `0xde3a` walks `+0x50`/`+0x54` for DDC, applying
`data = BIT(n)` on `0x1c48` with `OE = BIT(n + 16)`; `0xdcd6` walks
`+0x6c`/`+0x70` for MISCIO, `data = BIT(n)` on `0x1c4c` with
**`OE = BIT(n + 10)`**.

So on this board the **transmitter reset lines** are MISCIO function ids
`0x20`, `0x21`, `0x22`, `0x23` at physical bits **3, 4, 6, 7** of `0x1c4c`,
output-enabled by bits 13, 14, 16, 17 [DER].

*Correction to `docs/parhelia-m9.md`*: it describes PInS `+0x0e`/`+0x10` as the
start and end of the register-patch table.  On this ROM `+0x0c`/`+0x0e` are the
start and end of the **output-descriptor** table, `+0x0e` doubles as the patch
table's start, and `+0x10` is zero [BIN].

### The output-descriptor table - where the I2C addresses come from

`0x75a4`..`0x7604`, twelve 8-byte records, verbatim [BIN]:

```
  75a4 1a 11 10 00 10 00 07 00     75d4 1a 13 30 00 10 00 09 00
  75ac 1b 11 10 00 12 00 07 00     75dc 1b 13 30 00 12 00 09 00
  75b4 1c 11 10 00 14 00 07 00     75e4 1c 13 30 00 14 00 09 00
  75bc 1a 12 20 00 10 00 08 00     75ec 1a 14 40 00 10 00 0a 00
  75c4 1b 12 20 00 12 00 08 00     75f4 1b 14 40 00 12 00 0a 00
  75cc 1c 12 20 00 14 00 08 00     75fc 1c 14 40 00 14 00 0a 00
```

Layout, from the decoder at `0xd5ce` [DER]:

| byte | meaning |
|---|---|
| 0 | device class: `0x1a` main page, `0x1b` hot-plug page, `0x1c` PHY page |
| 1 | link code `0x11`..`0x14` = links 0..3 |
| 2 | high nibble = connector index + 1 |
| 3 | flags; `0x00` here, so the record is 8 bytes with no trailing dwords |
| 4-5 | u16 **I2C slave address in 8-bit write form**: `0x0010`, `0x0012`, `0x0014` |
| 6 | DDC bus selector `0x07`..`0x0a` for connectors 0..3 |
| 7 | zero |

That is the clean-room source for the connector to (bus, slave) binding, it is
per board, and it settles the `0x72`/`0x76` question outright: the ROM takes
the addresses from PInS, the X driver takes them from the same board data, and
neither hard-codes them [DER].  The runtime copies live at `cs:0x7fd7` (slave,
four bytes) and `cs:0x7fd3` (bus index, four bytes), with `add dl,0x02` at
`0x91ed` reaching the hot-plug page and `add dl,0x04` at `0x916f` and `0x9517`
reaching the PHY page [BIN].

The ROM's own selector-to-bus map at `0x9c47` handles `0x07` and `0x08` only
and fails on `0x09`/`0x0a` [BIN, reported], so POST can address connectors 0
and 1 and no more - which is consistent with only DP-1 having a picture.  The
PInS data for all four connectors is present; the ROM's table is what stops at
two.

### The register-patch table

`0x760c`, entries `u16 reg; u8 flags; u8 clksel;` followed by `u32 data` if
`flags & 1`, `u32 mask` if `flags & 2`, `u32 alt` if `flags & 4`; stride
`4 + 4 * popcount(flags & 7)`; terminated by a zero dword at `0x779c` [DER].
It decodes cleanly to 47 entries.  The display-relevant ones are quoted in §8;
the rest are the `0x0f04`..`0x0f60` PLL/PHY trims and a `0x8000`-prefixed
address space of 21 entries whose meaning is not established here.

### The VBIOS bring-up, `0x906e`

This is the ROM's whole DisplayPort path, and it is the same shape as
`LLDP_Reinitialize` [DER]:

```
 1.  HMC acquire (0x956d): 0x0318 &= 0x8fffffff, poll 0x03b0 bit 1 for 0
 2.  select the connector's bit-banged bus (0x4c56) from cs:0x7fd3
 3.  call 0x9102          /* main-page init, slave 0x10                  */
 4.  call 0x916a          /* PHY-page init then main DP-mode table       */
 5.  call 0x94f9          /* PHY-page 0x26 bit-0 strobe, 2 x 1200 us     */
 6.  call 0x91e8          /* detect -> CL = 0, 1, 2 or 3                 */
 7.  read main 0x00; write |= 0x08; write &= ~0x08     /* soft reset     */
 8.  if (CL == 3):        /* DisplayPort                                  */
         call 0x9369      /* PLL / bridge settle                          */
         call 0x926b      /* LINK TRAINING                                */
 9.  write main 0x0a = 0x00       /* un-blank                             */
10.  write main 0x00 = 0x00
11.  HMC release (0x954a): 0x0318 |= 0x60000000 then &= 0xefffffff
```

An outer routine at `0x93b8` runs the transmitter reset first [BIN]:

```
  HMC acquire
  call 0x93fa                      /* transmitter reset, see below       */
  resolve the connector's slave and bus into cs:0x7fd7 / cs:0x7fd3
  call 0x901f                      /* init without training              */
```

### Transmitter reset over MISCIO, `0x93fa`

```
  if MISCIO function 0x08 exists:
      drive 0x08 high
      drive 0x1c low ; wait 210 us ; drive 0x1c high
      drive 0x08 low
  else:                            /* this board: 0x08 is absent         */
      drive function 0x20 low
      wait 210 us
      drive function 0x20 high
```

Driving is push-pull through `0x4fe7`: set the data bit in `0x1c4c` and the
output-enable bit `n + 10` [DER].  The literal `0x20` means the ROM resets
**connector 0 only**; ids `0x21`, `0x22`, `0x23` are published in PInS for the
other three and are unused by POST [BIN].

### `0x9102` - main-page init (slave `0x10`)

```
  0x0a = 0x00   0x24 = 0x01   0x20 = 0x21   0x00 = 0x22
  0x74 = 0x00   0x77 = 0xb2   0x78 = 0x10   0x72 = 0xa9
  0x27 = 0x30   0x30 = 0x32   0x71 = 0x20   0x2f = 0x82
  0x86 = 0xf7   0x02 = 0xb6
```

This is the X driver's `LLDP_InitCommon` plus `LLDP_InitPowerOn` merged, with
three deltas [BIN]: `0x78` is seeded `0x10` here and `0x30` there, `0x86` is
`0xf7` here and `0xf0` there, and the ROM never writes `0x8b`.

### `0x916a` - PHY page then main DP-mode table

`0x916a` writes the PHY page (slave `0x14`) and then **falls through** into
`0x9195`, returning at `0x91e7`; they are one routine [BIN].

```
  page3 0x00 = 0x18   0x03 = 0x06   0x24 = 0x10   0x25 = 0x04   0x26 = 0x04
  main  0x2c = 0x00   0x2d = 0x00   0x2e = 0x0c
        0x76 = 0xff   0x78 = 0x33   0x75 = 0xf8   0x75 = 0x00
        0x87 = 0x74   0x5f = 0x0c   0x71 = 0x20   0x5e = 0x02
```

This is `LLDP_InitInDisplayPortMode` with the page order reversed and two
deltas: the ROM writes `0x87 = 0x74` once where the driver writes `0x70` first
and `0x74` last, and it seeds `0x5e = 0x02` where the driver seeds `0x04`
[BIN].  Every value here is one of the differential values measured on the
POST-trained connector, `0x87 = 0x74` and `0x5f = 0x0c` included [MEASURED],
which is what proves POST runs this routine.

### `0x9369` - PLL and bridge settle

```
  for (i = 0; i < 0xffff; i++):
      read main 0x76
      if (bit 0) break
      if (bit 3):
          write 0x76 = 0x08
          write 0x75 = 0x80 ; write 0x75 = 0x00      /* PLL reset */
          wait 1200 us
  write main 0x76 = 0x01
```

Same shape as `LLDP_WaitDPCDReadFinished`, and it supplies the delay the X
driver's version does not show: **1200 us after a PLL reset** [BIN].

### `0x926b` - link training, six attempts

```
  attempts = 6
  write main 0xac = 0xe4
  read  main 0xaa -> v ; write main 0xaa = v & 0xf0
  write main 0x5c = 0x10
  write main 0x60 = 0x00
  read  main 0x52 -> lanes
  write main 0x5e = lanes                   /* raw byte, bit 7 preserved */
  if ((lanes & 0x0f) & 0x04):
      rate = 0x06                           /* four lanes -> force RBR   */
  else:
      read main 0x51 -> m
      rate = ((m & 0x0f) == 0x0a) ? 0x0a : 0x06
  write main 0x5d = rate
  write main 0x76 = 0xff
retry:
  read  main 0x5f -> t ; write main 0x5f = t | 0x01      /* start        */
  poll  main 0x76 bit 1, up to 0xfff reads
  write main 0x8f = 0x04 ; read main 0x8d -> s
  if (s & 0x40) goto done                                /* success      */
  if (--attempts == 0) goto done
  write main 0x76 = 0x02 ; read main 0x76 -> q
  if (!(q & 0x08)) goto retry
  wait 1200 us
  read  main 0x5f -> t ; write main 0x5f = t | 0x01
  write main 0x76 = 0x0b
  wait 1200 us
  goto retry
done:
  write main 0x76 = 0x02
```

Differences from the X driver's `LLDP_PerformLinkTraining` [DER]:

| | X driver | VBIOS |
|---|---|---|
| attempts | 3 inner x 3 outer | 6, no re-initialisation |
| `0x5c` | `0x00` for a native sink, `0x10` behind a dongle | always `0x10` |
| rate policy | pixel clock against the lane/rate table | four lanes force 1.62 Gbps, otherwise whatever `0x51` allows |
| start | `0x5f` bit 0 cleared then set | `0x5f` bit 0 ORed in |
| success | `(0x8d & 0x70) == 0x40` | `(0x8d & 0x40)` |
| verification | sink lane status **and** DPCD `0x00100` read back over AUX | none |
| AUX | used | **never**: no access to `0x7d`..`0x84` exists anywhere in the ROM [BIN], and no DPCD address constant appears in it |

The two implementations agree on everything that matters: the rate and lane
registers, the `0x5f` bit-0 start, the `0x76` bit-1 completion, the `0x8f`/
`0x8d` master status, and the absence of any per-phase clock-recovery or
channel-equalisation loop.  **The training state machine is in the
transmitter.**

### `0x922b` - and one disagreement worth recording

`0x922b` writes `0x74 = 0x20`, `0x74 = 0xa0`, `0x0a = 0x30` [BIN].  It has no
caller in the ROM [BIN].  The `0x74` pair is byte-identical to the X driver's
`LLDP_DisableEncoderOutput`, and `0x0a = 0x30` sets both the bit the driver
sets to blank and the bit it sets to mute, with the DisplayPort mode bits
cleared, so this reads as **blank**, not as output enable [DER].  The VBIOS
bring-up's own un-blank is `0x0a = 0x00` at step 9 of `0x906e` [BIN].

## 11. Main register `0x0a`: two gate bits, only one of them destructive

An earlier revision of this note claimed bit 4 gated the AUX channel.  **That
is withdrawn.**  The binary and the board together give a different and better
answer.

### What the binary does

`LLDP_Enable` / `LLDP_DisableEncoderOutput` share one core at `0x2dcd00`, and
its two branches are the whole encoding [BIN]:

```
  blank   (argument 0):   0x0a = (read 0x0a & ~0x20) | 0x10
  unblank (argument 1):   0x0a =  read 0x0a & ~0x30
```

The blank sets bit 4 and **explicitly clears bit 5** - it is careful never to
leave bit 5 set.  `LLDP_InitPowerOn` writes `0x0a = 0x0c`, both gate bits clear,
and the VBIOS writes `0x0a = 0x00` in the same place [BIN].

The only place either vendor implementation writes bit 5 is the fault path in
`LLDP_HandleInterrupt`, on interrupt-status bit 3 [BIN]:

```
  read 0x0a ; write 0x0a |= 0x30      /* both gates                 */
  write 0x75 = 0x80 ; write 0x75 = 0x00   /* PHY PLL reset           */
  unblank: 0x0a &= ~0x30
```

Note the PLL reset **between** setting and clearing.  The handler does not call
`LLDP_PerformLinkTraining` on that path; it falls through to the remaining
status bits [BIN].

### What the board does

[MEASURED] on connector 0, reading the mirrors and issuing a real AUX
transaction after each write:

| `0x0a` | `0x51` | `0x52` | `0x63` | `0x65` | AUX |
|---|---|---|---|---|---|
| `0x00` | `14` | `c4` | `77` | `01` | ok |
| `0x10` | `14` | `c4` | `77` | `01` | **ok** |
| `0x30` | `14` | `c4` | `00` | `80` | fails |
| `0x00` | `14` | `c4` | `00` | `00` | ok, but the link is gone |

So **bit 4 blanks the stream and leaves AUX and the link alone; bit 5 kills
both, and clearing it again does not bring the link back** - the lane-status
mirror stayed zero until the next full bring-up.

### Reconciling the two

They agree, once the roles are separated [DER]: bit 4 is the ordinary stream
blank and bit 5 is a link-destroying gate, which is exactly why the vendor's
blank clears bit 5 while setting bit 4, and why the only code that sets bit 5
is a fault path that pulses the PHY PLL before letting go of it.

Two consequences for a driver:

* **Blank with `0x10`, never with `0x30`.**  `0x30` requires a re-initialisation
  and a re-train afterwards [MEASURED].
* The recovery the vendor pairs with `0x30` includes a PLL reset that a plain
  `0x30` -> `0x00` does not.  Whether `0x0a = 0x30`, `0x75 = 0x80`,
  `0x75 = 0x00`, `0x0a = 0x00` restores the link without a full bring-up is
  **not established** [INF], and is three writes to find out.

### The predicate at `0x2dd990`, and why its name is withdrawn

The routine is four instructions and the bit is not ambiguous [BIN]:

```
  2dd9ca:  call  <i2c read>            /* main register 0x0a */
  2dd9cf:  0f b6 44 24 0f   movzbl 0xf(%rsp),%eax
  2dd9d4:  c1 e8 04         shr    $0x4,%eax        <- bit 4, not bit 5
  2dd9d7:  83 f0 01         xor    $0x1,%eax
  2dd9da:  83 e0 01         and    $0x1,%eax
```

It returns the inverse of **bit 4**.  Its only caller, the output constructor at
`0x2e5fc6`, skips `LLDP_Reinitialize` when it returns non-zero, so it is a
"this output is already live, do not disturb it" test over the blank bit [DER].

I previously named it `LLDP_IsAuxChannelEnabled` on the grounds that it was the
only unassigned routine name of that shape.  That argument does not hold: after
every behaviourally-certain identification there are **four** unassigned names
(`Enable`, `DisableEncoderOutput`, `IsAuxChannelEnabled`, `ResetPll`) and
**five** unassigned functions (`0x2dcd00`, `0x2dcdb0`, `0x2dd690`, `0x2dd990`,
`0x2dea10`), so one of the five is a static helper with no event name and the
pigeonhole gives nothing.  Combined with the board showing bit 4 not gating
AUX, the name assignment is withdrawn [INF retracted].

If this routine *is* `LLDP_IsAuxChannelEnabled`, then the binary and the board
disagree about which bit that name refers to, and the binary says bit 4
unambiguously.  Recorded as a disagreement, not reconciled.

### What the earlier symptom actually was

The eight registers that read zero on a probed board - `0x51`, `0x52`, `0x53`,
`0x5c`, `0x63`, `0x64`, `0x65`, `0x8d` - are all AUX-derived, and `0x0a = 0x30`
is exactly what kills them.  `0x8d` reading `0x04` on the trained connector was
a blanked-state artefact; with the link healthy it reads `0x44`, and
`(0x8d & 0x70) == 0x40` holds exactly as recovered in §3 [MEASURED].  The
trained state reads `0x63 = 0x77`, `0x64 = 0x77`, `0x65 = 0x81`, which is the
`GetLinkStatus` predicate of §3 satisfied.

## 12. Implementation plan for `drivers/gpu/drm/m9x`

The kernel driver already has the bus: `m9_ddc.c` owns four bit-banged
adapters, the board-controller request/grant handshake, the PInS map, and
`m9_tx_read`/`m9_tx_write`/`m9_tx_read_hpd` against a probed `tx_addr`.  What
is missing is the transmitter state machine.

**New files**

* `m9_dp.c` - the transmitter driver: register tables, AUX, training, detect.
* `m9_dp.h` - the register names and the `struct m9_dp_tx` interface.

**Changes to existing files**

* `m9_drv.h`: add `struct m9_dp_tx` and embed one **per head**, not one global.
  It needs `struct m9_ddc_bus *bus`, the three 7-bit addresses, `u8 state`,
  `u8 lanes`, `u8 rate`, `bool trained`, and a mutex.
* `m9_ddc.c`: extend `m9_tx_probe()` to record `tx_addr + 1` and `tx_addr + 2`,
  and export `m9_tx_read`/`m9_tx_write` with an explicit page argument so
  `m9_dp.c` can reach pages 2 and 3.  Move the bridge open/close in
  `m9x_ddc_read_edid()` onto `m9_dp_ddc_request()`/`m9_dp_ddc_release()` so the
  `0x76` bit-3 PLL check happens.
* `m9_kms.c`: call `m9_dp_enable(head, mode)` from the pipe's enable hook,
  **after** `m9x_mode_program()` has programmed the timing and the link
  registers, and `m9_dp_disable(head)` from the disable hook.  Both take the
  head, so the path is per-head from the first version.
* `m9_reg.h`: keep `M9_DP_TX_MAIN`/`M9_DP_TX_BRIDGE`/`M9_DP_TX_HPD`, move the
  rest of the transmitter map into `m9_dp.h`.
* `Makefile`: add `m9_dp.o`.

**Data-driven, not a fixed payload**

Every table in §2 becomes a `static const struct m9_dp_reg { u8 reg, val; }`
array walked by a loop, with its length from `ARRAY_SIZE`.  Lane count and link
rate come from the sink at run time; the pixel-clock table is a `u32[2][4]`
indexed by rate and lane index.  Nothing is a fixed-size copied blob.

**Ordering inside the modeset**

The vendor's order is fixed and a driver must keep it (§9):

```
  teardown            0x1cac |= 0xf0, controllers on hold
  per connector       PhxDisableDVOSyncs: link |= 0x002c0000
  per connector       DC timing and synthesiser  (m9x_mode_program)
  per connector       link word, format companion, 0x1cac, 0x1cb0
  per connector       transmitter reset, init, DisplayPort mode, training
  per connector       clear link bit 18, clear 0x1cac bit 4 + link
```

The transmitter samples the video it is fed, so re-timing the controller under
a running link leaves it sending the old attributes; the last two steps are
what the driver repeats on every mode set.  The vendor retries the whole
transmitter stage up to three times with 100 ms between attempts.

A failed training must leave the transmitter blanked rather than the pad open,
so a dark head never feeds a half-locked link - except on a connector the
firmware already trained, which must not be taken down by a training attempt
that could not run.

**One hazard in the enable path as it now stands.**  The common bring-up writes
`0x0a = 0x30` before the PLL pulse and the final `0x0a = 0x00`, which is the
vendor's fault-recovery shape and is safe *because training follows it*.  But
bit 5 destroys a link that was already up (§11), so that write commits the
driver to a successful re-train on every enable, including on the connector the
firmware trained.  Either keep training reliable on that path, or take the
`0x30` out and blank with `0x10` alone, which leaves a working link intact.

**Two pieces the LLDP module does not contain, and that the driver still needs**

* **Transmitter reset over MISCIO**, from the VBIOS (§10): PInS `+0x6c`/`+0x70`
  publish function ids `0x20`..`0x23` for connectors 0..3 at `0x1c4c` physical
  bits 3, 4, 6 and 7, output-enabled by bits 13, 14, 16 and 17.  Drive the bit
  low, wait 210 us, drive it high, before the first init of that connector.
  `m9_ddc.c` already parses the PInS DDC map at `+0x50`; the MISCIO map at
  `+0x6c` is the same eight-byte, physical-bit-ordered format.
* **The pad gate**, `0x1cac` bit `4 + link` (§8): clear it after that link's
  transmitter is initialised and unblanked, and set it as part of
  the teardown at the top of a modeset, where the vendor writes `old | 0xf0` to
  park all four.  POST leaves three of the four bits set, and that is the whole
  reason three connectors are dark.  The low nibble is an input whose meaning is
  not established (§8); nothing should gate on it.

**Three deltas against the bring-up already in `m9_ddc.c`**

The common-init tables there are correct, but the DisplayPort part is missing
and one value differs [DER]:

1. The **DisplayPort mode table** of §2 is absent: `0x2c`, `0x2d`, `0x2e`, the
   `0x75 = 0xf8` / `0x00` PHY pulse, `0x87 = 0x70` then `0x74`, `0x5f = 0x0c`
   and `0x71 = 0x20`.  `0x87` reads `0x74` on the POST-trained connector and
   `0xb0` on the others [MEASURED], so its absence is visible on the board.
2. The page-3 table there is the pre-reset one; the DisplayPort mode table's
   page-3 half additionally writes `0x00 = 0x18` and `0x03 = 0x06`, both of
   which are in the measured differential.
3. `0x77` is written `0xa3`; the VBIOS writes `0xb2` and the POST-trained
   connector reads `0xb2` [MEASURED].  `0xa3` is the X driver's other branch,
   selected by a board config item.  Both enable the AUX-done source, so this
   is a small difference, but `0xb2` is what this board's firmware chooses.

`m9_dp.c` writes all three as part of entering DisplayPort mode, so the common
path in `m9_ddc.c` need not change.

## A connector POST never drove is held in reset

MEASURED 2026-09-18 on the **two-port** `128-E1024LAF` (serial `KFP35758`), which
is a different board from the four-port `UM9148-E1024LAF` above.

Its second connector answered **nothing at all** on its DDC bus - not the sink at
`0xa0`, not the transmitter at `0x10`/`0x12`/`0x14`/`0x16` - while the first
connector answered all four transmitter pages.  Probing all four conventional bit
pairs (clock `2n`, data `2n+1`) found devices only on bits 0/1, so the pins were
never the problem and the PInS map is right.

The cause is the transmitter reset line.  `0x1c4c = 0x0001007b`: MISCIO function
`0x20` (connector 0) sits at bit 6 with its **output enable set and the bit
high**, while function `0x21` (connector 1) at bit 7 is **undriven and reads
low** - that transmitter is held in reset, so it cannot answer I2C.  Pulsing the
line and then *restoring* the register word, as `m9_tx_reset()` did, drops an
undriven line straight back into reset before any probe can run.  Leaving it
**driven high** after the pulse is what releases it:

```
connector 1 transmitter at 0x10, hotplug 0x07   (was: no transmitter answered)
DP-2 connected, EDID 1920x1200
```

So "no transmitter answered" on a connector POST did not touch is a reset-line
state, not a missing device or a wrong bus.

**Still open on that board:** with the transmitter released, head 1's controller
rasters at the programmed rate and `LLDP` training reports success, but the
transmitter's main-stream attributes read zero - no video is reaching it.
Neither routing link 1 nor link 2 from controller 1, nor clearing the
corresponding `0x1cac` pad-gate bits, changed that.  Note also that `0x1cac`
**does** accept writes in this state (`0x81` was written and read back), which
contradicts the earlier "latches `0x00000001`" measurement in
`drivers/gpu/drm/m9x/m9_mode.c`; that behaviour is evidently state dependent.

This board's own output-descriptor table (PInS `+0x0c`..`+0x0e`, format in §10)
decodes to six records rather than twelve:

```
  75a4 1a 31 10 00 10 00 07 00     75bc 1a 33 20 00 10 00 09 00
  75ac 1b 31 10 00 12 00 07 00     75c4 1b 33 20 00 12 00 09 00
  75b4 1c 31 10 00 14 00 07 00     75cc 1c 33 20 00 14 00 09 00
```

Byte 2 high nibble gives connectors 0 and 1, byte 6 gives DDC bus selectors
`0x07` and `0x09` - and the selector-to-pin-pair relation is `pair = selector + 3`,
so `0x07` is bits 0/1 and `0x09` is bits 4/5, exactly the two pairs PInS `+0x50`
names (`09 0a 00 00 0d 0e 00 00`).  Byte 1 is `0x31`/`0x33` where the four-port
board has `0x11`..`0x14`; if its low nibble is still the link code + 1, this board
routes connector 1 to **link 2**, not link 1, which the driver's
connector-n-to-link-n assumption does not express.  That reading is DERIVED and
was **not** confirmed: writing link 2 with controller 1 as its source did not make
the transmitter send.

**Connector to bus and slave, from the board rather than by probing**

PInS `+0x0c`/`+0x0e` bracket the output-descriptor table (§10), whose records
give, per connector, the three 8-bit slave addresses and the DDC bus selector.
Parsing it replaces `m9_tx_candidates[]` with board data and removes the last
hard-coded address from the driver.

**The shortest hardware test**

Connector 2 is now transmitting a real stream after a full vendor
initialisation plus `0x00`, `0x77`, `0x5d`, `0x5e`, `0x5f` copied from the
trained connector and the pad gate cleared [MEASURED].  Those copied values
make the result unrepeatable on a different sink, so the test that matters is
whether training *derives* them.  With a sink on connector 2:

1. `0x0a = 0x00` (never `0x30`, §11) and `0x77 = 0xb2` on connector 2's main
   page, then read `0x52`.  A non-zero lane count hands you the real lane count
   in one read.  If it is zero, add the prologue `0xac = 0xe4`,
   `0xaa &= 0xf0` and read again.
2. With that lane count, run §3 steps 12-16 once, choosing `0x5d` from the
   lane/rate table in §3 rather than copying it: write `0x5d`, `0x5e`,
   `0x60 = 0`, `0x76 = 0x02`, then `0x5f` bit 0 cleared and set.
3. Poll `0x76` for bit 1.  If it sets, write `0x8f = 0x04` and read `0x8d`;
   `(v & 0x40)` is the transmitter's own success flag.
4. Clear `0x1cac` bit 6 (pad 2) and read main `0x10`..`0x1f`.  Main-stream
   attributes matching head 2's programmed mode is the pass, and needs no
   monitor.

Step 1 alone is two writes and one read and settles the largest open question.
Steps 2 and 3 are the first execution of `PerformLinkTraining` with values
derived from the sink rather than copied, which is the whole point.
