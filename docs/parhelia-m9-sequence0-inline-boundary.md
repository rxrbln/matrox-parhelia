# M9148 sequence-0 setup envelope and inline-program boundary

This note turns the first saved vendor submission into an exact,
implementation-facing setup recipe and tests whether the selector-`68`
inline path can remove the remaining selector-`6c` dependency for the first
flat or smooth triangle.  It uses saved captures and vendor binaries only.
No command was sent to hardware.

Reproduce every numeric result with:

```sh
python3 tools/m9-sequence0-inline-boundary-evidence.py --self-test
```

The checker pins both sequence-0 files, the controlled smooth/flat A/B files,
their selected system-resource images, the Windows `MxMD.dll`, the Linux
`m9x_drv.so`, and the fixed-function raw archive.  To obtain literal C arrays
and the original outer-record ends, run:

```sh
python3 tools/m9-sequence0-inline-boundary-evidence.py --emit-c \
  > /tmp/m9-sequence0-packets.c
```

The output is generated from hash-pinned evidence.  It is not a userspace
submission interface.

## Exact three-range envelope

Vendor sequence 0 uses three end-exclusive ExecuteDma ranges.  Stripping only
the private outer count dwords produces the following raw indexed-packet
arrays:

| range | logical bytes | BM bytes | packets | private records | raw dwords | SHA-256 |
|---:|---:|---:|---:|---:|---:|---|
| 0 | `[000,538)` | `[008,540)` | 78 | 73 | 261 | `b528be30d4600b17af7682a0ff353c103806c631910a08e0af862b42d05c8832` |
| 1 | `[538,f6c)` | `[540,f74)` | 145 | 144 | 509 | `655752615cf4ae3d141eb09d2f941c9eab79760998794a8723c8e54162f13fce` |
| 2 | `[f6c,f98)` | `[f74,fa0)` | 3 | 3 | 8 | `4728cca10a124b89118067e04291c0da7b426029886b687e0cc9c2dc0ad6beb9` |

All descriptor flags are zero.  The `--emit-c` output also supplies a
cumulative dword-end array for the 73, 144, and 3 private records.  A guarded
first diagnostic should preserve those boundaries when placing raw packets in
primary intervals.  Concatenating all packets in a range is a later hardware
isolation step, not a property established by the private ExecuteDma format.

Range 0 installs disabled/default surface, clip, texture, common, and raster
state.  It also clears five fixed vertex-conversion auxiliary lines:

```text
68=80001300, ports 60..67 = 0
68=80001320, ports 60..67 = 0
68=80001340, ports 60..67 = 0
68=80001360, ports 60..67 = 0
68=80001500, ports 60..67 = 0
```

These writes do not activate an inline program.  Sequence 0 never writes
`69`, `6a`, or `6d`.

Range 1 first binds scratch allocation 26, then uploads the invariant 80-slot
image through `8f/90..9f`.  Its `0x280`-byte payload and equality with setup
sequences 8, 10, and 14 remain pinned by
`m9-first-triangle-resources-evidence.py`.  Range 1 then performs the saved
state transitions around scratch allocations 27 and 38.  Range 2 emits the
completion address, final `e6=3,e6=2,e3=target|1`, and matching `43=target`
footer.

## Typed patch map

Every count-free dword outside this table is literal for this exact envelope.
`T` is a newly reserved, 16-byte-aligned block of nine consecutive target
values corresponding to captured values `0x30..0xb0`.  Preserve each repeated
value and every low flag bit.

| range | dword offsets | index | replacement | mask / fixed bits |
|---:|---|---:|---|---|
| 0 | `2` | `e3` | `T+00` | `fffffff0 / 0` |
| 0 | `23,73` | `e3` | `T+10` | `fffffff0 / 0` |
| 1 | `2` | `e3` | `T+20` | `fffffff0 / 0` |
| 1 | `18,52,383` | `e3` | `T+30` | `fffffff0 / 0` |
| 1 | `415` | `e3` | `T+40` | `fffffff0 / 0` |
| 1 | `429` | `e3` | `T+50` | `fffffff0 / 0` |
| 1 | `466` | `e3` | `T+60` | `fffffff0 / 0` |
| 1 | `480` | `e3` | `T+70` | `fffffff0 / 0` |
| 2 | `5` | `e3` | `T+80` | `fffffff0 / 1` |
| 2 | `7` | `43` | `T+80` | `fffffff0 / 0` |
| 1 | `20` | `b0` | scratch 26 GPU address | `ffffffff / 0` |
| 1 | `431` | `b0` | scratch 27 GPU address | `ffffffff / 0` |
| 1 | `482` | `b0` | scratch 38 GPU address | `ffffffff / 0` |
| 2 | `1` | `1b` | coherent completion address | `fffffff0 / 2` |

