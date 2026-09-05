# WARP register readback port, and how to trust it

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

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
