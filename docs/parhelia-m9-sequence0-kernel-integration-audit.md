# M9148 sequence-0 kernel integration audit

This note is the implementation contract for adding the captured first setup
submission to the guarded clean-room triangle diagnostic.  It audits saved
files and the current kernel source only; no command was sent to hardware.

Recheck the inputs, private framing, all relocation sites, scratch allocation
results, generated image, and current kernel capacity with:

```sh
python3 tools/m9-sequence0-kernel-audit.py
python3 tools/m9-sequence0-kernel-audit.py --generate /tmp/m9-seq0
python3 tools/m9-sequence0-kernel-audit.py --emit-c \
  > /tmp/m9-sequence0-framed.c
```

The generated directory contains an exact 32-KiB `secondary.bin`, its 36-byte
`primary.bin`, and a JSON manifest of every patched value.  The default
addresses are synthetic, valid addresses for offline checking only.

## Exact BmBuffer image and primary records

The captured BmBuffer begins with eight bytes of private host bookkeeping.
The first dword is the stale process value `0x006c57d8`; it is not submitted.
A clean-room image must zero bytes `[0,8)` and retain the captured bytes
`[8,fa0)` byte for byte before applying the typed patches below.  The vendor
BmBuffer has an exact `0x8000`-byte extent.  Bytes `[fa0,8000)` are zero, and
all eight pages are present in the compact translation table.

The three ExecuteDma ranges are end-exclusive:

| range | logical bytes | secondary bytes | packets | outer records | framed SHA-256 |
|---:|---:|---:|---:|---:|---|
| 0 | `[000,538)` | `[008,540)` | 78 | 73 | `2eb4abba0035e593ba1d1f5b99e7df59199e886c4055ab0e51e603c6099d0779` |
| 1 | `[538,f6c)` | `[540,f74)` | 145 | 144 | `e082d94a4102b92ac743a18dc5e191be347a3eee1f5800091f6292cba1eb4dd7` |
| 2 | `[f6c,f98)` | `[f74,fa0)` | 3 | 3 | `a79c187e4e4e90438c9b80046b683d7df28e50d5f5e8119bcbf275069ba599ae` |

All 220 outer records are counted records.  Their count dwords remain in the
secondary image.  The verifier walks each count to the next exact record
boundary and pins a boundary-list hash for each range.  It also pins the
complete source BmBuffer as
`91854895140729c8dcb6ec55c511505241f3441be4bf5eea040fbea1ddfeeebd`.

For compact class-1 GPU virtual address `S` of the coherent secondary BO,
build exactly nine primary dwords:

```text
45452d2c  (S+008)|1  (S+540)|2
45452d2c  (S+540)|1  (S+f74)|2
45452d2c  (S+f74)|1  (S+fa0)|2
```

Index `2c` receives the inclusive byte start with tag 1.  Index `2d` receives
the end-exclusive byte address with tag 2.  `S` is the BO's
`0x40000000..0x4fffffff` dispatcher address.  Its raw DMA32 pages appear only
in the compact PTE values.  Build all 36 primary bytes, execute `dma_wmb()`,
and publish one final endpoint.  Do not expose intermediate 12-byte or 24-byte
endpoints.

## All 17 patches

Offsets below are absolute byte offsets in the framed `0xfa0`-byte secondary
image.  `T` is the first of nine consecutive 16-byte target values.

| range/dword | framed offset | index | replacement |
|---|---:|---:|---|
| 0/2 | `014` | `e3` | `T+00` |
| 0/23 | `084` | `e3` | `T+10` |
| 0/73 | `184` | `e3` | `T+10` |
| 1/2 | `54c` | `e3` | `T+20` |
| 1/18 | `5a0` | `e3` | `T+30` |
| 1/20 | `5ac` | `b0` | scratch 26 local address |
| 1/52 | `648` | `e3` | `T+30` |
| 1/383 | `cc0` | `e3` | `T+30` |
| 1/415 | `d74` | `e3` | `T+40` |
| 1/429 | `dbc` | `e3` | `T+50` |
| 1/431 | `dc8` | `b0` | scratch 27 local address |
| 1/466 | `e88` | `e3` | `T+60` |
| 1/480 | `ed0` | `e3` | `T+70` |
| 1/482 | `edc` | `b0` | scratch 38 local address |
| 2/1 | `f7c` | `1b` | coherent completion DMA address `|2` |
| 2/5 | `f90` | `e3` | `(T+80)|1` |
| 2/7 | `f9c` | `43` | `T+80` |

