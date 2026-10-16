# Matrox M9 blocked texture layout: recovered address calculation

This note traces the numeric class-3 texture layout from the CPU-visible
staging representation to the physical tiled byte address. The Windows
user-mode driver delegates the conversion to a GPU copy, but the unstripped
Linux kernel core contains an independent CPU readback path with the exact
coordinate-to-byte calculators. That second path closes the physical layout
gap which was previously thought to require a VRAM capture.

The checked binaries are
`xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`,
and `m9xdriver-x86_64-1.4.3-80847-20140908-build_98/kernel/`
`KernelLibraries.o_shipped`, SHA-256
`bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7`.
Run `python3 tools/m9-blocked-layout-evidence.py` from the repository root to
verify the byte sequences, symbols, relocations, and formula vectors below.

## Class 3 is accessed through linear staging

The resource-to-descriptor join already establishes that resource source enum
`+0x1e8 == 1` becomes descriptor storage/layout class `+0x13c == 3`. The
ordinary 2D constructor at `0x51c39..0x51c50` tests that same source enum and
sets staging flag `+0x2b8 = 1`. When `+0x234` is zero it also sets `+0x2c4`,
selecting allocation of a temporary staging buffer on the first map.

The constructor computes ordinary linear byte geometry [BIN][DER]:

```text
one resource branch:
    staging_pitch = align(width, 4) * bits_per_element / 8
    staging_size  = align(height, 4) * staging_pitch

other resource branch:
    staging_pitch = align(width * bits_per_element / 8, 2)
    staging_size  = height * staging_pitch
```

The first formula is at `0x51c5e..0x51c89`, the second at
`0x51c91..0x51cb5`, and both store the pitch at `+0x2b0`. Per-level and
volume constructors repeat the same selection at `0x52ddc..0x52e4a`,
`0x539e4..0x53a4d`, and `0x54534..0x5459c`. This is construction evidence
for a linear CPU representation, rather than evidence for the physical
class-3 permutation.

The common row-pitch helper `0x4d1f0` closes the consumer join. It returns
`+0x290` for staging mode `+0x2a4`, `+0x2b0` for class-3 staging mode
`+0x2b8`, and only then falls back to resource-layout pitch formulas.

## The CPU fallback does not swizzle

The clear fallback `0xb5a80` chooses map helper `0x561b0` when `+0x2b8` is
set (`0xb5ba8..0xb5bba`). With `+0x2c4` set, `0x561b0` allocates the
page-aligned temporary buffer and returns its CPU pointer. Unlock chooses
`0x56430` at `0x55a77..0x55a87`; its temporary-buffer branch submits the
staging record at `0x5644e..0x56460` before freeing it.

After mapping, the fallback obtains `element_bytes = +0x1a0 >> 3` at
`0xb5b41..0xb5b4f` and constructs the first address at
`0xb5c33..0xb5c4e` [BIN][DER]:

```text
address = mapped_base + x * element_bytes + y * staging_pitch
```

The inner loop advances one logical element and the outer loop adds the
staging pitch (`0xb5d62..0xb5d78`). The table at `0xb5e38/0xb5e50` merely
dispatches element widths 1, 2, 4, 8, and 16 bytes to five masked writers.
Unsupported widths select the no-write/increment case. There is no coordinate
permutation or CPU swizzle table in this complete fallback.

## The GPU upload keeps logical coordinates

The normal resource-copy path reaches `0x56a10` or `0x56c60`, then transfer
builder `0x1b8d0`, and core emitter `0x1b650` [BIN][DER]. In the representative
`0x56a10` path:

* `0x56a3b..0x56a63` copies the four caller rectangle coordinates unchanged;
* `0x56a71..0x56a88` adds only the separately recovered packed-mip-tail X/Y
  origin when that special case is active;
* `0x56af0` clips the logical rectangle, and `0x56b66` calls `0x1b8d0`.

The transfer builder obtains source and destination descriptor/address
records through `0x1b420` at `0x1b8e9` and `0x1b8f4`. It copies the four
logical rectangle dwords again at `0x1b9b3..0x1b9d6`, then calls `0x1b650` at
`0x1b9f5`. The core emitter packs logical endpoints and emits the resource
descriptor state. It contains no CPU calculation of a destination byte offset
from X and Y.

This producer/consumer trace proves that the user-mode driver delegates the
linear-to-blocked conversion to the GPU transfer operation. A whole-image scan
additionally finds none of the standard `0x55555555`, `0xaaaaaaaa`, or
`0x33333333` Morton expansion masks in `MxMD.dll`; that absence is only
corroborating evidence, because a different implementation need not use those
masks.

## Linux reverse-read path contains the physical calculation

The overlooked implementation is in the unstripped Linux kernel object.
`m9x101` at text `0x24e60` is a CPU surface-copy/readback routine. It obtains
the surface at request `+0x08`, reads two address-configuration values, and
calls `m9x745` at relocation `0x24f34`. `m9x745` first calls initializer
`m9x611` at relocation `0x233d0`, then stores a selected address function at
calculator `+0x50`.

The copy loop at `0x25300..0x25348` calls that function indirectly with this
exact contract [BIN][DER]:

```text
offset = calculate(calculator, base, x, y, z)
calculator+0x24 = offset
```

The row-major `m9x252` implementation proves the coordinate order:

```text
base + (((z * height) + y) * width + x) * bits_per_element / 8
```

For a tiled surface, the dispatcher selects dedicated functions for 1, 8, 16,
32, 64, and 128 bits per element, plus DXT1 and DXT2-5. These routines contain
the expected shifts, masks, coordinate XORs, tile strides, and memory-bank bit
permutations. They return a physical byte address, rather than a logical
descriptor or pitch.