The verifier rejects any nonzero `e3`, `43`, `1b`, or one of the three known
scratch `b0` operands that is not present in this table.  The three scratch
references are untagged full-dword addresses and are write destinations in
this sequence.  The allocation results preserved independently in the raw
archive give their exact bounds:

| vendor allocation | captured base | byte extent | dimensions |
|---:|---:|---:|---:|
| 26 | `11e19000` | `0800` | `1 x 1` |
| 27 | `11e21000` | `20080` | `2000 x 1` |
| 38 | `11ed9000` | `0400` | `4 x 8` |

Allocate distinct placement-4 local-VRAM BOs of at least those extents and
clear them before the first use.  Patch their untagged
`0x10000000 + BAR0 offset` engine addresses.  Keep them resident until `T+80`
retires.  The unusual logical dimensions are vendor allocation metadata and
do not replace the byte bounds.  Exact result fields and the kernel allocation
policy are audited in `parhelia-m9-sequence0-kernel-integration-audit.md`.

The safe replay rule is consequently exact: copy the three hash-pinned arrays,
patch only the 17 table entries, preserve the 220 private-record boundaries,
publish all CPU writes, post the ranges in order, and accept success only when
the coherent completion word matches `T+80` after masking its low nibble.

This is a post-engine-init setup envelope.  The capture began after the vendor
had initialized clocks, reset and the primary requester.  Sequence 0 contains
no MMIO reset, clock, requester-base, or bus-master enable operation.  It is
therefore warm-safe evidence and becomes a cold-context recipe only after the
clean-room driver's already guarded cold engine init and no-op completion have
passed.  Raw sequence-0 replay against a reset GPU is not supported by this
evidence.

## Selector 68 cannot yet replace selector 6c

The controlled case-37 A/B fixture settles one direct context, draws the
default smooth/Gouraud state, changes only `glShadeModel(GL_FLAT)`, and draws
again.  Each state is bound four times in its setup submission:

```text
smooth/Gouraud: 68=0 69=11e42000 6a=25010000 6b=00001030 6c=40c3a401
flat:           68=0 69=11e42000 6a=25010000 6b=00001030 6c=40c3aa01
```

Neither submission writes `6d` or uploads a selector-`68` image.  Both draws
complete with different verified framebuffer hashes.  Removing the low tag
from `6c` selects two different `0x80`-aligned records, at `+0x400` and
`+0xa00`, in the same captured `0x21000`-byte system BO.  Both pre-submit
resource images contain the expected seven-float geometry prefix at the
selected record.

This proves three useful negatives for the first clean-room triangle:

1. selector `68` is inactive in both successful smooth and flat vendor paths;
2. shade-model selection is represented inside the atomically selected `6c`
   record, while `69/6a/6b` stay identical; and
3. the five sequence-0 `68` writes are auxiliary-state zeroing, not the
   program used by either triangle.

The saved binaries agree.  Windows calls main selector `0x13b260`, companion
selector `0x13b570`, and linker `0x13bf80` from fixed
vertex/declaration-conversion wrapper `0x13c2c0`.  Linux has one direct call
to inline-image builder `0x44ba40`, from the corresponding constructor at
file offset `0x44c803`.  The inline emitter later uploads `68/60..67`, writes
`69=4`, and emits starts through `6a` and optional `6d`.  Its owner is distinct
from the external linked-record packet at Linux file offset `0x4b9a73`.

The selector-`68` transport is implementable, but the saved evidence does not
show it producing the complete GL two-pass program selected by `6c`.  It is a
fixed vertex/declaration-conversion chain with two internal starts; its
selector inputs, companion choice, and resulting native image for this GL
flat/Gouraud pair were never captured.  Substituting it would discard the only
observed shade-model selection and would invent a relation between two
separate state representations.

The precise remaining gate is one current-context `6c` serializer for the
known fixed pair.  If that cannot be recovered statically, the alternative is
one saved/runtime differential that captures the complete inline image and
both start controls for the same simple smooth/flat triangle and verifies the
two-pass target.  No additional transport grammar is missing: line upload,
padding, 256-slot bound, and `69/6a/6d` emission are already exact.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
