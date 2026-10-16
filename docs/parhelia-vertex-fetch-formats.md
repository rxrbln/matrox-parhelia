# Parhelia vertex fetch formats

Copyright 2026 René Rebe <rene@exactco.de>

This document covers the attribute descriptor's format field, bits [15:12] of the `0x10b8` words (DESC_ATTR, register index `0x2e`). The descriptor layout itself is in `parhelia-indexed-draws.md` §3.3. The packed colour is in `parhelia-vertex-fetch-and-memory-classes.md` §3.1.

Tags:

* **[BIN]** read from the vendor binaries;
* **[HW]** measured on the hardware;
* **[INF]** inferred.

## 1. The vendor tables [BIN]

The same tables are in `MTXPARD.dll` (0528), `mtxpserd.dll` (P690) and the Linux `mtx_parhelia.o` (`.rodata+0x5440`).

**Declaration type to internal code.**

* MTXPARD `0x1723a9..0x1723e4`, table `0x19e8e0`; mtxpserd `0x174369`, table `0x1a08c8`.
* The table is indexed by the D3DDECLTYPE, plus 8 when the declaration has no POSITIONT element.
* Only the DX8 types 0..7 are mapped:

  | type | internal code |
  |---|---|
  | FLOAT1..4 | 1..4 |
  | D3DCOLOR | 8 |
  | UBYTE4 | 9 when pretransformed, 5 otherwise |
  | SHORT2 | 11 |
  | SHORT4 | 13 |

* `0x19e80c[type]` sets declaration bit 23, which becomes descriptor bit 29. It is set on FLOAT1..3 and SHORT2 and clear on the 4-component types. **[INF]** It requests default values for the missing components.

**Internal code to descriptor.** The descriptor S word is built at MTXPARD `0x11dfec..0x11e032` and packed into the `0x10b8` value by `0x12dd00`.

* Bits [15:12] = `0x19e110[code]`, for codes 0..18: `0 0 0 0 0 1 5 2 6 3 7 10 11 10 11 8 9 8 9`.
* The mask [25:22] = `0x19e7a0[code]`. It is 1 for every one-dword packed code and 3 for every two-dword one, so for packed formats **the mask counts dwords, not components**.
* The dwords per code (`0x19df00`) agree: 1 for codes 5..12, 15 and 16; 2 for 13, 14, 17 and 18.
* [19:16] is the declaration slot, i.e. the WARP input register: 0 position, 5 colour, 7 + n texture coordinate set n.

No D3D type produces codes 6, 7, 10, 12, 14 and 15..18, but the vendor tables them anyway.

## 2. What each format fetches

The measured columns use two probes. Every value is raw, ramped across the primitive, and read back per channel.

* **Texcoord slot.** `parhelia-drm-test --vertex-format-probe` puts coordinate set 0 at dword 2 and reads s, t, r back through the TEXCOORD pass (P690 only: the 0528 has no WARP_REPLACE program). The A channel does not carry q.
* **Colour slot.** `--packed-colour-probe` with `PACKED_DESC`, in the 3-dword x, y, colour stream, on both families.

| [15:12] | dwords (mask) | fetch | P690 | 0528 |
|---|---|---|---|---|
| 0 | 1..4 (mask = components) | FLOAT32 | [HW] | [HW] (every existing stream) |
| 1 | 1 | UINT8 ×4, byte 0 first, unnormalized | [HW] | [HW] colour slot |
| 2 | 1 | UNORM8 ×4, byte 0 first (GL RGBA8 order) | [HW] | [HW] colour slot |
| 3 | 1 | reads back 0 in the texcoord slot for every value tried | unidentified | not probed |
| 5 | 1 | UINT8 ×4, byte 2 first (BGRA) | [HW] | [HW] colour slot |
| 6 | 1 | UNORM8 ×4, byte 2 first (D3DCOLOR) | [HW] | [HW] |
| 7 | 1 | reads back 0 in the texcoord slot | unidentified | not probed |
| 8 | 1 or 2 | UINT16 ×2 per dword, low half first, unnormalized | [HW] texcoord slot | not probed |
| 9 | 1 or 2 | UNORM16 (0xffff = 1.0) | [HW] texcoord slot, 2 and 4 components | not probed |
| 10 | 1 or 2 | SINT16 (0xffff = -1, 0x8000 = -32768) | [HW] texcoord slot, 2 and 4 components | not probed |
| 11 | 1 or 2 | SNORM16 (0x7fff = 1.0) | [HW] texcoord slot | not probed |

Codes 4 and 12..15 are not in the vendor table and were not probed. There is no half-float format.

Measured details:

* **UNORM16:**
  * raw 0x8000 reads 128;
  * 0x3c00 reads 60;
  * 0xffff reads 255.
* **SINT16:**
  * a ramp from -1 to 3 crosses zero a quarter of the way;
  * 0x8000 reads 0, which identifies it as signed.
* **UINT16:** 0x3c00 saturates, which identifies it as an integer and not a half.
* **SNORM16:** 0x7fff reads 255.
* **Bytes:**
  * format 2 turns a flat `c0804010` into the pixel `c0104080` (byte 0 to red) on both families;
  * formats 1 and 5 saturate the same flat value to `ffffffff`;
  * the corner colours `ff0000ff` / `ff00ff00` / `ffff0000` come out of format 5 exactly as from format 6, and out of format 1 exactly as from format 2.

## 3. Restrictions found on the way [HW]

* **16-bit formats do not draw in the colour slot (kind 5) on the P690.**
  * In the 3-dword packed-colour stream, formats 8..11 left the target untouched.
  * In a 5-dword x, y stream through the kernel pass-through program, format 10 with mask 3 stopped the engine: "submit timeout at CALL 19/19 ... wedged", on 2026-10-15. A power cycle was needed.
  * The 16-bit formats are therefore measured only in the texcoord slot, and only on the P690.
  * They were not run on the 0528: the colour slot is its only probe route, and that route misbehaves on the P690.
* **Kernel extent check.** The kernel computes a draw's fetch extent as `offset*4 + popcount(mask)*4`. Because the mask counts dwords, that is exact for every packed format.

## 4. What this means for ioquake3 (2026-10-15)

Measured on nano with `PARHELIA_TC_STATS=1`, `bench` demo, 1280x720, per frame:

* 5.8k range vertices with 6 dwords (position 3, packed colour 1, s,t 2);
* 2.8k range vertices with 8 dwords (two coordinate sets);
* 20.5k index dwords.

Coordinate set 1 (lightmaps) is always inside [0,1]. Set 0 spans [-28.7, 15.5], and after an integer bias up to 2^13 texels.

* **Set 1 as UNORM16** would save 2.8k dwords per frame. Mesa only; no kernel change.
* **Set 0 as SINT16** needs a per-draw scale and bias in the WARP transform program, i.e. new kernel programs. It would save 8.6k dwords per frame.
* **Cost on the EV56.** The conversion has to be done with integer bit operations: there is no FTOI, and `-mieee` puts a trap barrier on every FP instruction. It costs about as much as the stores it saves.
* **Estimate [INF]:** at most 0.1-0.2 ms per frame of the Alpha's ~32 ms, below its noise.

Neither was implemented (decision 2026-10-15). The unfinished lightmap path is kept as `campaign/agents/vertex-records/tc16-lightmap-not-pursued.diff`.
