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
    0x920 = ?   see the conflict below

> **UNRESOLVED — `0x920` bit 0.** A static reading of `mtx656` @0xa0ee4 and
> `mtx3810` @0xa3d31 says bit 0 is `(hal[0xa3114] != 0)`, and `0xa3114` is the
> driver's **no-hardware/simulation** flag (`mtx2316` routes to the real
> uploader `mtx1391` when it is 0). That reading implies **`0x920 = 0` on real
> hardware**, and that writing 1 selects simulation — which would mean the
> engine never runs.
>
> Against that, the live mmiotrace on this hardware shows `W 0x920 0x1`, and
> `mtx676` itself writes 0. Do not flip this on the static reading alone;
> it is two runs to settle empirically, and it is a prime suspect for
> "the program is bound but nothing executes".

`0x918`/`0x91c` are the **instruction store only** — not a geometry path.
**Do not stream `warp_ucode.h` / `mtx2785`**: it is the 4812-byte WARP RAM
self-test, and streaming it arms the engine with a register sweep.

## Phase 3 — constants, AFTER the bind [BIN, corrected 2026-09-05]

**This reverses what this file previously said.** The vendor emits
bind → block loads → vertices, in one submission. `mtx693` @0x436a0, in
emission order: optional preamble, `entry = mtx2316(...)`, the **bind packet**
at 0x43748, and only then at 0x437cd `if (bank && *bank) mtx1342(bank, ring)` —
the block loads — then `mtx1937` to submit. `mtx1814` has the identical shape.
The ring write pointer only advances, so code order is stream order.

`mtx1342` has **exactly one caller**, `mtx693`. The block file is never flushed
independently of a program bind, and if `program == NULL` `mtx693` returns at
0x436e6 emitting nothing at all — not even pending constants.

**The "all-zero constant store rebooted the machine" theory is not supported by
the binary.** The vendor binds programs while `0x000..0x14c` are entirely zero;
that is its own steady state. Whatever took the machine down in attempt 1, it
was not that. Both orders may work, but only bind-then-constants is attested.

### There are two block-file banks, and they are not interchangeable

`mtx2209` @0x8e000 builds both; `mtx3654` selects on every validate via
`byte [HWREG+0x7d9]`:

| | bitmap base | entries base |
|---|---|---|
| **bank A** (`0x7d9 != 0`) | `HWREG+0x07e8` | `HWREG+0x0a18` |
| **bank B** (`0x7d9 == 0`) | `HWREG+0x2218` | `HWREG+0x2448` |

`mtx2037` @0x8b443 sets `0x7d9 = 1` right after a program object is produced, so
**a WARP vertex program present ⇒ bank A ships** (inferred, but well supported).
`mtx2209` initialises `0x7d9 = 1`, so bank A is the default.

**This resolves the long-standing `0x173` disagreement between our docs:** bank A
holds `{1.0, 1.0, 0.5, −0.5}` and bank B holds `{−0.5, −0.5, 0.5, 0.5}`. The
value recorded in `parhelia-state-slot-map.md` was bank B's.

### The init set is 10 indices, not 26, and 4 of the "13 setup rows" are bank B only

Every one of the 0x180 entries is zeroed at construction (`mtx3107` @0x8fef0).
`mtx2209` then writes and dirty-marks exactly these in bank A:

| idx | d0 | d1 | d2 | d3 |
|---|---|---|---|---|
| `0x14d` | 0 | 0 | 0 | `42000000` 32.0 |
| `0x160` | 0 | 0 | 0 | **`3f800000` 1.0** |
| `0x161` | 0 | 0 | 0 | 0 |
| `0x172` | `3f800000` | 0 | 0 | `3f800000` |
| `0x173` | `3f800000` | `3f800000` | `3f000000` | `bf000000` |
| `0x174` | `45ff0000` | `45ff0000` | `c5ff0000` | `c5ff0000` |
| `0x175` | `43000000` 128.0 | 0 | 0 | 0 |
| `0x178` | 0 | 0 | `3f000000` | `49400000` |
| `0x17d` | `4b000000` | `3d4ccccd` | `41f80000` | `3d042108` |
| `0x17e` | 0 | `3d800000` | 0 | 0 |

`0x000..0x006` and `0x149..0x14c` are **not** written at init — they stay zero
until the app uploads matrices.

**`0x179..0x17c` (the EXP/LOG Taylor series) exist only in bank B** and are
never written in bank A by any code in the object. Loading them into a WARP draw
loads something the vendor never loads there. `0x176`, `0x177`, `0x17f` are never
written in either bank; `0x162..0x16b` are referenced nowhere at all.

So the "thirteen setup rows" figure is wrong for a WARP draw: it is **seven** —
`0x172, 0x173, 0x174, 0x175, 0x178, 0x17d, 0x17e` — plus `0x14d.d3` and the
viewport pair.

`0x172`/`0x173` are not magic constants: they are the point/line attenuation
`{a,b,c,size}` and the size clamp `{min,max,0.5,−0.5}`, and those values are the
GL defaults.

Packet header (unchanged, **[HW]** on both 0x0528 and P690, fetch pointer stops
exactly at `end`):

    0x41000000 | ((entries) << 17) | (entries*4 + 3)
    then   count, start_index, 1, 0, then entries*16 bytes

Note `mtx1185`, an exported entry point with no caller in this object, writes the
descriptor as `(0, start, 0, 0)` rather than `(count, start, 1, 0)` for the same
hardware operation — so dwords 0 and 2 are mode flags, not constants.

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
streaming that program (`mtx1391` returns `base/3 + 4*entry[idx]`).

