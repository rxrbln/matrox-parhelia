<!-- SPDX-License-Identifier: GPL-2.0 -->

# Parhelia/P-series KMS DDC and connector detection

This note records the DDC/I2C and analog-load-detection behavior recovered from
the saved Matrox Linux `mtx_drv.so` 7.4.99 and Windows XDDM `MTXPARM.sys`.
Addresses below are binary virtual addresses/RVAs, not MMIO offsets. No target
machine was used for this analysis.

## Native DDC GPIO

All four native DDC channels use the shared 32-bit MMIO register `0x1c48`.
For physical GPIO bit `n`, the data/readback mask is `BIT(n)` and output-enable
mask is `BIT(n + 16)`. The electrical operation is open drain:

- low: clear the data bit and set OE;
- high: clear OE and let the board pull-up raise the line;
- input: release OE, then sample the same low data bit.

Linux `HSLPARI2CGenericRead`/`Write` at `0xc2360`/`0xc24b0` prove this. The SCL
write path polls the sampled SCL level after release, bounded at about 10,000
iterations with `ClientWait(10)`, so clock stretching is supported.

`HSLPIN60GetDDCIORegMasks` at `0xda420` maps a logical pin ID to a physical GPIO
bit. It reads the PInS version word at `PInS+0x04`. For versions greater than
`0x0609`, the packed four-byte table is at `PInS+0x1b4`; otherwise it uses
`0x05070608`. A requested logical ID matching byte index `n` produces data mask
`BIT(n)` and OE mask `BIT(n+16)`.

## DDC-line resolver

Linux `HSLPARGetDDCIORegMasks` is at `0xb8410`, with its pair resolver at
`0xb7e10`. It does not inspect `PInS+0x1a8`. It dispatches line index 0..3 to
fixed candidate lists:

| line | logical `(SCL,SDA)` candidates in order | Linux helper |
|------|------------------------------------------|--------------|
| 1 | `(1,2)`, `(5,6)`, `(0xd,0xe)`, `(9,0xa)` | `0xb82b0` |
| 2 | `(3,4)`, `(7,8)`, `(0xd,0xe)`, `(0xb,0xc)` | `0xb8150` |
| 3 | `(5,6)`, `(0xf,0x10)`, `(9,0xa)` | `0xb8020` |
| 4 | `(7,8)`, `(0xf,0x10)`, `(0xb,0xc)` | `0xb7ef0` |

The Windows miniport independently contains the identical resolver at `0x42305`
and identical line helpers at `0x413c0`, `0x4147f`, `0x4153e`, and `0x415da`.
Its pair resolver is at `0x407b3`. This cross-check is useful because an older
analysis incorrectly labeled unrelated Linux function `0xda4d0` as the DDC
resolver and consequently assigned meaning to `PInS+0x1a8`.

The two saved ROMs resolve as follows:

| board/ROM | PInS version | table | DDC line 1 | DDC line 2 |
|-----------|--------------|-------|------------|------------|
| P690 `tools/p690.rom` | `0x0700` | `0x03040102` = IDs `{2,1,4,3}` | GPIO SCL1/SDA0 | GPIO SCL3/SDA2 |
| original `vbios-matrox-parhelia` | `0x060a` | `0x05070608` = IDs `{8,6,7,5}` | GPIO SCL3/SDA1 | GPIO SCL2/SDA0 |

Consequently, the known P690 mapping is appropriate for PCI ID `0x2539`, and
the saved original-board mapping is appropriate for `0x0527/0x0528`. The
`0x2537/0x2538` family uses the same resolver but its actual GPIO bits must come
from that board's PInS table. Product/device ID alone is insufficient for a
fully general implementation.

The DDC line describes the physical connector. It does not imply which display
controller, DVO, or DAC feeds that connector. In particular, the working P690
DVI-1 route uses controller A/head 0 through DVO2 despite older DDX conventions
that associated its first connector with another head.

## EDID transaction behavior