Every target and completion address has its low nibble cleared before the
listed tag is added.  The three local `b0` operands are untagged.  The
verifier rejects any other nonzero `e3`, `43`, `1b`, or matching scratch
`b0` value as an unclassified dynamic operand.

Reserve the target block atomically:

```text
used = T + {00,10,20,30,40,50,60,70,80} modulo 2^32
next = T + 90 modulo 2^32
```

Seed the completion dword so it cannot equal `T+80`, publish the secondary
and primary writes, then wait until `dma_rmb()` observes
`completion & ~0xf == T+80`.  Sequence 0 therefore consumes nine timeline
values, not one.  Keep the primary, secondary, completion page, and all three
scratch BOs pinned until that final value retires.

## Three local scratch objects

The allocation responses prove that all three operands are placement 4 local
VRAM objects in the M9148 `0x10000000..0x1fffffff` engine domain.  They are
not DMA32 coherent allocations.  For a pinned GEM VRAM object, patch
`0x10000000 + drm_gem_vram_offset()` into `b0`.

| vendor allocation | kind | captured base | usable bytes | dimensions | result `+21c/+220/+224` |
|---:|---:|---:|---:|---:|---:|
| 26 | `10` | `11e19000` | `0800` | `1 x 1` | `80/20/20` |
| 27 | `01` | `11e21000` | `20080` | `2000 x 1` | `20000/80/2000` |
| 38 | `0e` | `11ed9000` | `0400` | `4 x 8` | `20/20/08` |

All three captured bases are 4-KiB aligned.  Preserve at least that allocator
granularity, the exact usable extents, and a guard page.  Keep the three BOs
distinct from one another and from the first-draw transients.

The result word at `+0x21c` has been called an allocation alignment in older
notes.  Allocation 27 rules out that interpretation for this record: its
returned base `0x11e21000` is not `0x20000` aligned.  Treat `+21c/+220/+224`
as pinned layout-result metadata until their private meanings are recovered;
do not request `0x20000` base alignment from `+21c`.

The capture contains allocation records but no pre-submit VRAM snapshots for
these objects.  It therefore cannot prove the vendor allocator's initial
bytes.  Sequence 0 only binds these three addresses as `b0` destinations and
does not bind them as source or texture operands.  Zero every usable byte in
the clean-room objects before first submission.  This gives deterministic
content and avoids making unknown allocator residue an input.  Retain the
objects for the diagnostic lifetime; freeing immediately at `T+80` would be
safe for command reachability, but later hidden context use has not been
excluded by a content capture.

This missing historical pre-content is not a sequence-0 publication blocker:
there is no captured command read from it, and deterministic zeroing supplies
the only unwritten bytes.  It remains a limitation on claiming byte-identical
vendor scratch output after completion.

## Current kernel integration

`m9_triangle.c` now allocates the secondary as the exact `0x8000`-byte vendor
BmBuffer rather than placing a sentinel after the submitted `0xfa0` bytes.
It maps all eight pages, which raises the complete source/system/secondary
compact table from 38 to 44 PTEs, and clears the full BmBuffer before building
either sequence zero or the draw image.  Its `u32 primary[9]` holds the exact
36-byte publication.

The three local scratch identities use the captured usable sizes `0x800`,
`0x20080`, and `0x400`; the four `0x16800` first-draw transients remain
distinct.  Sequence zero runs after guarded cold engine/requester/window
setup.  After its ninth value retires, the driver constructs the draw image in
the shared BmBuffer and publishes the draw's own six values.

The exact setup publication has no remaining offline transport blocker.
Hardware execution still depends on the parent work that publishes the local
and system address windows/page table and passes the existing cold-init and
no-op diagnostics.  A hang after those prerequisites would be a hardware
isolation result rather than missing BmBuffer, relocation, scratch, or primary
framing information.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
