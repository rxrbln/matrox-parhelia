# Session summary, 2026-09-05: what changed and what to do next

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## The headline

**No triangle from our own driver.** But the diagnosis changed completely, and
most of what this project believed about why was wrong.

The engine was **fetching our 3D ring to completion without executing it**.
`0x404 == end` — the check every previous negative rested on — only says the
fetch pointer advanced. With a validated fence we can now see execution
directly, and the picture is different from "the draw runs and emits nothing".

## Instruments: three were broken, one works

| instrument | status |
|---|---|
| WARP readback port | reads real content, but is **read-only** and nothing we can execute moves it |
| pixel scan | was anchored at the wrong offset; fixed, and now **proven** by drawing 2D into the 3D render target |
| DWGSYNC with bit 0 set | **never retires**, hangs the engine, needs a reboot — this hung the box four times today |
| **DWGSYNC with bit 0 clear** | **works.** Four exact tag matches, negative control holds its previous value |
| **memory write-back** | **works**, via MMIO with space tag 2 — see below |

Every "0 registers changed" and every "packets consumed, nothing drawn" result
in this repository predates a working instrument and should be re-read with that
in mind.

## What executes, measured

| packet | result |
|---|---|
| register-write `[hdr][4 data]` | executes |
| address-mode draw `0x17000001\|…` | executes |
| DWGSYNC (non-blocking) | executes, observable at `0x41c` |
| count-1 header-less `0x41` block load | executes |
| multi-vec4 `0x41`, any framing incl. `mtx676`'s verbatim | **stops the engine** |
| secondary call `0x20292820`, any space tag | **stops the engine** |

## Fixes landed

* **`0x300` is 16-bit.** The high half is read-only status, so the captured
  `0x1f0b0140` was the vendor writing back what it read. Our arm wrote `0` where
  the vendor holds `0x140`. Now verified sticking.
* **Completion write-back solved**: `0x1090 = addr | 2`, `0x1094 = value`,
  written **by MMIO before the doorbell**, not as a ring packet. Tag 0 never
  fires; tag 2 does. Memory-pollable, no MMIO read needed.
* **Engine init recovered** from `mtx_drv.so` `mtx1373`, and confirmed against
  hardware (`0x0000` reads back `0x10000000` after X runs). Its aperture block
  resets the machine under a live console and is flag-gated.
* Tracer/replay: in-band buffer creation, the `B` provenance flag for dwords
  both sides write, and the control-page pointer rebuild.

## Eliminated

`0x08fc = 0` (was the leading hypothesis) · the WARP bind as a precondition ·
both readings of `0x1000`/`0x10b0` · the secondary-buffer draw form · an
identity matrix at block `0xE0` · X-as-initialiser (works, but the handover
takes 21 s and racing it wedges the box).

## The structural finding

**The kernel never emits a block-load packet.** `0x4106000f` is the only `0x41`
tag in the object and all four uses are in the RAM self-test. The format
`parhelia-3dtri.c` has emitted since the beginning exists nowhere in the vendor
driver.

Both kinds of WARP state travel through kernel-side software instead:

| | record | shadow | dirty | flush |
|---|---|---|---|---|
| constants | key `0x43` | `HW[0x1e60]`, 256 slots | `0x1b44`… | `mtx610`, 6 groups x `0x40`, into `constobj+0x230` |
| matrices | key `0x20` | `HW+0x1ef8 + slot*64` | `HW[0x1bf4]` | `mtx3699` compaction |

Every stage examined is software. No host-side code was found that pushes the
block file to hardware.

## The open question, and the hypothesis

**Where does the staging area reach the hardware?** The best hypothesis is that
the WARP reads its constants **directly from memory**, `constobj+0x230` being a
DMA source rather than something copied through the ring. That would explain, at
once: no `0x41` outside a self-test, no register path to the block file, and a
read-only readback port.

To settle it, find where that buffer's bus address is handed over — most
plausibly one of the write-only WARP registers (`0x1000`, `0x1040`, `0x10b0`,
`0x1100`, `0x1104`).

## Recommended next steps

1. **Capture rather than disassemble.** `MTXTRACE_BUFS=1` around a `glLoadMatrix`
   in `gltri` will show the key-`0x20` record; a capture with the *kernel* side
   visible would show whether a bus address is programmed. `tools/gltri.c` and
   `tools/parhelia-recdec.py` make this cheap and the grammar now parses
   byte-exactly.
2. **Do not add more packet-framing variants.** If the DMA hypothesis holds they
   cannot work, and six were tried today.
3. Keep every future negative behind the non-blocking DWGSYNC bracket **with a
   distinct unused tag and a negative control**. That single discipline
   overturned three standing conclusions in one evening.