The vendor EDID functions are `HALPReadSlowDDCByte` `0x94b40`,
`HALPReadDDCByte` `0x94d60`, `HALGetEdidBlockContent` `0x94f80`, and
`HALPReadDDCEDID` `0x954c0`. They use DDC slave byte address `0xa0` (7-bit
address `0x50`), a one-byte offset, and 128-byte blocks. There is no E-DDC
segment-pointer write to address `0x60`, so the vendor path reaches only the
first 256 bytes.

The kernel should use standard DRM EDID helpers over an `i2c-algo-bit` adapter.
They provide normal retries and E-DDC segment reads beyond the vendor limit.
Provide both `getsda` and `getscl`; `i2c-algo-bit` then waits for clock stretching.
Because all adapters modify `0x1c48`, lock the whole transfer across every
connector with shared `pre_xfer`/`post_xfer` callbacks. Per-adapter I2C locking
does not protect a second adapter. Use read-modify-write so one connector never
changes another connector's GPIO bits.

Recommended adapter/connector sequence:

1. Resolve each connector's logical DDC line through the PInS pin table.
2. Register one `i2c_algo_bit_data` adapter per physical connector. A 10-20 us
   half-period and approximately 100 ms stretch timeout are conservative.
3. Initialize with `drm_connector_init_with_ddc()`.
4. Detect with `drm_connector_helper_detect_from_ddc()` and read modes with
   `drm_edid_read()`, `drm_edid_connector_update()`, and
   `drm_edid_connector_add_modes()`.
5. Model a physical DVI-I socket as `DRM_MODE_CONNECTOR_DVII`. Set its
   subconnector property from `drm_edid_is_digital()`: DVI-D for digital input,
   DVI-A for analog input.

## Analog load sense

Linux `HSLPARSenseLoadOnInternalDAC` at `0xc0df0` uses the following common
registers on all five requested device IDs:

| purpose | DAC1/head 0 | DAC2/head 1 |
|---------|-------------|-------------|
| DAC control | `0x1c18` | `0x1c1c` |
| test word | `0x188c` | `0x1a8c` |
| mode | `0x1910` | `0x1b10` |
| result | `0x1c14[2:0]` | `0x1c14[5:3]` |

The vendor saves all touched registers, ORs `0x1400` into the selected DAC
control, writes test word `0x16459164`, masks the mode with `0xf000ffff`, waits
about 10 ms, reads `0x1c14`, and restores the saved state. A cleared result bit
means load on that color channel; `(sense & channel_mask) != channel_mask`
therefore means at least one channel is loaded.

For a DVI-I connector, a valid EDID is authoritative: EDID input bit 7 selects
the digital or analog route. Only use DAC load sense when no valid EDID is
available. Otherwise the load detector can falsely override a valid digital
sink. Since sensing temporarily changes DAC/test state and can flicker an active
analog output, avoid repeated destructive polling while that DAC is scanning.

## Device-family and connector distinctions

| PCI IDs | DDC behavior | output-routing distinction |
|---------|--------------|----------------------------|
| `0x0527`, `0x0528` | shared `0x1c48`, default or ROM `+0x1b4` pin table | original DVO crossbar at `0x1c20` |
| `0x2537`, `0x2538`, `0x2539` | same `0x1c48` algorithm, board PInS pin table | LX/P-series twin DVO registers `0x1c54`/`0x1c58` |

Do not derive connector type, DVO/DAC route, or connector count merely from
these PCI IDs. PInS `+0x1a1` is involved in the vendor's board route selection
through `HSLPARGetDVOIndex`/`HSLPARGetDACIndex`; the precise enum decode remains
separate work. Keep a board/output descriptor that independently records:

- physical connector type;
- DDC line/pin pair;
- digital DVO route;
- analog DAC route;
- usable CRTC/head mask.

This separation permits the established P690 DVI-1 DVO2 route without changing
its DDC line and avoids advertising DVI-2 until its wire route is verified.

## External board path

MMIO `0x1c4c` is a board-gated CEM/CPLD I2C path, not the integrated second DVI
DDC channel. The vendor port object uses SCL data `0x2`, SCL OE `0x800`, SDA data
`0x1`, and SDA OE `0x400`. Port IDs `0x85`/`0x92` additionally select the CEM
mux through `HSLPAREnableCEMDDCSel2`. This path should remain disabled unless
the board description explicitly identifies that external hardware.
