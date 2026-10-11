# The engine init the X driver performs on first start

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Read out of `xserver/7_0_0/mtx_drv.so`, function `mtx1373`. That binary is a
far better RE target than the kernel object: it exports real symbol names
(`HALInitHardware`, `HSLDACInit`, …) and its register accessors are named
`ClientWriteRegisterDword` / `ClientReadRegisterDword`, so every write is
trivially attributable.

**Why this matters.** Our tooling has only ever driven a card that saw
`modprobe mtx` and then our own register writes. A 2D solid fill works that way
because it needs almost nothing. A `DWGSYNC` with bit 0 set (block until
complete) never completes and leaves `0x1300` stuck at the `0xc07` flush value,
and a 3D primitive emits no fragments. That is the signature of an engine that
was never brought up, and this is the sequence that brings it up.

## The sequence

```c
write(0x08fc, 0);                               /* hardware DMA, not emulated */
write(0x0920, read(0x0920) & 0x7fffffff);       /* clear bit 31, WARP mode word */
write(0x10b0, 0x40);
write(0x12b8, 0x00070601);
write(0x12b0, 0);

for (i = 1; i <= 0x13; i++) {                   /* 19 aperture descriptors */
    r = i << 4;                                 /* 0x0010 .. 0x0130 */
    write(r, read(r) & 0x0fffffff);             /* clear the top nibble = disarm */
}

write(0x0000, 0x10000000);
write(0x0004, 0x00000000);
write(0x0008, 0x10000020);
write(0x000c, 0x00010001);

write(0x03a8, read(0x03a8) | 0x20);
```

`mtx2069` and `mtx1875` additionally write `0x0300 = 3` and `0x0308 = 3`.

## What we were getting wrong

| register | vendor | us, before this |
|---|---|---|
| `0x08fc` | `0` | **never written** |
| `0x0920` | `read & 0x7fffffff` | written as a raw mode value, bit 31 untouched |
| `0x10b0` | `0x40` at init | `0x10` |
| `0x12b0` | `0` | never written |
| `0x0000`–`0x000c` | the aperture base block | never written |
| `0x0010`–`0x0130` | top nibble cleared on all 19 | never written |
| `0x03a8` | `read \| 0x20` | never written |

**`0x08fc` is the one to look at first.** It is the same register the kernel's
software DMA interpreter `mtx2434` keys on — that function is the emulation path
taken when the shadow of `0x8fc` says so. If the engine is not in hardware
bus-master mode, packets can be fetched and retired without any of their side
effects reaching the drawing pipeline, which is exactly the behaviour we have
been unable to explain.

**`0x10b0` is written twice with different values and both are right.** `0x40`
here at init, and a per-draw primitive word later (`parhelia-minimal-triangle.md`).
Treating the init value as the per-draw value — which our tool did — leaves the
primitive type unset on every draw.

## Also decoded: the kernel's 22-step device start table

`.rodata+0x1860`, `{init_fn, cleanup_fn, name}` at 0x18 bytes each, walked
forwards on start and backwards from the last completed step on teardown.
Recorded in `parhelia-draw-recipe.md` §6 as open item 2, "a from-scratch
bring-up needs to know what the other 21 program". They are:

| step | init | cleanup | | step | init | cleanup |
|---|---|---|---|---|---|---|
| 0 | `mtx1893` | `mtx3332` | | 11 | `mtx1007` | `mtx2927` |
| 1 | `mtx521` | `mtx3008` | | 12 | `mtx3282` | `mtx602` |
| 2 | `mtx824` | `mtx2234` | | 13 | `mtx1625` | `mtx1584` |
| 3 | `mtx501` | `mtx1480` | | 14 | `mtx381` | `mtx3650` |
| 4 | `mtx3277` | `mtx550` | | 15 | `mtx1515` | `mtx1875` |
| 5 | `mtx1543` | `mtx2665` | | 16 | `mtx298` | `mtx2726` |
| 6 | `mtx2460` | `mtx1935` | | 17 | `mtx3722` | `mtx3826` |
| 7 | `mtx435` | `mtx888` | | 18 | `mtx926` | `mtx2819` |
| 8 | `mtx1987` | `mtx2865` | | 19 | `mtx3810` | `mtx3191` |
| 9 | `mtx1025` | `mtx603` | | 20 | `mtx3987` | `mtx2911` |
| 10 | `mtx2253` | `mtx994` | | 21 | `mtx3335` | `mtx1176` |

**The useful result is negative:** only step 19, the WARP unit, touches
registers at all (`0x920`, `0x384`, then the `mtx676` self-test). Every other
step is C++ object construction — `operator new`, allocator and list setup. So
the register-level bring-up an open driver needs is **not** in the kernel's
start table; it is in the X driver, above.

## Status

Implemented as `engine_init()` in `tools/parhelia-3dtri.c`, selected with
`--engineinit`. **Not yet run on hardware** — the test box wedged before this
could be tried. The first three things to measure once it is back:

1. does `--engineinit` make a `DWGSYNC` with the block bit retire, i.e. does
   `0x41c` come back holding the tag;
2. with the fence working as a validated instrument (both a positive and a
   negative control this time), does the 3D draw packet execute;
3. then the six-way `0x1000` / `0x10b0` sweep the two static readings disagree
   about.
