# The authoritative Parhelia draw sequence

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

One ordered sequence, reconciled from `parhelia-warp-binding.md`,
`parhelia-mmio-capture.md`, `parhelia-block-state-map.md`,
`parhelia-raster-clip.md`, `parhelia-command-construction.md` and
`parhelia-rendertarget.md`, plus the recorded hardware attempts.  Where those
files disagreed, `parhelia-state-slot-map.md` §7.2 and the audit notes in each
file say which side won.  **Follow this file.**

Evidence marks: **[HW]** proven on a board; **[BIN]** read out of a vendor
binary and not executed.

## A trap that decides half the register names

Two rules both look like `0x1000 + n*4`:

* **VALID [HW]** — `reg = 0x1000 + index*4` where *index is a byte inside a DMA
  ring tag dword*. Proven from the vendor's own stream interpreter.
* **REFUTED** — the same arithmetic on a *region-2 GL state-slot id*. A state
  slot does not name a register at all.

Any draw-path register you can reproduce by multiplying a slot id by 4 is
suspect.

## Phase 0 — pre-flight [HW]

1. Sample `0x8f0` **more than once**. Idle = `0x00020000`. A steady
   `0x03000000` is a locked engine: `rmmod`/`modprobe` and an X restart do not
   clear it, **only a reboot does**, and a submit to a locked engine is void.
2. The ring must be **GFP_DMA32 system memory**, physically contiguous across
   every page. **Never the framebuffer BAR** — write-combining means the header
   is not resident when the engine fetches, and it runs off through VRAM.
3. Log line-buffered to a file before submitting; a bad attempt can reboot the
   box and take the evidence with it.

## Phase 1 — engine arm [HW, from the mmiotrace]

    0x10b0 = 0x00000010          bus-master arm
    0x12b8 = 0x00070601          bus-master configuration
    0x0300 = (read & 0xe0e0ff3f) | agpbits | 0x100      read-modify-write
    0x0384 = 0                   DMAPAD, not a soft-trap
    0x0308 = 0x83                (0x01 on parts other than 0x0528)
    0x0bc0 = 0x4087
    0x0920 = 1                   WARP enable -- this bit and nothing else
    0x0314 = 1                   kick

`0x10b0` carries two unrelated values in this driver: `0x10` here is the
bus-master arm; `0x0002305c` in Phase 5 is the WARP primitive type.

## Phase 2 — microcode [BIN gates, HW-exercised]

Gate in order, then stream:

    poll 0x8f0 bit17
    poll 0x450 == fence
    poll 0x908 until (v & 0xfff7ffbf) == 0x63a78001
    poll 0x924 until (v & 0xfff) == 0x100
    poll 0x8f0 until (v & 0x03000000) == 0
    0x918 = byteCursor / 12
    0x91c = code[0..2] per instruction
    0x920 |= 1

`0x918`/`0x91c` are the **instruction store only** — not a geometry path.
**Do not stream `warp_ucode.h` / `mtx2785`**: it is the 4812-byte WARP RAM
self-test, and streaming it arms the engine with a register sweep.

## Phase 3 — constants, BEFORE any bind [HW-adjudicated]

Hardware has ruled on this ordering: binding against an all-zero constant store
rebooted the machine; constants-first survived.

    header = 0x41000000 | (count << 17) | (count * 4 + 3)
    then   count, start_index, 1, 0, then count*16 bytes

**[HW] on both 0x0528 and P690** — the fetch pointer stops exactly at `end`.
Max **0x3f** entries per packet (six-bit count field).

Minimum set: `0x160`/`0x161` viewport scale/bias (**the pair that cannot be
omitted**), `0x000..0x006` world-view + normal matrix, `0x149..0x14c`
projection, `0x172..0x17e` the thirteen setup rows — using the **corrected**
values in `parhelia-block-state-map.md`, not the four wrong rows that stood in
`parhelia-state-slot-map.md` §6.3.

Every entry is `{0,0,0,0}` until written. Anything the program reads must be
programmed.

