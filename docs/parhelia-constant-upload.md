# How constants reach the WARP

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

> **RETRACTION (2026-09-05, later the same day).** An earlier version of this
> file concluded that "the kernel never emits a block-load packet", on the
> grounds that `0x4106000f` is the only `0x41……` immediate in the object. That
> reasoning was wrong: **the header is computed at runtime**, so an
> immediate-scan could only ever find the one hard-coded self-test literal. The
> emitter exists and is `mtx1342`. The DMA hypothesis built on the old
> conclusion is also withdrawn — `MtxRegion_GetPhys` is called exactly four
> times in the whole object and never for the constant area, so nothing DMAs
> the constant file.

## The emitter: `mtx1342` (`.text+0x90010`)

Walks the 384-bit dirty bitmap at `constobj+0x04`, finds runs of contiguous
dirty slots, and writes one packet per run straight from `constobj+0x230`:

```c
n = last - first;              /* vec4s in this run */
if (n >= 0x40) n = 0x3f;       /* 63 max: the quad field is 6 bits */
hdr = 0x41000000
    | ((n << 17) & 0x007e0000) /* quads following - 1 */
    | ((n*4 + 3) & 0xffff);    /* dwords following - 1 */
ring[0] = hdr;
ring[1] = n;                   /* count */
ring[2] = first;               /* first slot, 0..383 */
ring[3] = 1;
ring[4] = 0;
memcpy(&ring[5], &constobj[0x230 + first*16], n*16);
```

Called only from `mtx693+0x2f5` (`.text+0x43995`), the per-draw emitter, which
then rings the doorbell through `mtx1937`.

**That framing is byte-for-byte what `tools/parhelia-3dtri.c`'s `block_load()`
has emitted since the beginning.** The format was never the problem.

## The universal packet header

```
header = opcode | mode | ((quads_following - 1) << 17) | (dwords_following - 1)
```

Confirmed across seven builders — `0x41` block loads (three variants),
`0x17`/`0x03`/`0xd7` draws, `0x0f` control. Checked against the self-test
literal: `0x4106000f` gives 16 dwords and 4 quads, and `mtx676` writes exactly
17 dwords. The 6-bit quad field is why `mtx1342` clamps a run to 63.

## The missing piece: the destination-select preamble

Every caller emits this immediately **before** a block load:

```
[2][0x21211000][0x00000000][0x00000004]                  0x1000 = 0, 0x1040 = 4
[3][0x212c1000][0x00000000][0x00000004][0x0002305c]      + 0x10b0 = 0x2305c
[4][0x403c3cdd][mode][mode][desc]                        0x1374, 0x10f0, 0x10f0, 0x1100
```

Without it the engine has no destination for the payload. **This explains the
hardware results exactly**: a count-1 packet "executed" because it is a
degenerate case, every multi-vec4 form stopped the ring, and `mtx676`'s verbatim
packet failed too — it carries its own descriptor words (`0xaaaaaaaa`) as test
data and no preamble.

`0x1000`, `0x1040`, `0x1100`, `0x1104`, `0x1110` and `0x1130` are **never
written by MMIO anywhere in the object** — they are ring-only registers, which
is why every readback of them returned 0. The sole MMIO write near the block
file is `0x10b0 = 0x10` at chipset init.

## CONFIRMED ON HARDWARE (2026-09-05)

The preamble is what block loads needed. Measured with the validated
non-blocking DWGSYNC bracket, tags 0x30 / 0x48:

| stream | `0x41c` | |
|---|---|---|
| trilevel 1, no block loads (control) | `0x48` | executes |
| **trilevel 2, block loads WITH preamble** | **`0x48`** | **executes — first time** |
| trilevel 2, block loads without preamble | — | engine stops, as before |

**Multi-vec4 block loads execute for the first time in this project.** The
framing was always right; the destination-select preamble was the missing
piece, exactly as the disassembly predicted.

### A hardware caveat found immediately after

Following that run the card entered a state where **the 2D fill draws 0 pixels
even across a warm reboot**, with `bmstat` reading `0x02020000` (bit 25 set)
where it read `0x00020000` all session, and `0x1300` powering up at `0x009600c8`
instead of `0x002004c2`. Matrox registers are known to survive a warm reset, so
this needs a **cold power cycle**, not `reboot -f`. Do not interpret a
zero-pixel 2D fill in that state as a driver result — it is the instrument
being broken, and the first thing to check after any reboot is that the 2D
baseline still draws 120000.

## `constobj` layout

| offset | contents |
|---|---|
| `+0x00` | word-presence mask |
| `+0x04..+0x33` | 12-word dirty bitmap = **384 slots** |
| `+0xa0/+0xa4/+0xa8` | committed / head / tail |
| `+0xb0..+0x22f` | 6 descriptors x 0x40 |
| `+0x230..+0x1a2f` | **384 x vec4 = 6144 bytes, the constant file image** |

384, not 256 — the client upload path (`mtx558` entry 67) clamps to the low 256,
but the file is 384.

## What an open driver emits

Per contiguous run of at most 63 vec4s: the preamble above, then

```c
hdr = 0x41000000 | ((n << 17) & 0x007e0000) | ((n*4 + 3) & 0xffff);
{ hdr, n, first_slot, 1, 0, /* n*4 float dwords */ }
```

then the doorbell. None of the kernel's shadow, dirty-bitmap or compaction
machinery is needed — that is all bookkeeping for deciding *which* runs are
dirty.

## Also settled

* `0x384` is written **100 times** as a PCI write-posting barrier, always `0`,
  gated on the Intel host-bridge check in `mtx3376`. It is not a register in any
  meaningful sense — filter it out of captures.
* A second path to the same file exists: `0x10b4` = write pointer, `0x10b8` =
  data port, used by `mtx3355` and `mtx676`.
* `mtx_drv.so` uploads **no** WARP constants at all — a clean negative.
