# M9 auxiliary-window inputs and initial E6 packet

This closes the Linux M9148 `m9x421` values for MMIO `0x360/0x364` and
`0xf64/0xf68`. It also documents why the analogous Windows construction
produces different values. The two platform builders share descriptor field
roles, but their constants, flags, and input sources are not interchangeable.

Evidence tags mean binary disassembly (`BIN`), open wrapper source (`SRC`),
saved capture (`CAP`), and algebraic substitution (`DER`).

## Linux result for the test target

The exact ordered direct-MMIO writes are:

```
0x360 = 0x00861000
0x364 = 0x008e0000
0xf64 = 0x008e1000
0xf68 = 0x00940000
```

These values follow from the canonical shipped Linux core, the default Linux
`MmFlags=0x000100e8`, and the test target's 256 MiB actual VRAM and 256 MiB
BAR0 `[BIN][SRC][CAP][DER]`.

## Linux descriptor construction

Linux `m9x858` starts with two different byte counts:

* `V` is actual VRAM bytes decoded from the video BIOS.
* `B` is PCI BAR0 aperture bytes supplied by the wrapper as `uiFbLen`.

The source join is exact. `m9x798` initially copies `uiFbLen` into internal
fields `+0x38` and `+0x3c`, then replaces `+0x3c` with the VRAM total decoded
from the BIOS memory description when valid. The `m9x748+0x10` vtable has
these relevant ELF relocations `[BIN]`:

```
relative method +0x20 -> m9x1167 -> return object+0x38  (BAR0 bytes)
relative method +0xb0 -> m9x986  -> return object+0x3c  (actual VRAM bytes)
```

The open wrapper sets `uiFbLen = pci_resource_len(dev->pci_dev, 0)` `[SRC]`.
This corrects the earlier cross-platform interpretation which treated the
Linux `+0xb0` result as BAR0 length.

For the Linux default flags, bits 3, 5, and 6 are set while bit 4 is clear.
Consequently `m9x858` constructs `[BIN]`:

```
P   = (V >> 10) + 0x20000       # bit 3 supplies V; bit 6 adds 0x20000
A   = B >> 10                    # bit 5; this is BAR0, not an allocation
C18 = 0x801000
C1c = P
C24 = 0                         # bit 4 is clear
C28 = C18 + C1c + C24
C2c = A
C30 = C28 + C2c
C34 = 0x40000
C38 = C30 + C34
C3c = P
```

For `V=B=0x10000000`, this is:

```
P=0x60000 A=0x40000
C28=0x861000 C2c=0x40000 C30=0x8a1000
C34=0x40000 C38=0x8e1000 C3c=0x60000
```

Linux `m9x53` transfers `C28/C2c` into the selected `0x360` manager range,
`C30/C34` into its added-offset range, and `C38/C3c` into the selected
`0xf64` range. Linux `m9x421` at `0x1680e..0x168a9` writes `[BIN]`:

```
if C2c != 0:
    MMIO[0x360] = C28 & 0xfffff00c
    MMIO[0x364] = C28 + C2c + C34 - 0x1000
MMIO[0xf64] = C38
MMIO[0xf68] = C38 + C3c - 0x1000
```

Write order is conditional `0x360`, `0x364`, then unconditional `0xf64`,
`0xf68`, followed by `0xf74/0xf78/0xf7c`. The target has nonzero `C2c`, so
all four writes occur. `0x364 = 0xf64 - 0x1000` is an implementation
assertion.

## Why the Windows values differ

The Windows descriptor builder at `MxMD.dll` RVA `0xcfb40` uses `[BIN]`:

```
C18 = 0x401000                  # Linux uses 0x801000
P   = V >> 10                  # no Linux-style +0x20000 here
C24 = 4*P                      # Windows MmFlags 0x101f8 has bit 4 set
C28 = 0x401000 + 5*P
```

Its `C2c` input is also different. `MxMD.dll` RVA `0x85d50` stores output
dword 3 of miniport IOCTL `0x230458` into `hw+0xa0`. The `MxMM.sys` handler
at RVA `0x702cf` selects a candidate from system and device policy, floors it
to 4 MiB, attempts to allocate/map it, and retries smaller candidates until
successful. Output dword 3 is that accepted allocation byte count `[BIN]`.
It is not the Linux BAR0 input and cannot be inferred from PCI BARs.

For the Windows path only, with `P=V>>10` and
`A=accepted_allocation_bytes>>10`, the writes are:

```
if A != 0:
    MMIO[0x360] = (0x401000 + 5*P) & 0xfffff00c
    MMIO[0x364] = 0x440000 + 5*P + A
MMIO[0xf64] = 0x441000 + 5*P + A
MMIO[0xf68] = 0x440000 + 6*P + A
```

The previously reported `0x541000` target base was therefore a valid
Windows-path specialization for `V=256 MiB`; it was not a Linux value. No
Windows tuple should be used for the Linux first triangle.

## Initial E6 packet

The canonical Linux `m9x421` tests exactly
`(*(builder+0x48)+0x28) & 1` `[BIN]`:

```
bit clear:  454545e6 00000002
bit set:    4545e6e6 00000000 00000002
```

The saved register captures contain neither this transient packet nor the
object bit. The Linux choice remains a one-bit live boundary. The smallest
safe observation is the byte at manager build time, or the command packet
before publication.

Windows instead tests `MmFlags` bit 0. The default `0x101f8` clears it:

```
default:    454545e6 00000004
alternate:  4545e6e6 00000000 00000004
```

The Windows payload (`4`) must not be copied into Linux (`2`).

## Reproduction

Run:

```
python3 tools/m9-aux-window-evidence.py
```

The verifier pins the canonical Linux and Windows binaries, Linux descriptor
and manager slices, ELF vtable relocations, BAR0 wrapper plumbing, capture
inputs, platform-specific formula results, write order, and E6 variants. It
defaults to `/tmp/m9vendor-original/kernel/KernelLibraries.o_shipped`; the
working-tree copy may be modified during bring-up and is intentionally not
trusted.
