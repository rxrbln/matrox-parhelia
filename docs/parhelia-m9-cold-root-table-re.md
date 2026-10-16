# M9148 cold root translation table

> **Correction (2026-09-15):** both `m9x421` calls are MMIO window writes
> only on the cold path; the packet
> half is gated on `B+0xc8` and does not run before `m9x182`.  Step 10's
> leaf publication is followed by `314=1` (`.text+0x37a4b`).  See
> `parhelia-m9-cold-transition-submit-decode.md`.

## Result

The Linux vendor manager constructor builds a translation level which the
clean-room DRM driver initially omitted.  The table lives in the low-VRAM
range selected by MMIO `0x360/0x364` and maps every page of the vendor root
allocation back to the same VRAM page.

For the captured M9148 profile:

```text
root allocation:     [0x000000,0xd41000)
root page count:      0xd41000 >> 12 = 0xd41
table child:          [0x861000,0x8a1000)
initialized bytes:    [0x861000,0x864504)
entry[i]:             (i << 12) | 0x8, i=0..0xd40
first entry:          0x00000008
last entry:           0x00d40008
```

This is separate from the compact leaf table rooted at `0x801000`.  System
resource GPUVA `A` selects its leaf at:

```text
leaf address = 0x801000 + 4 * (A >> 12)
```

For the first-triangle secondary buffer at GPUVA `0x40024000`, the leaf is at
VRAM `0x901090`.  That address is on low-VRAM page `0x901`, so the root lookup
which makes the leaf page reachable is:

```text
root entry address = 0x861000 + 4 * 0x901 = 0x863404
root entry value   = (0x901 << 12) | 0x8 = 0x00901008
```

An early clean-room run read the expected leaf value from `0x901090` using the
CPU BAR mapping, but the root entry at `0x863404` was zero.  Adding the table
removed that concrete mismatch.  It did not by itself make a class-1
secondary fetch retire, so the zero root entry explained one invalid state but
was not the final execution blocker.

A later live BAR snapshot after exact root construction and per-allocation
leaf publication read:

```text
root[0]       @ 861000 = 00000008
root[901]     @ 863404 = 00901008
root[905]     @ 863414 = 00905008
root[d40]     @ 864500 = 00d40008
leaf[400000]  @ 901000 = 0595400a
leaf[400020]  @ 901008 = 04d8000a
leaf[400240]  @ 901090 = 04d7800a
leaf[410000]  @ 905000 = 02a0000a
```

The DMA page values vary per boot.  In the same boot a class-2 raw-DMA
secondary range retired immediately, while the same footer through class-1
GPUVA `0x40024000` did not.  This proves the CALL engine and secondary framing
work and isolates the remaining failure to compact-translation selection or
activation rather than the CPU-visible table image.

The clean-boot test of kernel `b788cc4` repeated that boundary after applying
all three corrected construction rules: both cold `m9x421` calls were
MMIO-only, the restore tuple used tag one, and each mode-1 publication ended
with `31c=f,1,0` followed by one `314=1`.  The direct primary no-op retired at
target `0x10`.  The canonical class-1 secondary then timed out at target
`0x20` with:

```text
writeback        = ffffffdf
engine/frontend  = 00000000 / 00000000
kick             = 00000000
command          = 04b90002
fences           = 04bd5006 / 04bd5002
restore tuple    = 41000201 / 41000200 / 4117fc06
secondary        = GPUVA 40024000, DMA 05dd0000
secondary leaf   = 05dd000a (expected 05dd000a)
```

The corrections are therefore present but are not sufficient to make compact
secondary fetch retire.  The remaining gate is below command framing and
above the CPU-visible leaf contents.

## Binary proof

The source is `KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.

Cold `m9x53` setup at `.text+0x1889f..0x1891f` reads the root allocation size
from the object reached through its fourth argument, shifts it right by 12,
stores `0xd41` in descriptor field `+0x38`, selects manager child `M+0x70`,
and calls `m9x328` with selector zero.  The fresh builder has no upload stream,
so `m9x328` takes its CPU-mirror fallback and writes the sequential `|0x8`
entries through the child CPU mapping.

The manager children and construction order are:

| Child | GPU interval | Size | Cold action |
|---|---:|---:|---|
| primary | `0x801000..0x861000` | `0x60000` | clear with `m9x1027` |
| range/root | `0x861000..0x8a1000` | `0x40000` | build identity table with `m9x328` |
| auxiliary | `0x8a1000..0x8e1000` | `0x40000` | clear with `m9x1027` |
| tail | `0x8e1000..0x941000` | `0x60000` | clear with `m9x1027` |

The two latter `m9x1027` calls run because both child sizes are nonzero.  On
this fresh path their `m9x304` upload helper returns immediately and both
operations reduce to CPU clears.  Their selector-5 pre/post lists are empty.

The exact relevant order is:

1. Publish the first `m9x421` MMIO window tuple; emit no transition packets.
2. Clear the primary child.
3. Build the `0xd41`-entry root identity table.
4. Order the BAR/WC writes with `wmb()` and write `0x31c = 0xf, 1, 0`.
5. Write `0x30c = 0x1000`.
6. Clear the auxiliary and tail children.
7. Order the clears and write `0x31c = 0xc, 1`.
8. Apply `System\\TlbPrefCnt=1` in `0xf0c`.
9. Publish the second `m9x421` MMIO window tuple; emit no transition packets.
10. Publish later system-resource leaf PTEs through the separate `m9x887` to
    `m9x328` path, ending with `0x31c = 0xf, 1, 0`, then one `0x314 = 1`.

Every later `m9x887` allocation has its own CPU mirror and `0xf,1,0`
publication.  The vendor does not batch independently allocated leaves under
one final activation.

The surrounding service owner also calls `m9x115` once during initial service
construction and twice at the final `m9x1141`/explicit-`m9x115` boundary.
With all 20 software slot pointers null, each call writes only dword zero of
MMIO slots `0x000,0x010,...,0x130`, setting it to `0x0000ffff`.  No active
descriptor entry is constructed for the ordinary mode-1 BmBuffer path.

Publishing resource leaves before the cold tail clear is incorrect because
the tail child contains those leaf PTEs.  It also reverses the vendor's split
between manager construction and later resource publication.

## Clean-room implementation

The kernel implementation belongs in a distinct cold manager helper between
the two MMIO window publications.  It uses
`iosys_map_wr()`/`iosys_map_memset()` for the BAR mapping and `wmb()` before
the MMIO activations.  `dma_wmb()` is not the right primitive for these WC
VRAM writes.  Dynamic compact leaf PTEs are committed only after window
publication two; each allocation gets its own `31c=f,1,0` and following
`314=1` before the requester is armed once.

This result is specific to the captured 256 MiB M9148 descriptor.  Other M9
memory sizes require their own recovered manager layout rather than silently
using these fixed intervals.
