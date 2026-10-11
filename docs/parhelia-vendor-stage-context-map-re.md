# Vendor texture-stage context-map boundary

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This audit follows the saved vendor Linux object through the texture-stage
state compiler. It records byte-level facts useful for sampled-alpha work;
it does not infer a register value that the object does not expose.

## Result

The Linux driver has a concrete, dynamic path from three texture-stage
properties to sparse context maps:

```
property ID 4/5/6
  -> raw stage descriptor +0x3c/+0x44/+0x48
  -> compiled stage record +0x10/+0x14/+0x18
  -> eight sparse-map copies selected by map nodes
```

The final map-node identifiers and the later map-to-command-buffer encoding
are not recoverable from this static object alone. In particular, this path
does **not** prove that any one of the three fields writes a sampler,
combiner, or alpha-test MMIO register. A matched vendor run must capture the
map-node contents and final command section before Mesa can reproduce this
part of the compiler.

## Reproducible object locations

All virtual addresses below are .text addresses in
matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o; its .text section
starts at file offset 0x40, so add 0x40 for the file offset.

| routine | VMA / file offset | demonstrated operation |
|---|---:|---|
| mtx673 | 0x17260 / 0x172a0 | dispatches a property ID and updates a raw descriptor only when the value changes |
| mtx2223 | 0x17e70 / 0x17eb0 | compiles ten raw stage descriptors into ten 0xa0-byte records |
| mtx580 | 0x1b640 / 0x1b680 | invokes mtx2223 and returns compiled record 0x434 + unit * 0xa0 |
| mtx950 | 0x14be0 / 0x14c20 | performs eight mtx713 sparse-map propagations |
| mtx713 | 0xaaaf0 / 0xaab30 | copies a 32-bit word selected by every attached map node |
| mtx3136 | 0x1b680 / 0x1b6c0 | invokes mtx580(ctx, 0) and immediately passes the result to mtx950 |

Commands used to check the offsets and relocations:

```
objdump -t kernel/mtx_parhelia.o | rg 'mtx(673|2223|580|950|713|3136)'
objdump -drwC -Mintel --start-address=0x17260 --stop-address=0x17740 \
  kernel/mtx_parhelia.o
objdump -drwC -Mintel --start-address=0x17e70 --stop-address=0x17fe1 \
  kernel/mtx_parhelia.o
objdump -drwC -Mintel --start-address=0x14be0 --stop-address=0x14c10 \
  kernel/mtx_parhelia.o
```

## Proven property and record layout

mtx673 uses edx as a property selector. Its relocated dispatch table is
.rodata+0xa38; the relevant entries are exact and independent of symbol
names:

| property ID | dispatch target | raw descriptor write |
|---:|---:|---:|
| 4 | 0x176b7 | dword [state + 0x3c] = ecx |
| 5 | 0x176d2 | dword [state + 0x44] = ecx |
| 6 | 0x17643 | dword [state + 0x48] = ecx |

Each handler sets dirty bit 0x10 at state+1 before returning. This is the
source-level evidence behind the conventional names ALPHAOP, ALPHAARG1,
and ALPHAARG2; the numeric property-to-offset correspondence itself is
directly disassembled.

mtx2223 reads raw descriptor n from
ctx + 0xdd7 + n * 0x82, and writes compiled record n at
ctx + 0x434 + n * 0xa0. Its instructions at 0x17ea2, 0x17eae, and
0x17eb4 make this exact transfer:

| raw descriptor | compiled record |
|---:|---:|
| +0x3c | +0x10 |
| +0x44 | +0x14 |
| +0x48 | +0x18 |

The same routine also transfers +0x40 -> +0x6c and +0x4c -> +0x70.
Their public state names are not needed to establish the alpha triplet, so
this audit leaves them unnamed.

## Sparse-map propagation, precisely bounded

mtx3136 contains the only direct call chain of interest:

```
0x1b6a9: call mtx580       /* arguments: main context, unit 0 */
0x1b6b4: call mtx950       /* arguments: output object, returned record */
```

mtx950 begins with destination output + 0x78 and source equal to that
compiled record. It executes exactly eight iterations:

```
mtx713(output + 0x78 + i * 0x60, compiled + i * 0x74), i = 0..7
```

That follows from 0x14be9 (+0x78), 0x14bfb (source += 0x74),
0x14c00 (destination += 0x60), and the initial counter seven at 0x14be2.

mtx713 traverses the list rooted at destination+0x28. For every node it
loads a 32-bit index from node+0x20, then calls LocMemCpy with four bytes
from source + index * 4 to the payload pointer at node+0x28
(0xaab20..0xaab35). Thus the opaque map nodes, rather than a fixed offset
in a stage record, select which compiler words are forwarded.

This explains why an inspection limited to the final static sampler window
and 0x1260..0x1284 combiner packet can miss the sampled-alpha distinction:
the object preserves the three alpha properties in state and publishes them
through a keyed dynamic map, but the map consumer is a separate boundary.

## Windows cross-check and limit

The saved Windows OpenGL ICD is
xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll (PE32, 5,410,816 bytes).
It is stripped and does not contain a symbol or relocation trail equivalent
to Linux mtx673/mtx2223; no trustworthy byte-offset mapping from a Windows
texture-stage API field to a hardware packet was extracted from it. The saved
vendor command captures do show co-issued RGB and alpha combiner words for
textured GL_MODULATE, as recorded in
parhelia-p690-sampler-alpha-emission-re.md, but that is downstream output,
not evidence for a dynamic compiler-field mapping.

Do not turn the Windows absence of a static mapping into a guessed packet.
The useful next capture is one vendor draw per alpha case, retaining: the raw
stage descriptor, its 0xa0 compiled record, all eight 0x60 map containers
and their node payloads, plus the final submitted command section. Diffing
opaque and zero-alpha textures will then identify either an actual map-node
delta or demonstrate that the failing P690 route lies elsewhere.