The two configuration values are also statically joined to hardware. The
unnamed service initializer at `0x121d0`, immediately after `m9x1187`, reads
memory-controller register `0x0f04`. It stores `0x0f04[1:0]` at
address-config `+0x44` (`device_mode`) and `0x0f04[20:19]` at `+0x48`
(`channel_mode`). Thus a replacement driver must read these fields after
memory-controller initialization and select the matching formula; PCI device
ID alone does not choose one M9148 board layout.

## Exact 32-bit dispatch

For ordinary 32-bit texels, nonzero tiled mode selects one of four functions.
Surface flag byte 0 bit 3 distinguishes local memory from the aperture path:

| `device_mode == 2` | surface flag bit 3 | function | text address |
|---:|---:|---|---:|
| yes | 1 | `m9x474` | `0x24230` |
| yes | 0 | `m9x659` | `0x244b0` |
| no | 1 | `m9x587` | `0x24350` |
| no | 0 | `m9x602` | `0x24580` |

This selection is proven by the four `R_X86_64_32S` relocations at
`0x23769`, `0x23785`, `0x237a5`, and `0x237c1`. Class 3 does not have a
separate hidden permutation: it reaches this general tiled dispatcher through
its nonzero surface mode and uses the function for its element size and memory
location.

The same dispatcher completely identifies the remaining format families. Each
row below is ordered as mode-2 local, mode-2 aperture, other-mode local, and
other-mode aperture:

| element/format | selected functions |
|---|---|
| 1 bit | `m9x439`, `m9x531`, `m9x163`, `m9x323` |
| 8 bits | `m9x297`, `m9x638`, `m9x417`, `m9x1115` |
| 16 bits | `m9x977`, `m9x259`, `m9x1199`, `m9x460` |
| 32 bits | `m9x474`, `m9x659`, `m9x587`, `m9x602` |
| 64 bits | `m9x1029`, `m9x256`, `m9x885`, `m9x934` |
| 128 bits | `m9x1010`, `m9x898`, `m9x177`, `m9x307` |
| DXT1 (`format 0x10`) | `m9x710`, `m9x331`, `m9x341`, `m9x273` |
| DXT2-5 (`format 0x15/0x17`) | `m9x521`, `m9x8`, `m9x911`, `m9x118` |

Surface flag `+0x154 bit 6` overrides the ordinary uncompressed table with
`m9x382`. Zero tiled mode selects row-major `m9x252`, or `m9x990` for DXT.
The evidence checker verifies every one of these relocation targets.

## Recovered local 32-bit layout

Let `xn`, `yn`, and `zn` denote bit `n` of the logical coordinates. Both local
32-bit variants place the low byte-address bits as follows:

```text
A[1:0] = 0                    (32-bit texel alignment)
A[4:2] = x[2:0]
A[6:5] = y[1:0]
A[7]   = x3
```

This proves the visible eight-texel class-3 block width directly. Higher bits
fold in coordinates from neighboring blocks and layers, so the layout is not
a fixed Morton permutation repeated at every block.

For `device_mode == 2`, `m9x474` adds these bits before the final channel
transform:

```text
A8  = x4 ^ y2
A9  = z0 ^ y3 ^ x5 ^ y2
A10 = y4 ^ x7 ^ x5 ^ x6 ^ z1
A11 = x7 ^ x5 ^ y5 ^ y3

tile = (z * (height >> 5) + (y >> 5)) * (width >> 5) + (x >> 5)
address = (base & ~0x3ff) + (tile << 12) + A10*0x400 + A11*0x800
address |= A9*0x200 + A8*0x100 + (x3*0x80)
         + ((y & 3) << 5) + ((x & 7) << 2)
```

For the other device modes, local routine `m9x587` uses:

```text
A8  = z0 ^ y2 ^ x4
A9  = x6 ^ x4 ^ y4 ^ x5 ^ ((y2 ^ x4 ^ y3) & x5)
A10 = y3 ^ x5

tile = (z * (height >> 4) + (y >> 4)) * (width >> 5) + (x >> 5)
base_mask = ~0x3ff when device_mode == 1, otherwise ~0x7ff
address = (base & base_mask) + (tile << 11) + A10*0x400
address |= A9*0x200 + A8*0x100 + (x3*0x80)
         + ((y & 3) << 5) + ((x & 7) << 2)
```

All operations above follow the vendor's 32-bit arithmetic, including the
addition of the masked base and bank-select bits. Finally, when
`device_mode` is neither 1 nor 2 and `channel_mode != 1`, every calculator
rotates address bits 8-10:

```text
new A10 = old A8
new A9  = old A10
new A8  = old A9
```

The executable reference implementation in
`tools/m9-blocked-layout-evidence.py` includes these two local formulas and
the `m9x659`/`m9x602` aperture formulas. Its frozen vectors cover block edges,
bank XORs, and coarse tile transitions. The checker also hashes the complete
vendor function bodies, so a different saved binary cannot silently validate
against offsets from this build.

## Remaining validation boundary

No capture is needed to implement CPU detiling for the formats covered by this
calculator family. A short coordinate-coded class-3 upload and VRAM dump is
still useful as hardware validation of the selected `0x0f04` mode and base
address convention. It is now an implementation check with an exact expected
answer, rather than an information-recovery requirement.

The checked Python implementation currently exposes all four 32-bit variants.
The vendor object also contains exact routines for 1/8/16/64/128-bit and DXT
surfaces; those bodies and their dispatch are identified, but they have not yet
been transcribed into maintainable source formulas.
