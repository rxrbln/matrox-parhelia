<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# Matrox Parhelia DDC / EDID / I2C — complete GPIO register map

Reverse-engineered from the vendor HAL (`mtx_drv.so`, 7_4_99 build) and the card
VBIOS, cross-checked against the working native driver `src/parhelia_ddc.c`.
Tags: **CONFIRMED** = derived from disassembly *and* HW-validated in the driver;
**PROBABLE** = derived from disassembly only (not yet exercised on this board).

## Anchors (vendor binaries)

Disassembly `scratchpad/full99.asm` (= `mtx_drv.so`, dynamic symbols present as
`<sym@@Base+0xNN>` comments; local bodies show as `mtxNNNN`). VBIOS in
`scratchpad/vbios.asm` (= `/srv/t2/xf86-video-parhelia/vbios-matrox-parhelia`).

| Function                       | addr     | role |
|--------------------------------|----------|------|
| `HSLPARI2COpenPort`            | 0xc1c30  | selects reg + masks per port id; fills the port object |
| `HSLPIN60GetDDCIORegMasks`     | 0xda420  | pin-id → (data mask, OE mask) from the PInS table |
| `HSLPARGetDDCIORegMasks`       | 0xda4d0  | DDC-line selector → SCL/SDA masks (uses PInS+0x1a8) |
| `HSLPARI2CGenericRead`         | 0xc2360  | release-OE + read-back one line |
| `HSLPARI2CGenericWrite`        | 0xc24b0  | drive/release one line, SCL clock-stretch poll |
| `HSLPARI2CGenericRead/WriteSCL/SDA` | 0xc2410/0xc2460/0xc25f0/0xc2640 | per-line thunks (pick mask pair from the port object) |
| `HALPI2CStart` / `HALPI2CStop` / `HALPI2CWaitAck` | 0x99300/0x993f0/0x98e80 | start/stop/ack bit sequencing |
| `HALGetEdidBlockContent`       | 0x94f80  | reads one 128-byte EDID block (base 256 B only) |
| `HALPReadDDCByte`/`HALPReadSlowDDCByte` | 0x3b470/0x3a1b0 (plt) | fast vs slow EDID byte read |
| `HSLPAREnableCEMDDCSel2`        | 0xa6ef0  | external CEM/CPLD DDC mux (board-specific) |
| VBIOS DDC bit-bang             | vbios 0x4dec–0x4eed | same 0x1c48 register, same OE<<16 |

---

## 1. The DDC GPIO register 0x1c48 — bit map (CONFIRMED)

Single 32-bit MMIO register `PARHELIA_REG_DDC = 0x1c48`, common to every Parhelia
variant (original 512/8x, LX, P690; M9 also uses 0x1c48 for its bit-bang path —
`src/parhelia_m9_dp.c`). Open-drain lines are emulated with a **data bit** and an
**output-enable bit** 16 positions higher:

```
bit  31 ............ 24 23 ........ 16 15 ........ 8 7 ......... 0
      [ reserved       ] [ OUTPUT-ENABLE ] [ INPUT read ] [ OUTPUT data ]
                          per pin: OE = 1<<(pin+16)   pin = 0..7
```

Per DDC line, given its GPIO `pin` (0..7):

| field           | mask            |
|-----------------|-----------------|
| output data     | `1 << pin`      |
| output enable   | `1 << (pin+16)` |
| input readback  | `1 << pin`      (same low-byte bit; see §4) |

Open-drain semantics (**CONFIRMED**, `HSLPARI2CGenericWrite` @0xc24b0):
- **drive low**: set OE (`|= 1<<(pin+16)`), clear data (`&= ~(1<<pin)`).
- **release high**: clear OE (`&= ~(1<<(pin+16)`)); the board pull-up raises the
  line; the level is then *read back* on `1<<pin`.

This is exactly what `parhelia_ddc_putbits()` does. The vendor never sets a data
bit to 1 to make a line high — high is always "release OE and let the pull-up win".

### Two GPIO banks in one register (PROBABLE)

