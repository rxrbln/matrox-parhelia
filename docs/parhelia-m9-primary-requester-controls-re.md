# Matrox M9 primary requester control packing

This note closes the numeric packer behind primary requester MMIO registers
`0x44c`, `0x450`, and `0x454`.  The evidence is the saved Linux vendor kernel
object `KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.
Run `python3 tools/m9-primary-requester-control-evidence.py --self-test` to
recheck the binary slices and arithmetic vectors.

## Producer and consumer

Kernel constructors `m9x654` at `.text+0x2c930` and `m9x1086` at
`.text+0x2cd40` are byte-identical across their complete `0x408` bytes.  Each
zeros object dwords `+0x6c`, `+0x70`, and `+0x74`, reads two property tuples
through device fields `+0x5f0` and `+0x5f8`, validates them, and packs the
three dwords byte by byte.  Their common control slice has SHA-256
`07974c81a40cb4d93d94761b15a77d0f806836c6843fc8fd8243f6f712ac74eb`.

Initializer `m9x109` at `.text+0x2d150` later consumes those dwords without
transformation:

| object field | MMIO register |
|---:|---:|
| `+0x6c` | `0x44c` |
| `+0x70` | `0x450` |
| `+0x74` | `0x454` |

This closes the earlier description of three opaque DMA controls.  Their
individual semantic names remain neutral because the property structures are
not symbolized.

## Exact packing

Let the primary property tuple be `(A,B,C,D)` and the secondary tuple be
`(a,b,c)`.  The constructor accepts `A,B,C` only when all are nonzero and
`A+B+C <= 64`; otherwise it substitutes `(8,32,24)`.  It accepts `D` in
`1..32`, otherwise substituting `32`.  It accepts `a` in
`1..min(A,4)`, `b` in `1..min(B,8)`, and `c` in `1..min(C,8)`; invalid
secondary values become `4,8,8` respectively.

The emitted controls are:

```text
0x44c =             (A+B)<<16 | A<<8
0x450 = (D-1)<<24 | (A+B+C-1)<<16 | (A+B-1)<<8 | (A-1)
0x454 = (c-1)<<16 | (b-1)<<8 | (a-1)
```

All fields are bounded before packing, so the additions fit their recovered
five- or six-bit byte fields.  No address or host pointer participates.

The exact fallback image is:

```text
0x44c = 00280800
0x450 = 1f3f2707
0x454 = 00070703
```

The first two words describe cumulative boundaries for the `8+32+24=64`
partition.  The third selects `4,8,8` subcounts.  Those structural names are
data-flow descriptions, not claims about the physical execution-unit names.

## Target validation and remaining boundary

The static packer and safe fallback are closed.  A read-only snapshot of the
tested M9148 (`102b:0540`, subsystem `102b:2240`, revision 1) after the
clean-room no-op-fence tests found the device unbound with PCI bus mastering
disabled.  Three consecutive samples read exactly
`00280800/1f3f2707/00070703`, proving that this board's warm controls match the
fallback tuples.  The pinned fixture is
`captures/m9x-requester-live-20260914/warm-unbound-registers.txt`.

This target result does not establish that every M9 product uses the fallback.
General cold initialization should join `+0x5f0/+0x5f8` to the board/device
database that supplies them, preserve verified warm values, or guard this
fallback to the tested product.  Kernel commit `d545438` now reads all three
controls before enabling bus mastering and fails closed unless this M9148 has
the exact verified image.  It never overwrites the firmware values.  The
resulting module completed a fresh hardware no-op fence at `0x10`, unloaded
cleanly, and left the device unbound with `BusMaster-`.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