## Phase 4 — raster / clip [BIN, from the kernel core]

    0x1300 DWGCTL
    0x1328 SCISSOR_MIN = ((H-y-h) << 16) | x
    0x132c SCISSOR_MAX = ((H-y)   << 16) | (x + w)
    0x12f4 guard band  = 0x41f80000 (31.0f), 0x41700000 above two texture stages

Set DWGCTL bit 23 when scissoring. `0x1280`/`0x1284` are **not** scissor — they
are a packed 10-bit colour constant and dead code.

## Phase 5 — surfaces, then preamble

    0x1220/0x1224   surface slot 0 = colour render target
    0x1230/0x1234   surface slot 2 = depth/stencil
      base = plain VRAM byte offset (bits[1:0] = memory-space selector)
      cfg  = pitch[13:0] | type[18:16] | fmt[29:24] | tiling[31:30]

Then the pipe preamble (`parhelia-warp-binding.md`):

    0x1000=0, 0x1040=4, 0x10b0=0x0002305c        primitive type
    0x10b4=0, 0x10b8=0x83c00000
    0x1120..0x112c = 0,0,0,1.0f
    0x1110, 0x1114, 0x1118
    0x1130 = ctl                                  WARP pipeline control

## Phase 6 — bind, SHORT FORM ONLY

    [0x00000004][0x403c3cdd][xx][0x20000028][0x20000028][entry]
      index bytes LSB-first: dd(0x1374 strobe), 3c(0x10f0), 3c(0x10f0), 40(0x1100)

`MODE = 0x20000028`; `entry = 4 * N` where `N` is what went to `0x918` before
streaming that program.

**Do not write the six stage registers (`0x10c0`, `0x10c8`..`0x10d8`) or
`0x10f8`** until the short form is proven — that combination is what took the
machine down.

## Phase 7 — DWGCTL last, then the draw

DWGCTL is written **after** the surfaces, matching the vendor's dirty-bit flush
order (surfaces -> BCOL -> FCOL -> PAT -> PLNWT -> SCISSOR -> DWGCTL ->
geometry).

    0x1300 = (dstSlot << 10) | (srcSlot << 8) | 0x07  [| 0x800000 if scissoring]

**Do not copy the literal `0xc07`** — it encodes dst slot 3. Build the slot
fields to match the surface you actually bound.

Vertices live in a **separate buffer**, **stride 20 bytes, `[x, y, r, g, b]` as
IEEE floats in raw GL coordinates** — confirmed twice independently. The command
ring carries only the range:

    [0x8f000001][first][last]
    [0x00000002][0x21212425][fenceValue][fenceAddr]     -> 0x1094, 0x1090

**`0x4106000f` + 16 dwords is NOT this path.** It appears twice in the notes and
both are `mtx676`, the boot-time RAM self-test, pushing walking
`AAAAAAAA`/`55555555` patterns. Borrowing it is borrowing a memory test.

Submit:

    0x404 = ringStart ; write barrier ; 0x408 = ringEnd | 3

The low two bits (DMA mode 3) are the trigger. **There is no go register** — the
vertex push is what makes a bound program run, and `0x12bc` is DWGSYNC, a fence
written as a pair, not a fire.

## Packet count fields — three different rules

* opcode `0x8f` (draw): `count = payload dwords - 1`
* opcode `0x07`: `count = indices - 1`
* register lists: `count = number of registers`
* multi-tag register packets: `header = (tag dwords + data dwords) - 1`

Getting this wrong makes the engine mis-parse silently.

## What is genuinely [HW] on this path

The kernel ioctl ABI including the `nr=33` doorbell; the ring submit
`0x404`/`0x408|3` from GFP_DMA32 memory; block-load framing on both cards; the
Phase 1 arm set; the microcode gate-and-stream; the stride-20 `[x,y,r,g,b]`
vertex; a 2D solid fill of 120000 pixels.

**Everything about program binding is [BIN] and has never been validated.**
Note also that "CONFIRMED" in several files means "read out of the binary", not
"works on the board" — `parhelia-vertex-pipeline.md` states outright that none
of its part 2 has been executed on a Parhelia.