The VBIOS exposes a **second** pin bank in the high data byte (bits 8..15). Its
line routines (vbios 0x4e0e, 0x4ec4) take a high-byte mask and compute OE via
`and eax,0xff00; shl eax,8` — i.e. data bit `8+k` → OE bit `16+k`. So bits 8..15
are a second 8-line group sharing the **same** OE nibble (16..23) as bits 0..7.
On the tested integrated boards both DVI connectors live in the low byte
(pins 0..3), so this bank is unused there; it is the likely home of a secondary
DDC / MISC-IO-expander channel on boards that populate it. **Not exercised on HW.**

---

## 2. Pin-id → GPIO-bit table (CONFIRMED)

The physical pin number is *not* fixed; it comes from a packed 4-byte
**pin-id table** in the VBIOS PInS. `HSLPIN60GetDDCIORegMasks` (0xda420):

1. `pins_rev = PInS[0x04]` (word). If `pins_rev > 0x609` → table = `PInS[0x1b4]`,
   else → hardcoded default `0x05070608`.
2. For the requested logical pin-id, scan the 4 bytes of the table; the **byte
   index i (0..3)** where the id is found is the GPIO bit.
3. `data_mask = 1<<i`, `OE_mask = (1<<i) << 16`.

Default table `0x05070608` unpacks (LSB byte0 first) to ids `{8,6,7,5}` at bits
`{0,1,2,3}`. The driver reimplements this verbatim in `pins_id_to_bit()` /
`ParheliaPinsDDC()` (`src/parhelia_pins.c`, reads `PInS+0x1b4`).

The higher-level line→pin resolver `HSLPARGetDDCIORegMasks` (0xda4d0) additionally
reads a **DDC-line assignment word `PInS[0x1a8]`** (gated on `pins_rev > 0x608`)
and maps up to **four DDC lines** (selector bits `&1/&2/&4/&8`) to their SCL/SDA
pin-ids. The driver does not consult 0x1a8 (it hardcodes the SCL/SDA candidate
id pairs instead) — see §11.

### Resolved pin pairs on known boards (CONFIRMED on HW)

| board                 | DVI-1 (head 1)     | DVI-2 (head 0)     |
|-----------------------|--------------------|--------------------|
| P690 / Parhelia-LX    | SCL pin 1, SDA pin 0 | SCL pin 3, SDA pin 2 |
| original Parhelia-512/8x | SCL pin 3, SDA pin 1 | SCL pin 2, SDA pin 0 |

---

## 3. Port-id → register / DDC-line map (from `HSLPARI2COpenPort` @0xc1c30)

`OpenPort(dev, portId)` switches on `portId` and writes into the port object:
- `+0x4164` = MMIO register offset (0x1c48 for the DDC lines)
- `+0x4168 / +0x416c` = SCL data / SCL OE mask
- `+0x4170 / +0x4174` = SDA data / SDA OE mask
- `+0x4148` = 1 when the port is valid/enabled

