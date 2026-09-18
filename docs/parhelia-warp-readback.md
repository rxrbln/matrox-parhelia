# WARP register readback port, and how to trust it

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

> **The "never been shown to observe anything" section below is superseded.**
> `parhelia-p690-warp-isa-re.md` §7 has a verified constant readback on the
> P690: the port is addressed per component, `0x0910 = (3 << 24) | (comp << 16)
> | (bank << 9) | slot`, `0x0938 = 0xfffffffc`, then four settling reads of
> `0x0914` (`parhelia_warp.c:683`). This file's scans used one address per vec4
> and so read three components' worth of nothing. What still stands: **kind 3
> only**, and the port addresses the WARP constant/source file — a vertex-unit
> resource. It is not a window onto engine status or statistics; see
> `parhelia-occlusion-query-re.md` §1.3.

The card exposes a CPU-side readback path into the WARP register space.  It is
the only instrument we have that observes engine state without rendering, so
everything decoded through it depends on knowing exactly what it can and cannot
see.  This file records that calibration, measured on hardware (0x0528, box 56).

## The port

    wr32(0x910, (kind << 24) | (((bank << 9) | index) & 0xfff));
    wr32(0x938, 0xfffffffc);
    for (i = 0; i < 4; i++) out[i] = rd32(0x914);

`0x914` returns the same value on all four reads: it does not auto-increment,
so a single access yields one 32-bit word, not a vec4.  Treat the four reads as
one value read four times.

## Calibration: the null control

A probe that submits a command and then diffs the register file against a
baseline **must** be run a second time with the submit suppressed
(`--nullprobe`).  Measured, on an idle engine, with no submit at all:

    const-probe: 65 changed registers, bmstat=00020000

The same 65 registers "change" with nothing submitted.  They are confined to
`kind=2`, banks 1 and 2, index >= 144, and the changes are single-bit flips that
go in both directions between reads:

    kind=2 bank=1 r144: fffffffb -> fffff7fb
    kind=2 bank=1 r145: fffff7fb -> fffffffb

Near-all-ones values with a wandering bit are an undriven read, not storage.
**Those registers are unstable and carry no information.**  The remaining ~99.5%
of the scanned space (8 kinds x 4 banks x 384 indices) is stable across a
re-read and is usable.

Rule: no diff over this port is a finding until the null control has been run in
the same session and the changed set is disjoint from the unstable set.

## CONFIRMED: the block-load packet framing is correct (2026-09-05)

With the ring in system memory (one page, physically contiguous by definition,
obtained through `dma_sysbuf`) a nine-dword block-load packet submitted with

    wr32(0x404, start); write_barrier(); wr32(0x408, end | 3);

left the fetch pointer at exactly the end address:

    const-probe: ring at phys 0x1aafe000
      submitted 9 words [1aafe000..1aafe024), bmstat=00020000 fence=ffffff8f
      fetch ptr 0x404=1aafe024 == end, framing OK

The engine consumed the packet and stopped where it was told, and stayed idle.
This is the first hardware confirmation that the block-load packet header

    0x41000000 | (count << 17) | (count * 4 + 3)

is understood by the engine, and it holds independently of anything the readback
port does or does not show.  Reproduced across separate runs and ring addresses.

Note the `dma32buf` module is NOT usable on box 56: the checked-in `.ko` was
built for the development machine's kernel (7.1.3-t2), not the box's 2.6.30, and
the box's kernel build tree is incomplete so it cannot be rebuilt there.  The
single-page `dma_sysbuf` path is what works.

## The kind field: only kind=3 is defined, and scanning the rest locks the engine

`mtx514` (`.text+0x8f7c0`) is the port accessor, and it has **exactly one caller
in the whole vendor object**: `mtx612` (`0xa0120`), the self-test verifier, which
always passes `kind = 3`.  Internally kinds 0/1/3/5 take one path and kinds
2/4/8/9/10 take another.

That retroactively explains the lock-ups.  The broad scan here swept kinds 0..7,
and the "noise" it produced sat in **kind=2** -- one of the other-path kinds.
Restricted to kind=3 the space is perfectly stable:

    const-probe: baseline 1536 registers, 0 unstable and masked

Zero unstable, against 65 "changing" registers when kind=2 was included.  The
instability was never a property of the port; it was the result of driving it
with kind values the vendor never uses.  **Scan kind=3 only.**

## MEASURED: a block load leaves no trace in the kind=3 space **[HW, P690]**

With framing confirmed in the same run:

    const-probe: baseline 1536 registers, 0 unstable and masked, bmstat=00020000
    const-probe: writing marker to block 0x134
      fetch ptr 0x404=18dfb024 == end, framing OK
    const-probe: 0 changed stable registers (of 1536 stable), bmstat=00020000

The engine consumed the packet and stopped exactly at the end address, yet
nothing in the entire kind=3 readback space changed.  A targeted probe of the
two candidate mappings for block index `0x134` -- `0x134` itself (port indexes
the block file) and `0x054` = r84 (port indexes the register file, the
`N - 0xE0` rule) -- also showed no change on any of the four banks.