**`0x10f0` behaves as a strobe/latch, not a plain state register.** The same
value is written twice at every site, and `mtx676` writes it a *third* time
after `0x1100`/`0x1104`. A single write is not enough, and this is a leading
candidate for "bound but never executes".

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

    [0x01][0x21212141](toggle << 2)      ; 0x1104 = 0 or 4, toggled every draw
    [0x8f000001] first count             ; note: first,COUNT
    [0x07000001] ((count-1) & 0xffff) | (first << 16)
    [0x00000002][0x21212425][fenceValue][fenceAddr]     -> 0x1094, 0x1090

The draw packet is emitted by the **ICD**, not the HAL -- `0x8f000001` does not
occur anywhere in `mtx_parhelia.o`. One source reads the payload as
`first, last` and another as `first, count`; unreconciled, and it matters.

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

## Readback: four banks, not one

`mtx514` @0x8f7c0 takes `addr = (space & 0xf) << 24 | ((sel << 9) | index) & 0xfff`.
Valid `space` values are `{0,1,2,3,4,5,8,9,10}`; `mtx612` uses `space = 3` and
sweeps **`sel = 0..3`, `index = 0..0x17f`** — four banks of 384. Reading only
`sel = 0` leaves three quarters of the file invisible, and a program writing into
`sel != 0` looks exactly like "zero registers changed".

## What is genuinely [HW] on this path

The kernel ioctl ABI including the `nr=33` doorbell; the ring submit
`0x404`/`0x408|3` from GFP_DMA32 memory; block-load framing on both cards; the
Phase 1 arm set; the microcode gate-and-stream; the stride-20 `[x,y,r,g,b]`
vertex; a 2D solid fill of 120000 pixels.

**Everything about program binding is [BIN] and has never been validated.**
Note also that "CONFIRMED" in several files means "read out of the binary", not
"works on the board" — `parhelia-vertex-pipeline.md` states outright that none
of its part 2 has been executed on a Parhelia.


## Hardware attempt, 2026-09-05, P690 — a controlled negative

Replicating `mtx676` as exactly as the disassembly allows — streaming `mtx2785`
itself, binding it in the vendor's short form, pushing the four walking-pattern
packets, then reading the register file back the way `mtx612` does — produces
**no register change at all**.

    mtx2785-selftest gates: idle=1 908=1 924=1 busy0=1 (0x908=63af8001 0x924=00200100)
    mtx2785-selftest: streamed 1203 dwords to 0x91c
    selftest676: 83 words
      bmstat during=00000000        <- the engine DOES go busy
      bmstat=00020000 fetch 0x404=197d014c == end
    selftest676: 0 registers changed -- no evidence of execution

This is the strongest available isolation of the problem, because it uses the
vendor's own program, the vendor's own data, and the vendor's own expected
result. It is not our transform program that is at fault.

**Confirmed working on this card in the same session**, so none of these is the
cause: the system-memory ring; packet framing (a 2D solid fill draws exactly
120000 pixels); the engine's willingness to do work (`bmstat` leaves idle on
both the fill and the WARP submit); the microcode gates (all four pass); the
`0x91c` stream (1203 dwords accepted).

**Ruled out by measurement, each with both settings tried:**

| candidate | tried | result |
|---|---|---|
| `0x920` bit 0 | 0 and 1, before *and* after the stream | no difference |
| DWGCTL slot encoding | `0x07`, `0x307`, `0xc07`, `0x800007` | no difference |
| register-write framing | bare `[tag][data]` **and** vendor `[count][tag][data]` | no difference |
| bind order | constants-then-bind **and** bind-then-constants | no difference |
| readback coverage | all four banks (`sel` 0..3), not just bank 0 | no difference |
| Phase 1 engine arm | present and absent | no difference |

**What has not been excluded, and is where to look next:**

1. `mtx1391` does more than stream: it spins on `[dev+0x78]`, calls `mtx1937`
   to drain the ring *before* touching `0x918`, and maintains a code-store
   allocator at `hal[0xa3118]`. The ring-drain-before-upload ordering is not
   replicated here.
2. The entry value. `mtx1391` returns `base/3 + 4*entry[idx]` — note `/3`, not
   `/12`. With `base = 0` both give 0, so this is untested, and any program
   streamed at a non-zero offset would expose it.
3. Whether readback `space = 3` observes what the self-test writes at all.
   `mtx612` uses it, but we have never seen a *positive* readback from this port
   on this card, so its ability to observe anything is itself unverified.

Item 3 matters most: without one positive control through the readback port, a
zero result cannot distinguish "the WARP did not run" from "the port does not
show it".


## DWGCTL: what is measured, and a method note (2026-09-05, P690)

Measured against the known-good 2D fill geometry, changing **only** `0x1300`:

| value | result |
|---|---|
| `0xc0` | draws 120000 px, to offset 0 **and** to the off-screen target |
| `0x07`, `0xc07`, `0x0030200c` | nothing |
| `0x00`..`0x10` | nothing |
| **`0x11`** | **locks the engine** — reboot required |

So the 3D opcodes do not route through the 2D rect path at all; they need the
WARP pipeline, and a `0x138c` LEN write does not substitute for it.

**Method note, recorded because it cost a reboot.** Sweeping the whole low byte
blind was a mistake: it is guess-and-test at scale, most values are meaningless,
and at least one of them locks the engine. The idle gate caught it and aborted
at the right place, but the right approach is to take opcode semantics from the
binaries and test named candidates, not to enumerate a field. Do not repeat the
sweep.


## DWGCTL 0x11 damages the engine in a way `0x8f0` does not report

Measured after a stale binary submitted `0x1300 = 0x11` (the guard existed in
the source but the box had been rebuilt from an older copy):

* `0x8f0` reads `0x00020000` — **idle**, the gate passes
* the 2D solid fill, which draws 120000 pixels on a healthy engine, draws
  **nothing**, repeatably