| portId        | action | reg | line sel (→ `GetDDCIORegMasks`) |
|---------------|--------|-----|---------------------------------|
| in {2,3,4}    | native DDC | 0x1c48 | 0 |
| 5             | native DDC | 0x1c48 | 1  *(the driver's "port 5" reference)* |
| 6..0x11       | native DDC | 0x1c48 | 2 |
| 0x12          | native DDC | 0x1c48 | 3 |
| 1             | CEM/CPLD or MISC-IO expander (board-gated) | **0x1c4c** or MISC-IO | — |
| 0x10          | MISC-IO expander (feature bit `board[0x88] bit3`) | — | — |
| 0x85, 0x92    | external CEM DDC mux → `HSLPAREnableCEMDDCSel2` | — | — |

So all four native DDC lines share **register 0x1c48**; only the pin pair differs
(via the pin-id table §2). This is why the driver probes several pin pairs on the
one register.

---

## 4. Read path (CONFIRMED, `HSLPARI2CGenericRead` @0xc2360)

To sample a line level the vendor:
1. `val = read(0x1c48)`
2. `val &= ~OE_mask` ; `write(0x1c48, val)`  — release/tristate the line
3. `val = read(0x1c48)` ; `*out = (val & data_mask) != 0`  — sample same bit

The driver's `parhelia_ddc_getbits()` samples `data_mask` directly without
touching OE, relying on `putbits(1,1)` having already released — functionally
equivalent because the server always releases before reading.

---

## 5. Write path + clock stretching (CONFIRMED, `HSLPARI2CGenericWrite` @0xc24b0)

Args: `(dev, reg, OE_mask, data_mask, wait_high_flag, level)`.
`wait_high_flag = 1` for **SCL**, `0` for **SDA** (set in the SCL/SDA thunks).

- **level 0 (drive low):** `val = (read & ~data_mask) | OE_mask; write`; then a
  short settle loop reading the register up to **10** times.
- **level 1 (release high):** `val = read & ~OE_mask; write`. If `wait_high_flag`
  (SCL only): poll the SCL readback in a loop —
  `ClientWait(10); GenericReadSCL(); repeat while line==0`, up to **0x270f (9999)**
  iterations, then `SetError(timeout)`.

**=> Clock stretching IS handled**: after releasing SCL the vendor waits for the
slave to let the clock rise (bounded ~10000×`ClientWait(10)`). SDA release does
not wait. The current driver relies on libI2C's fixed `RiseFallTime`/`HoldTime`
(20/40 µs) and does *not* poll SCL for stretch — adequate for the tested
monitors but see §11 for a robustness upgrade.

---

## 6. Start / Stop / Ack sequencing (CONFIRMED)

Delays use `ClientWait(delay)` where `delay = timing[+0x14]` is the per-bus
half-bit period (client-provided; the same knob the driver sets via the libI2C
`*Time` fields). Sequences:

**Start** (`HALPI2CStart` @0x99300):
```
SetSDA(1); GetSDA()->if low, flag "bus busy" (err code 2)
ClientWait(d); SetSCL(1); ClientWait(d)
SetSDA(0)  ; ClientWait(d)         ; SDA↓ while SCL high  = START
SetSCL(0)                          ; then pull clock low
```
**Stop** (`HALPI2CStop` @0x993f0):
```
SetSCL(0); SetSDA(0); ClientWait(d)
SetSCL(1); ClientWait(d)
SetSDA(1)                          ; SDA↑ while SCL high   = STOP
```
`HALPI2CWaitAck` @0x98e80 releases SDA, clocks SCL, and samples SDA for the
low ACK. Standard bit-bang order, one `ClientWait(d)` between every transition.

---

## 7. E-DDC / segment pointer 0x60 (CONFIRMED: NOT implemented)

`HALGetEdidBlockContent` (0x94f80) computes the read offset as
`block_index << 7` (0x80-byte granularity) and reads 0x80 bytes per call with a
**single-byte** word offset to slave **0xA0**. There is **no write to segment
slave 0x60** anywhere on this path. Consequences:

- Block 0 (0x00) and block 1 (0x80) — i.e. the **base 256 bytes** — are read
  (EDID 1.x base + one extension block). This covers the common CEA-861 case.
- EDID > 256 bytes (segment ≥ 1, multiple CEA blocks) is **not** reachable; the
  vendor never issues the E-DDC segment-pointer write.
- A **slow** variant `HALPReadSlowDDCByte` (0x3a1b0) exists as a fallback for
  monitors that need relaxed timing (retry path in the block reader also inserts
  `ClientWait(0x1f4)` = 500 units between attempts).

The X server's `xf86DoEDID_DDC2` used by the driver *does* implement segment
addressing itself, so the native driver can already exceed the vendor here if the
line timing holds — nothing extra to reverse for E-DDC.

---

## 8. Second connector / analog-VGA DDC (CONFIRMED)

There is **no separate I2C register per connector** on the integrated boards: the
second DVI and the analog VGA connector are additional **DDC lines on the same
0x1c48**, distinguished only by their SCL/SDA pin pair (pin-id table §2, resolved
per line by `PInS[0x1a8]`). This is why `ParheliaDetectOutputs()` iterates two pin
pairs on one register. The analog VGA (DVI-I / HD15) monitor answers DDC on its
line exactly like a DVI monitor; digital-vs-analog is then decided by the EDID
Video-Input bit + RAMDAC load-sense, not by a different DDC bus.

Register **0x1c4c** is the **CEM / CPLD 2-line I2C** (only `portId==1` under a
board condition; masks: SCL `0x800`/OE `0x400`, SDA `0x2`/OE `0x1`). External CEM
boards additionally gate a DDC mux via `HSLPAREnableCEMDDCSel2` (drives the CPLD
"special-purpose MISC-IO"). Neither is present on the tested integrated card —
0x1c4c is *not* a second native DDC bus for these boards (it earlier looked like
the SiI164 CEM bus in MEMORY notes; confirmed here as the CEM/CPLD path).

---

## 9. Hotplug / load-sense (CONFIRMED: no HPD interrupt for DDC)

There is no DDC-associated hotplug/HPD interrupt register on this path. Presence
is polled: EDID answer over DDC, plus **RAMDAC load-sense** for analog (regs
0x1c14 / 0x188c / 0x1910, see `docs/parhelia-analog-vga-*`). Detection is a
PreInit-time probe, not interrupt-driven.

---

## 10. VBIOS DDC path (CONFIRMED, cross-validation)

The VBIOS bit-bangs the **same register 0x1c48** (`mov si,0x1c48` at vbios
0x4df1/0x4e13/0x4e38/0x4e61/0x4e92/0x4ec4). Its line helpers confirm the
open-drain OE encoding independently:
- low-byte line: `and ax,0xff; shl eax,16; not; and ecx,eax` → **OE = data<<16**.
- high-byte line: `and eax,0xff00; shl eax,8` → data bit `8+k` → OE bit `16+k`.

Register access is indirect through the VBIOS's PCI-config aperture (index 0x44 /
data 0x48), irrelevant to the native driver's direct MMIO. The VBIOS serves only
the **primary** connector's DDC, which is the original reason the native path was
written — it reads *both* connectors.