Two readings remain open, and this run does not separate them:

1. The block load needs further arming before it commits.  A candidate is the
   WARP vertex-shader mode enable, bank 0x05 index 0x12, which the vendor's VS
   path emits as `(0x12, 1)` while both fixed-function paths write `(0x12, 0)`.
2. `kind=3` does not observe the constant/source file at all.  This is the more
   likely of the two: `mtx612` uses kind=3 to verify the self-test's **port-A
   destination** writes (`a0..a383`), so kind=3 plausibly reads the destination
   file, and the vendor never reads a constant back through this port anywhere.

Incidental observation: the four banks alias in pairs (bank0 == bank2 and
bank1 == bank3 for every index tested), so bit 1 of the bank field appears to be
ignored on this part.

## THE PORT HAS NEVER BEEN SHOWN TO OBSERVE ANYTHING (2026-09-05, P690)

**This retracts the interpretation of every "N registers changed" result in this
file and elsewhere.** The port has no positive control. Measured, on a healthy
engine, with no lock-up in any run:

| stimulus | provably happened? | port response |
|---|---|---|
| 2D solid fill | **yes** -- 120000 pixels drawn | 0 changes, all 8 spaces x 4 banks x 384 |
| block load, framing confirmed (`0x404 == end`) | packet consumed | marker absent from all 12288 locations |
| block load **with a program bound**, same stream | packet consumed | marker absent |
| `mtx676` self-test replicated: `mtx2785` streamed, bound, four walking-pattern packets | stream accepted, engine went busy | 0 changes |

The 2D-fill row does not prove the port is broken -- a 2D fill does not use the
WARP, so a blind port and an uninvolved WARP look identical. But taken together
with the other three, **there is no operation we can provably execute that this
port reflects.**

Consequences:

* "0 WARP registers changed -- no evidence the program ran" means **the
  experiment was not measured**, not that the WARP is idle. Earlier statements
  in this repository that the WARP "is not executing" rest on this port and are
  withdrawn.
* Until a positive control exists, the port cannot be used to accept or reject
  any hypothesis about program execution.

What would constitute a positive control: the vendor GL stack drawing a frame
with a diff taken around it, since that provably exercises the WARP. That is
currently blocked -- `/dev` is tmpfs and `mtx` registers no character-device
major while loaded, so no GL client can open the device. `init_module` does call
`register_chrdev` with the major taken from a global (a module parameter,
default probably 0 = dynamic), so the registration exists; why it is absent from
`/proc/devices` was not traced.

Until then, **use VRAM as the observable, not this port.** The 2D fill shows
VRAM is a reliable witness on this card.

## What has NOT been shown

Uploading a marker vec4 through a block-load packet produced no change outside
the unstable set, and the marker bit pattern (`0x449a5000` = 1234.5f) appeared
nowhere.  This does **not** establish that the constant store is invisible to
the port, because in the same run the submit did not complete correctly -- see
below.  The question of which readback address the block-store index maps to is
still open.

## The VRAM-resident command ring does not work

Pointing the ring at the framebuffer BAR (`--physbuf 0xd8000000`, i.e. offset
0x08000000 into BAR0) and submitting:

    wr32(0x404, start); write_barrier(); wr32(0x408, end | 3);

triggers the engine, but it does not stop at `end`.  Reading `0x404` back
afterwards gave `d81004e4` -- more than 1 MB past the end of a 9-word packet.
The engine ran on through VRAM interpreting whatever it found as commands and
locked up (`bmstat=03000000`, `dwgctl=fdf2f75f`, steady, not transient).

The likely cause is that BAR writes are write-combining, so the packet header
was not yet in memory when the engine fetched it; a garbage length field then
consumed an arbitrary span.  The working 2D fill recipe uses a GFP_DMA32
*system memory* buffer, which is what the ring should use.

Consequences:
  - A locked-up engine is persistent.  It survives `rmmod`/`modprobe` of mtx and
    an X restart; only a reboot clears it.
  - `bmstat` sampled once at process start is not enough.  Sample it repeatedly:
    a steady `03000000` is a locked engine, and any submit issued afterwards is
    void, which silently invalidates the run's result.

## Ring buffer requirements

`dma_sysbuf()` originally validated only the first page of the span, then
assumed the following pages were physically contiguous -- anonymous memory
offers no such guarantee, so a 64 KiB ring could straddle a discontinuity and
send the engine to an unrelated physical page.  It now walks every page of the
span and requires `phys(page n) == phys(page 0) + n * pagesize`.

It also mmap'd and mlock'd a 512 MB scan region, which cannot succeed on this
497 MB box; the region is now 32 MB.  For a guaranteed-contiguous sub-4 GB
buffer, prefer the `dma32buf` module (`--dmadev`).

## Procedure for any readback experiment

1. Verify `0x8f0 == 0x00020000` (idle) *and* sample it more than once.
2. Baseline the whole readback space.
3. Run the experiment.
4. Re-read and diff.
5. Re-run with `--nullprobe` and subtract the unstable set.
6. Confirm the submit actually completed before believing a negative result.