* an off-screen fill hangs the tool outright

So the busmaster idle bit is **not sufficient** to detect a damaged engine.
`engine_idle_gate()` will happily pass and every subsequent measurement is
worthless without saying so.

**Use the 2D fill as the health check.** It is the only positive control we
have: if it does not draw 120000 pixels, the engine is not healthy and nothing
measured afterwards means anything. Reboot first.

Two process rules this cost:

1. **Verify the source on the target matches before building.** Compare md5s;
   do not assume an earlier copy is current. A stale build here submitted a
   value the current source refuses, and then produced three "the fill no longer
   works" results that looked like a hardware discovery.
2. The tool prints a build stamp at startup for the same reason.


## Measurement traps that cost real time here

Three of these produced results that read exactly like hardware failures.

**1. Geometry must be read, not assumed.** The test box runs **1920x1200,
pitch 2048, depth 24 / 32 bpp** (`Xorg.0.log`: "Virtual size is 1920x1200
(pitch 2048)"), while `p3dtri` defaults to 1024x768 pitch 1024. A fill run with
the default surface pitch is self-consistent -- the engine writes at the pitch
it was given and a scan at the same pitch reads it back perfectly, which is why
"120000 px" was a valid proof that the *engine* executed -- but it writes across
the visible framebuffer in a garbled layout. The visible corruption is the
first symptom, and it is easy to misread as the tool being broken. Always pass
`--width 1920 --height 1200 --pitch 2048` when the target is the live screen.

**2. Scan the region you actually programmed.** The fill's pixel counter was
anchored at VRAM offset 0 regardless of the surface base. Pointing the surface
off-screen therefore reported zero pixels -- indistinguishable from a dead
engine -- when the fill had very likely drawn correctly where nobody was
looking.

**3. One count cannot tell three failures apart.** "0 green pixels" conflates
"wrote nothing", "wrote another colour" and "wrote somewhere else". The fill now
reports matches at the programmed base, matches on the visible screen, and
non-zero dwords at the base after clearing it.

And the standing one: **`bmstat` idle does not mean healthy.** After a bad
DWGCTL the engine reads idle and draws nothing. `--health` runs a 2D fill and
requires the full 120000 pixels; the WARP paths are gated on it. `--dmafill`
itself is not gated, so a fill result taken without checking the engine first
can be void -- check `0x8f0` at the top of the output, which the tool prints.


## X owns the engine: every prior WARP result was void

Measured on the P690, same 2D fill through our own ring, nothing else changed:

    == fill BEFORE X ==  green px at base 0x4000000 = 120000   drew
    == fill AFTER X  ==  green px at base 0x4000000 = 0        nothing

`0x8f0` reads `0x00020000` (idle) in **both** cases, so nothing announces the
difference. Starting the vendor MTX X driver stops our submissions taking
effect; it owns the DMA ring and writes `0x404`/`0x408` itself.

**Consequence: every WARP experiment recorded before this was run with X up and
is void.** "The engine consumes the packets but rasterises nothing", "0 WARP
registers changed", the DWGCTL comparisons, the bind-order comparisons -- all of
them measured a contended ring rather than the WARP. They are withdrawn as
evidence about the 3D path.

**Procedure from here: run engine experiments with X stopped.** Boot,
`modprobe mtx`, submit. Do not start X.

This also settles a question the earlier measurement had muddied: **the surface
base register IS honoured.** A fill to base `0x4000000` lands at `0x4000000`
when X is not running. The earlier "0 pixels off-screen" was X contention plus a
scan anchored at offset 0, not a hardware limitation.


## Two-source results from the Windows D3D driver (2026-09-05)

`MTXPARD.dll` is an independent second implementation for the same silicon.
Where it and `mtx_parhelia.o` agree, treat the result as settled.

### DWGCTL for a 3D primitive is the `0x10cc` family — and 0xc07/0x307/0x707 are not

Windows builds it explicitly (@0x151137-0x1511b7):

    DWGCTL = 0x10cc | {0x100000 | 0x180000} | [0x40000] | [0x200000]

Literals present in the DLL: `0x10cc, 0x810cc, 0x1010cc, 0x1810cc, 0x2010cc,
0x2410cc, 0x8010cc, 0xa410cc`. The Linux values `0x000010cc` and `0x002010cc`
sit inside that family.

Against the working 2D fill (`0xc0` = opcode nibble 0, bits[7:4] = 0xc), the 3D
value is `0x10cc` = **opcode nibble 0xc, same bits[7:4] = 0xc, plus bit 12**.
Change the opcode nibble and set bit 12; do not touch bits[7:4].

**`0xc07` / `0x307` / `0x707` are one-shot flush opcodes in BOTH drivers** —
each caches `0x1300` *unless* `(val & 0xf) == 6 || == 7`, at three sites in
Windows and in the Linux stream interpreter. They were never triangle opcodes.
Stop trying them.

### The `0x8f` draw packet belongs in a SEPARATE buffer

This is a real bug in what we emit. The draw packets live in a second-level
buffer that the primary ring *enters*; inlined into the level-1 ring,
`0x8f000001` is parsed as a tag dword. Entry is via `0x10a0 = start | 2`,
`0x10a4 = end | 2` (Linux HAL usage in `mtx1937` @0x323ce, where the pair is a
contiguous address range), with `0x109c = start` as an alternative reading from
the Windows emulator. The two sources disagree on whether `0x10a0`/`0x10a4` are
a range or a wait-until-equal semaphore; a "fetch until pointer == end" loop and
a "wait until equal" loop look identical statically. **Contested — try the range
first.**

### The WARP launch is the `0x1100` write, and a third packet class follows it

Windows @0x13b028 and Linux `mtx3400` @0x43bf8 emit the same shape and both
write the program handle to `0x1100` **last**. Immediately after, Windows emits
a **bulk inline-data word** and copies the payload behind it:

    ((n/4 - 1) & 0x3f | 0x2080) << 17 | ((n - 1) & 0xffff)   then n dwords

That is the vertex/constant payload riding behind the launch — a packet class we
do not emit at all.

`0x1104` is a bank/page toggle (`(flag ^ 1) << 2`, alternating 0/4), **not** a
launch. `0x1374` (index `0xdd`) is a **data-less trigger**: both drivers name its
index in a header and count its slot in the length but **never write the data
dword**. Copy that literally — do not "fix" it by writing a value, as we were.

### Merged one-time 3D setup (Windows `Setup3D` @0x2dc20 + Linux `mtx398` @0x32920)

    load the WARP program via 0x918 / 0x91c, keep the handle
    len=0xa : 0x1374 (no data); 0x10f0 = 0x20000000 x2; 0x10f0 = 0x80000000 or 0;
              0x1130 = 0x2000; 0x1100 = handle; 0x1104 = handle; 0x1110 = 3
    len=2   : 0x10b4 = 0, 0x10b8 = 0xa0000000
    len=7   : 0x121c = 0, 0x1374 = 0, 0x1200 = 0x0001000f, 0x1204/0x1208/0x120c = 0
    caps    : 0x1300 = 0x002010cc, 0x12f4 = 0x41f80000  (else 0x1300 = 0x000010cc)
    x5      : 0x1264, 0x126c, 0x1260, 0x1268 = 0x200, 0x400400, 0x200, 0x400400
    len=4   : 0x1300 = 0x008002cd, 0x1328 = 0x0fff0fff, 0x132c = 0, 0x12f4 = 0x41f80000
    len=3   : 0x1000 = ?, 0x1040 = 4, 0x10b0 = 0x00020020
    Linux adds: 0x11c0/0x11c4/0x11c8, 0x11e8, 0x12c0 = 0xffffffff, 0x12c4 = 0x7fffffff,
                0x12cc/0x12d0/0x12d4/0x12d8, 0x1324 = 0xffffffff,
                0x12b8 = 0x00070601, 0x12b0 = 0x3ff0f800

Note `Setup3D` uses `0x10f0 = 0x20000000` while the per-draw bind uses
`0x20000028`; the low nibble 8 appears only on the bind path.

### Other confirmations and new registers

* `0x1088` / `0x108c` are a **tag-repeat pair** — the tag dword to repeat and a
  repeat count. Useful for streaming long constant blocks.
* Index `0x21` terminates a header, index `0x20` pads but still consumes a data
  dword — third independent witness.
* `0x0940` bit 16 is an engine-idle poll; `0x940 = 1` brackets an operation and
  `0` releases it.
* `0x1148` is the most-emitted packet in the DLL (253 sites, data 0 or 1,
  bracketing every entry point) — **purpose unknown**.
* Windows flushes posted writes through PCI config space immediately before
  writing `0x408`. On PCIe the equivalent is a read-back before the doorbell.
  Without it the symptom is exactly "consumed but nothing drawn".

### A shorter path to first pixels than the WARP

The Windows legacy DDA / texture-trap path loads float gradients into
`0x1160`/`0x1164`/`0x1168` (S/T/Q start), `0x1178`/`0x117c` (d/dX) and
`0x118c`-`0x1198` (per-stage d/dY), then fires with the **same `0x1304`,
`0x1308`, `0x138c` sequence the 2D fill already uses on our hardware**. This is
a different, mutually exclusive pipeline from the WARP — consistent with our
note that these registers fault a live WARP — but it reaches a textured triangle
through a launch that is already proven here. Keep it strictly separate from any
WARP attempt.


## Verified state after the corrections (2026-09-05, P690, X stopped)

Everything below was measured in runs where the engine proved itself healthy in
the same run (`--health`: 2D fill draws 120000/120000) and with **X not
running**, so these are valid measurements rather than contended ones.

**Confirmed working:**

* The DMA ring: packets are consumed, the fetch pointer stops exactly at the
  programmed end, every time.
* **Register writes land.** After writing `0x1300 = 0x008002cd` through a ring
  packet, `0x1300` reads back `0x008002cd`. The tag/data framing reaches the
  register file.
* The surface base is honoured: a 2D fill to base `0x4000000` lands at
  `0x4000000`.
* `0x920 = 0`, the Phase 1 arm block, and the microcode gates all pass; 1203
  dwords stream to `0x91c` without complaint.
* The engine survives every 3D DWGCTL in the vetted table -- `0x10cc`,
  `0x2010cc`, `0x8002cd` all leave it idle.

**Still not rasterising.** With all of the following applied together, a WARP
draw writes nothing anywhere in VRAM (0 of 32768 pages):

* DWGCTL `0x000010cc` (the real family, not the old `0x0030200c` guess)
* a read-back of `0x404` to post the ring writes before the `0x408` doorbell
* the merged Setup3D list from both drivers
* the bind short form with `MODE = 0x20000028` and the launch written to `0x1100`
* vertices as `0x41` bulk-data quads riding behind the launch
* the `0x8f` draw range in a second-level buffer entered via `0x10a0`/`0x10a4`
* X stopped, engine health proven in the same run

The 3D opcodes also do not rasterise through the 2D rect geometry
(`0x1304`/`0x1308`/`0x138c`), which is consistent with them requiring the vertex
pipeline rather than the blit path.

**What that leaves.** The remaining candidates are the vertex payload contract
(the 16 dwords we push are a guess at what the bound program consumes), the
program image itself (our generated transform has never been validated, and the
canned vendor programs are not transforms), and the `0x10a0`/`0x10a4` reading,
which the two vendor sources disagree on. The DDA/texture-trap path remains
untried and reaches a triangle through the `0x138c` launch already proven here.


## Isolating the failure by packet class (2026-09-05, X stopped, health-gated)

Submitting progressively more of the stream and reading `0x1300` back
afterwards:

| level | stream contents | `0x1300` reads |
|---|---|---|
| 1 | register packets only | **`0x000010cc`** — the value written |
| 2 | + `0x41` constant block-loads | `0x44000000` |
| 3 | + `0x41` vertex quads | `0x44000000` |

**Register writes land.** That is now directly demonstrated rather than assumed:
with only register packets in the stream, `0x1300` reads back exactly what a
ring packet wrote. The tag/data framing is correct.

**Adding any `0x41` packet corrupts it**, and the vertices add nothing beyond
what the constants already do. Restricting the constants to exactly the ten
indices `mtx2209` writes into bank A — dropping `0x176`/`0x177`, which neither
bank writes, and `0x179..0x17c`, which exist only in bank B — makes no
difference, so it is not the bank-B entries.

**The likely reading, and it is consistent with the vendor sources:** a `0x41`
packet is a quad stream into the WARP input FIFO whose *payload contract is
defined by the program currently bound at `0x1100`*. With no valid program
bound, the payload is consumed by whatever the engine thinks is bound and can
land anywhere — including on `0x1300`. On that reading the corruption is a
**symptom of the bind not taking**, not a framing error in `block_load()`, whose
header arithmetic checks out against the confirmed rule for every count used.

**Unresolved and blocking:** the WARP bind registers `0x1000`, `0x1040`,
`0x10b0`, `0x10f0`, `0x1100`, `0x1104`, `0x1110`, `0x1130` all read back **zero**
after being written, while `0x1300` in the same stream reads back correctly.
Either they are write-only — plausible for a bind FIFO — or the bind genuinely
is not landing. Nothing available distinguishes those two, and that is the next
thing to settle: without it there is no way to tell a failed bind from an
unreadable one.


## GROUND TRUTH: a working ioquake3 draw on the P690 (2026-09-05)

ioquake3 runs on this card with the vendor ICD, and `mtxtrace` captured 349 MB
of a live GL session: all **7 regions** mapped, 27 ioctls, 1383 doorbells.
Extract kept at `captures/ioq3-p690-working-draw.txt`.

### The draw packet IS inline in the command ring

Region `0x8993000` (the 672 KB command ring), first submit, verbatim:

    D 000000 8f000001     DRAW header
    . 000004 00000000     first = 0
    D 000008 00000004     count = 4
    D 00000c 00000002     register-list header, 2 registers
    D 000010 21212425     tag: indices 0x25, 0x24, then 0x21 0x21 terminate
    D 000014 00000002     0x1094 = fence value, increments per draw
    D 000018 1a9bc000     0x1090 = fence address (a mapped region)

**This retracts two things.** The claim that the `0x8f` packet must live in a
second-level buffer entered via `0x10a0`/`0x10a4` is **wrong** — it is inline in
the level-1 ring, immediately followed by its fence register-write packet. And
the payload is `first, count`, **not** `first, last`; the ambiguity in
`parhelia-command-construction.md` is resolved in favour of count.

Each ring submit occupies a fresh 8 KB stride (`0x0`, `0x2000`, `0x4000`, ...)
and contains **only** the draw and its fence — 24 bytes changed.

### 3D state does NOT go through the ring — it goes through the record stream

Region `0x788000` (136 KB) carries the state, as `(slot, value)` dword pairs:

    D 000000 00000051     count = 0x51 pairs
    . 000004 00000001
    D 000008 00000002     key = 2
    . 00000c 00000000
    D 000010 0000000c / 0000006c      slot 0x0c = 0x6c
    D 000018 00000007 / 00000000      slot 0x07 = 0
    D 000020 00000008 / 00000003      slot 0x08 = 3   (polygon mode = fill)
    D 000028 00000009 / 00000002
    D 000030 0000000a / 0001ffff
    ...
    D 0000c8 00000025 / 3f800000      slot 0x25 = 1.0f

**This is the structural reason our from-scratch WARP draw does nothing.** The
tool writes hardware registers directly into the DMA ring. That is sufficient
for 2D — the solid fill draws 120000 pixels that way — but the vendor never
programs 3D state that way. It sends **state slots** into the record stream and
the kernel translates them into the register writes and opcode-0x41 block loads
the engine actually needs. Bypassing that skips whatever the translation does,
which `parhelia-state-slot-map.md` already showed is not a simple
`reg = 0x1000 + slot*4` mapping but a shadow structure combined across slots.

So the correct shape for a from-scratch 3D driver is either to reproduce the
kernel's slot translation faithfully, or to drive the slot interface and let the
kernel do it. Emitting registers into the ring and hoping is neither.


## Decoded state slots from the working ioquake3 context

Walking the record stream from `captures/ioq3-p690-working-draw.txt` with the
`[count][a][key][b]` + `(slot, value)` pair grammar:

| record | count | key | slots | non-zero |
|---|---|---|---|---|
| 1 | 81 | `0x02` | `0x07..0x8a` | 22 |
| 2 | 139 | `0x8c` | `0x8e..` | 71 |
| 3 | 5 | `0x01` | `0x02..0x07` | 1 |
| 4 | 8 | `0x09` | `0x0a..0x14` | 3 |
| 5 | 26 | `0x15` | `0x06..` | 19 |

Then the walk breaks at dword 538 (`count` decodes as `0x20002`), and records 2
and 5 show impossible slot ids like `0x1c0003` partway through. **That is the
documented behaviour, not a decoder bug**: region 2 is a `[key][count]` record
stream in which draw, clear, rect and vertex-format records are *not* (slot,
value) pairs and therefore mis-slice under a pair-only reader. A full decode
needs the record grammar, not this approximation.

**What decodes cleanly is still worth having**, and it independently confirms
several documented readings from a *live, working* context rather than from a
binary:

* `slot 0x08 = 3` — polygon mode, and the documented enum is `1=point 2=line
  3=fill`, so a filled polygon. Correct.
* `slot 0x16 = 3`, `0x17 = 4` — cull group, consistent with the documented
  2-bit field where 1 = off.
* `slot 0x9a = 1.0f`, `0x9b = 1.0f`, `0xa6 = 4096.0f` — **the point-size group**,
  exactly as `parhelia-gl-limits.md` places it (`0x9a` size / `0x9b` min /
  `0xa6` max), with `0xa6` at its documented 4096 default.

That last one is an independent confirmation of the `0xa6` correction made
earlier today: it is the point-size maximum, not "guard band / viewport scale".
A working GL context sets it to the value the limits doc predicts.

* `slot 0x3a = 0xffffffff`, `0x3b = 0xffffffff` — masks
* `slots 0x40..0x5f` are present and all **zero** in a working context, which is
  worth noting against the retracted advice to replay `0x1100-0x117c =
  0xffffffff` as "the state a from-scratch draw is most likely to be missing".
  The vendor writes zeros there.


## The region-2 record grammar -- RETRACTED, see the correction below

Derived from the working ioquake3 capture and validated by exact consumption --
five records account for all 857 dwords of the state submit with every boundary
landing on a valid following header.

    record = [count][a][key][0]   then   count x (slot, value) dword pairs
    slot   = (instance << 16) | slot_id

The **indexed slot** is what defeated every earlier attempt to read this stream.
A slot id is 16 bits with an *array index* in the upper half, so an entry for
light 3 or texture stage 3 appears as `0x0006_0003`. Read as a flat 32-bit slot
id that is a nonsense value like `0x1c0003`, which is exactly the "draw, clear
and vertex-format records are not pairs and mis-slice" symptom recorded in
`parhelia-command-construction.md`. **They are pairs after all** -- the reader
was wrong, not the record.

The visible run at dwords 520-571 makes it unambiguous: `0x0002_0001`,
`0x0003_0001` ... `0x0009_0001`, then `0x0001_0002`, `0x0002_0002` ... -- slots
1..9 repeating per instance, with per-instance values `0, 2, 2, 2, 2, 1000.0f,
-1000.0f, 1000.0f, 1.0f`.

Records in a freshly initialised ioquake3 context:

| # | at | count | key | instances | slot range | non-zero |
|---|---|---|---|---|---|---|
| 1 | 0 | 81 | `0x02` | 0 | `0x07..0x8a` | 22 |
| 2 | 166 | 139 | `0x8c` | 0..5+ | `0x00..0xab` | 71 |
| 3 | 450 | 1 | `0x02` | 0 | `0x04` | 0 |
| 4 | 456 | 5 | `0x06` | 0 | `0x07..0x0b` | 3 |
| 5 | 572 | 81 | `0x01` | 0 | `0x07..0x8a` | 22 |

Records 1 and 5 are the same shape with different keys (`0x02` and `0x01`),
which is consistent with two contexts or a front/back pair. Record 2 is the
indexed array -- lights or texture stages, six or more instances.

A parser can therefore validate itself: walk headers, and require each record to
end exactly on another well-formed header or the end of the stream. That check
is what pinned the boundaries here, and it is cheap enough to keep permanently.


## Replaying the working stream: how far it gets

The point of solving the grammar was to avoid reversing the kernel's
slot-to-register translation at all -- drive the slot interface and let the
kernel translate. `tools/parhelia-replay.c` against the ioquake3 capture:

**Works:**

* **7/7 cookies resolvable** on a real working trace, with the same publication
  sites as the older captures (control page `+0x28`, second slot `+0x48`). The
  harness generalises beyond the capture it was written against.
* The ioctl-returned cookie is correctly fixed up -- recorded `0x3a8000` becomes
  the live `0x104c000` -- and that region maps.

**Does not work:**

* `mmap` of the in-band buffers fails **EINVAL**. The live read at `+0x28`
  returns the *recorded* value rather than a freshly published one, so the
  kernel has not actually created the buffer at that point in the replay: the
  in-band creation handshake is being replayed as data rather than performed.
* The replay then **hangs in the kernel** on a doorbell ioctl and the process is
  unkillable (`kill -9` does not reap it).
* The engine is left unable to draw -- the 2D fill health check returns
  **0/120000** afterwards, needing a reboot.

Memory is also a practical constraint: the full 349 MB trace OOMs the 497 MB
box, because the harness holds every event. Filtering the texture-heap submits
(524288 dwords each, and irrelevant to a first triangle) makes it fit.

**What this says.** Cookie resolution -- the thing that blocked this harness for
its whole life -- is solved. The remaining blocker is different and more
specific: **buffer creation is an in-band handshake, not a replayable byte
sequence.** The client writes a request into the control page and the kernel
answers there; replaying the recorded bytes reproduces the question and the
stale answer, but never asks the kernel to allocate. A replay has to *perform*
that handshake -- write the request, ring the doorbell, then read the cookie the
kernel publishes -- rather than replaying both sides of it.


## CORRECTION: the record grammar above is wrong

The grammar was declared solved on the strength of one capture, where five
records consumed all 857 dwords with every boundary landing on a valid header.
A second capture falsifies it.

glxgears on the 0x0528, state submit of 3908 bytes, header:

    0000000d 00000001 00000002 00000000

and then **66 consecutive well-formed `(slot, value)` pairs** -- an unmistakable
run including `0x40..0x5b` all set to `0xffffffff` -- while the header's first
field says **13**. The count field is therefore **not** the pair count.

The exact-consumption check that seemed to validate the grammar is weaker than
it looked: with a loose "is this a plausible header" test, a walk can consume a
stream exactly by chance and still be wrong at every boundary. Applied to
glxgears the same walk yields impossible slots (`0xcccd`, instance 15948) while
still consuming the stream.

**What survives:**

* `(slot, value)` dword pairs are real and directly readable -- the
  `0x40..0x5b = 0xffffffff` run leaves no doubt.
* Records begin with a 4-dword header containing a key field.
* Some records carry an array index in the upper half of the slot word.

**What does not:** the meaning of the header's first field, and therefore where
one record ends and the next begins. That is still open, and it is what a
from-scratch driver needs in order to emit a stream the kernel will accept.

### Two findings from the glxgears capture

**`0xa6` is confirmed a third time.** It reads `45000000` = **2048.0f** here and
`45800000` = **4096.0f** in the ioquake3 capture -- exactly the two values the
limits documentation gives for `GL_POINT_SIZE_MAX` (4096 default, 2048 as
declared). Two different applications, both landing on documented values for
that reading, from live contexts.

**Slots `0x40..0x5b` = `0xffffffff` in glxgears, but `0` in ioquake3.** This is
the slot-level form of the `0x1100-0x117c = 0xffffffff` "attribute file init"
that `parhelia-3d-state-complete.md` recommended and that was retracted here as
a *register* claim. The retraction of the register addresses stands, but the
pattern itself is real at slot level and is **application-dependent** -- so
neither "always write 0xffffffff" nor "the vendor writes zeros there" is correct
as a general statement. Ours earlier said the latter, on one capture.


## Replay hazard: a doorbell can spin forever in the kernel

Replaying a captured trace against the live driver reliably produces an
**unkillable process**: state `R` (spinning in kernel mode, not blocked on I/O),
`kill -9` has no effect, 28 minutes of CPU and counting, and it holds
`/dev/matrox/mtx0` open so no other client can use the device -- which silently
breaks any capture attempted afterwards. Only a reboot clears it.

The explanation is already in this repository: the vendor driver's wait-for-idle
contains a **compiler-hoisted infinite loop**, recorded when the engine-reset
path was investigated. A doorbell rung with control-page state the kernel does
not expect enters that wait and never leaves.

Consequences for a replay tool, and for the open kernel driver:

* **Bound every replay and verify the bound actually fires.** `timeout` does not
  help -- the process is unkillable, so the timeout process simply waits
  alongside it. The bound has to be inside the replay, before the ioctl.
* **Check the device is free before capturing.** A stuck replay holding the node
  makes a subsequent capture produce a 49-byte file with only a header, which
  reads as "the app did not use the GPU" rather than "the device was busy".
  That cost a confusing round here.
* For the open kernel driver this is a specification point, not just a bug to
  avoid: the wait-for-idle **must** be bounded and must return an error, because
  the hardware genuinely does reach states where idle never arrives.


## Address-mode draw tried on hardware (2026-09-05, 0x0528)

The Windows driver never emits `0x8f000001`. Its draw is

    [0x17000001 | flags | ((vertexDwords-1) << 17)] [start] [end]

with the vertices sitting in the command buffer but **excluded from the
submitted range**, so the command processor never parses them. Both encodings
are the same packet class -- `low16 = payloadDwords-1` holds for each -- and they
differ in what the payload means:

* **index mode** (`0x8f`): `[first, end)` indices into a separately bound vertex
  array. Bits [22:17] are zero.
* **address mode** (`0x17|flags`): absolute `[start, end)` addresses, with the
  **stride in bits [22:17]**. Nothing external to bind.

**Every from-scratch attempt in this repository had used index mode with no
array bound**, which is a complete explanation for "the ring consumes every
packet, the engine leaves idle, and nothing rasterises".

### Two results

**Passing a CPU physical address of system memory took the machine down hard** --
no ping, cold boot. The addresses are not CPU-physical: the documented encoding
elsewhere in this hardware is a *byte offset within a pool* with an
address-space tag in the low two bits (0 = local frame buffer). Do not pass a
system address here.

**With the vertices in VRAM and addressed as an offset, the draw is safe** -- the
engine stays healthy, the 2D fill still draws 120000 px afterwards -- and still
produces **0 of 65536 VRAM pages changed**.

So address mode removes the unbound-array explanation without producing
geometry, and the remaining gap is elsewhere: most likely the WARP program
itself (ours is hand-written and has never been validated), or the fragment
program, which per `parhelia-slot-translation.md` is not optional -- texture and
combiner state exist only as a compiled program, and there is no register path
that a draw can fall back on.

## The failure is inside the 3D pipeline, and everything around it is proven

Measured 2026-09-05 on the 0528, engine healthy throughout, no reboots.

Two of the three false-negative causes that plagued this work — looking at the
wrong VRAM offset, and a render target that was never bound — are now
**definitively excluded**, using the only instrument on this card that has ever
been independently confirmed:

    ./3dtri --dmafill --fillrt     green px: at base 0x400000 = 120000
    ./3dtri --dmafill              green px: at base 0x0      = 120000

`--fillrt` points the 2D fill at `g_rtoff`, the same VRAM offset the 3D path
renders to, and it is found by the same scan the 3D path uses. So the surface
base and pitch programming is correct at that offset, and the observation method
sees what lands there.

Independently verified around the 3D draw, each in the same session:

| component | evidence |
|---|---|
| ring, submit, doorbell | `0x404 == end` every run; the 2D fill draws through it |
| engine arm | `0x300` reads back `0x140`, matching the vendor |
| render target base/pitch | 2D fill draws 120000 px at that exact offset |
| VRAM observation | same scan finds the 2D fill |
| microcode upload gates | all four pass, dwords accepted at `0x91c` |
| draw packet framing | consumed to the end, engine returns idle |

And the draw still produces zero fragments. So the fault is **inside the 3D
pipeline proper** — vertex fetch, the transform, or the raster setup — and not
in any of the plumbing around it.

**The one thing still missing is an execution signal.** All three candidates are
now known broken on this card: the WARP readback port shows nothing for any
provable operation, the pixel scan was anchored wrong (fixed, and now proven
correct by the table above), and a DWGSYNC with the block bit never retires.
Without one, "the program ran and emitted nothing" and "the program never ran"
remain indistinguishable, and further register-level variation is enumeration
rather than measurement.

## The block-load packet is what stops the engine, and its framing is unsolved

Measured 2026-09-05 with a **validated** fence (positive control: four exact tag
matches; negative control: unchanged with no packet). See the DWGSYNC section
above — the fence works only with bit 0 **clear**.

Seeding `0x41c` to `0x10` with a 2D run, then bracketing the 3D draw with tags
`0x30` and `0x48`:

| stream | `0x41c` after | meaning |
|---|---|---|
| trilevel 1 (no block loads) | `0x48` | **both tags retired, draw packet executed** |
| trilevel 2 (`warp_init_constants`) | `0x10` | neither retired |
| trilevel 3 | `0x10` | neither retired |
| `--oldconsts` (`warp_min_constants`) | `0x30` | pre retired, post did not |
| one count-4 load, our framing | `0x10` | neither retired |
| one count-4 load, header-less vendor tag `0x4106000f` | `0x10` | neither retired |

`0x404 == end` in every case. **The engine fetches the ring to completion and
does not execute it.** That is not the same failure as "the draw runs and emits
nothing", and every conclusion previously drawn from "packets consumed, engine
idle, nothing drawn" was about the wrong thing.

Two framings tried, both rejected by the hardware:

```
ours:    [0x41000000 | count<<17 | count*4+3][count][start][1][0] + count*4 data
vendor:  [0x41000000 | (count-1)<<17 | count*4-1] + count*4 data
```

For `count = 4` the second produces exactly `0x4106000f`, the literal the vendor
emits in `mtx676` — and it still desyncs. So the tag alone is not the whole
story; the payload shape matters.

**It is not the framing.** `mtx676`'s packet emitted **verbatim** — the exact
`0x4106000f` tag and the exact 16 payload dwords — also stops execution. And it
is not the WARP bind either:

| stream | `0x41c` | |
|---|---|---|
| trilevel 1 + bind, no block packet | `0x48` | executes fine |
| trilevel 1 + bind + verbatim block packet | `0x10` | stops |

So **any `0x41`-tagged packet in our PRIMARY ring stops the engine executing**,
in three framings, with and without a bound program.

**The hypothesis this points at.** The vendor never puts a `0x41` packet in the
primary ring. `mtx676` builds its block packets into the push-buffer object at
`r13+0x60` and submits them through `mtx1937`, which emits a *secondary-call*
packet — `[0x20292820][0][start|tag][end|tag][0]` into `0x10a0`/`0x10a4` — and
rings `0x408` with the primary tail. Every `0x41` the vendor executes therefore
lives in a **secondary buffer**, reached by a call, never inline in the primary
ring. That would explain a packet the engine fetches and refuses to execute
regardless of its contents.

**Tested, and the hypothesis does not survive — but the control is what matters.**
Putting the block loads in the secondary buffer still stops the engine. The
control run then showed why that measurement was meaningless:

| stream | `0x41c` | |
|---|---|---|
| trilevel 1, inline (control) | `0x48` | executes |
| trilevel 1, secondary call, NO block loads | `0x10` | **stops** |
| trilevel 3, block loads in the secondary buffer | `0x10` | stops |
| secondary call with space tag 0 / 2 / 3 | `0x10` | stops in all three |

**Our secondary-call packet itself stops the engine**, so the block-load result
through it says nothing. Two packet types are now known to stop execution in our
ring, independently:

```
0x41......    block load       - 3 framings incl. mtx676's verbatim
0x20292820    secondary call   - 3 address-space tags
```

Both are consistent with `mtx2434`, which lists indices `0x24`, `0x25`, `0x28`,
`0x29` (`0x1090`, `0x1094`, `0x10a0`, `0x10a4`) as **consumed but not written**
by the software DMA interpreter. Every one of those is a register our packets
try to reach and the engine declines to act on — the write-back pair that never
fires, and now SECADDRESS/SECEND. That is unlikely to be a coincidence and is
where to look next: those four indices may require a mode or privilege our ring
does not have.

What executes in our primary ring, proven: register-write packets, the
address-mode draw packet, and DWGSYNC. What does not: block loads and secondary
calls.

**The other clue, still worth following.** `mtx676`'s 16 payload dwords are *not* four vec4s
of data. They are `0xaaaaaaaa`x4, `0x55555555`x4, then `0, 0x40, 0x80, 0xc0,
0x100, 0x140, 0, 0` — two vec4s of data followed by six values spaced `0x40`
that look like destination indices, and two zeros. So a block load may carry
**data plus a scatter list of destinations**, not a base index and a run. That
would explain why a base-index header is rejected: there is no base index.

This is now the single blocking unknown for a triangle, it is well localised,
and there is a working instrument to test candidates against: put a candidate
packet in the ring behind a DWGSYNC and see whether the tag retires.