---

## 11. Gaps vs. the current driver + recommendations

The driver (`src/parhelia_ddc.c` + `src/parhelia_pins.c`) is a faithful, HW-proven
reimplementation. Residual, low-risk robustness items:

1. **PInS revision gate not honored.** The HAL uses `PInS[0x1b4]` only when
   `PInS[0x04] > 0x609`, else the hardcoded default `0x05070608`; the driver reads
   `PInS[0x1b4]` unconditionally. Harmless today (a bogus/zero table makes
   `ParheliaPinsDDC` return FALSE → falls back to the hardcoded pairs), but adding
   the `>0x609` gate would match the vendor exactly.
2. **Line assignment word `PInS[0x1a8]` unused.** For full portability the
   per-line SCL/SDA resolution could read 0x1a8 (rev `>0x608`) as the HAL does,
   instead of the hardcoded candidate id pairs.
3. **SCL clock-stretch poll absent.** libI2C uses fixed rise/fall/hold delays
   (20/40 µs). For slow/marginal monitors, mirroring §5 — after releasing SCL,
   read 0x1c48 back and wait until the SCL data bit reads 1 (bounded loop) — would
   improve reliability. Optional; no failure observed on tested panels.
4. **Slow-DDC fallback.** The vendor retries with `HALPReadSlowDDCByte` +
   `ClientWait(500)`. The driver already sets conservative (~40 µs) timing, so a
   second slower pass is only needed if a specific monitor NAKs the base read.
5. **E-DDC (>256 B).** Vendor does not implement it; the X server's DDC2 layer the
   driver uses already can. No RE work outstanding.
6. **CEM / 0x1c4c and high-byte bank.** Only relevant to external-CEM boards (not
   the tested silicon); left as PROBABLE. If such a board surfaces, `portId==1`
   uses 0x1c4c (masks in §8) and 0x85/0x92 use the `EnableCEMDDCSel2` mux.

## Register quick-reference

| reg     | meaning |
|---------|---------|
| 0x1c48  | native DDC/I2C GPIO — data `1<<pin`, OE `1<<(pin+16)`, readback `1<<pin` (all 4 lines) |
| 0x1c4c  | CEM/CPLD 2-line I2C (board-specific): SCL 0x800/OE 0x400, SDA 0x2/OE 0x1 |
| PInS+0x04  | PInS revision word (gates 0x1a8 @>0x608, 0x1b4 @>0x609) |
| PInS+0x1a8 | DDC-line → pin-id assignment word |
| PInS+0x1b4 | 4-byte pin-id table (byte i → GPIO bit i); default `0x05070608` = ids {8,6,7,5} |
