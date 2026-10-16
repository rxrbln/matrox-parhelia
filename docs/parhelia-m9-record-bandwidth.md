<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# The inter-pass record's traffic, and why it does **not** bound a scene

**The reading this note was written to support is REFUTED.**  Slots are now
allocated by liveness, which halves the stride for every shader of three
passes or more, and MEASURED 2026-09-19 on the M9128 at 1920x1200, one
binary with `M9X_RECORD_LIVENESS` the only variable:

| scene | stride | record | fps |
|---|---:|---:|---:|
| `desktop` blur | 12 -> 8 dwords | 8 -> 5 MiB | 38 -> 38 |
| `effect2d` 15-tap | 16 -> 8 dwords | 141 -> 70 MiB | 33 -> 33 |

Halving the traffic moved neither scene by a frame.  So neither is bound by
the record, and `parhelia-m9-fill-rate.md` had it right: they are bound by
fragment execution.  What the halving buys is **allocation** -- a 141 MiB
record has been refused on this card before now and a 70 MiB one is not --
and that is the reason to keep it.

The arithmetic below is retained because it is correct as arithmetic; what
was wrong was reading a frame rate off it.  The original text follows.

## The original reading (DERIVED 2026-09-18, refuted above)

## The arithmetic

The record carries **four 32-bit floats a fragment per hand-over**, and the
driver allocates one slot per hand-over: `stride = 4 * (passes - 1)` dwords.
Every hand-over is written once and read once, so at 1920x1200:

| passes | stride | traffic a frame | at 60 fps | record |
|---:|---:|---:|---:|---:|
| 2 | 4 dw | 74 MB | 4.4 GB/s | 35 MiB |
| 4 | 12 dw | 221 MB | 13.3 GB/s | 105 MiB |
| 5 | 16 dw | 295 MB | 17.7 GB/s | 141 MiB |

`effect2d`'s 15-tap kernel is the five-pass row and MEASURED **31 fps**,
which is 9.1 GB/s of record traffic alone - before a texture read or a
colour write.  Reaching 60 fps would need 17.7 GB/s from the record alone.

That was read as retiring the fill-rate explanation for this scene.  It does
not: the scene runs at the same 33 fps with the stride at 8 dwords and at 16
(MEASURED, above), so the record was never the constraint.  The fill-rate
model's 19 full-screen operations at 1.157 ms give 45 fps against a measured
33, which is the right order; the arithmetic here happened to land near the
same number for a different reason.

## Why the stride is smaller (implemented)

A slot per hand-over is only necessary if a pass can read an output older
than its immediate predecessor.  When the pass graph is a **chain** - pass
N writes, pass N+1 reads, and nothing reads further back - two slots
suffice, used alternately, and the stride is 8 dwords whatever the pass
count:

| passes | by liveness | at 60 fps | record |
|---:|---:|---:|---:|
| 4 | 8 dw, 147 MB | 8.8 GB/s | 70 MiB |
| 5 | 8 dw, 147 MB | 8.8 GB/s | 70 MiB |

For two passes the present rule is already minimal and must be kept: a
ping-pong would double it.  So the rule is **allocate slots by liveness**,
which degenerates to `min(4 * (passes - 1), 8)` for a chain and gives each
value its own slot only where one is genuinely live across another.  That is
what `m9_record_layout()` does, and the measured strides above are its
output.

This halves the traffic for the cases that need it and halves the record
with it - 141 MiB to 70 MiB for the five-pass case, 105 to 70 for the
four-pass one.  Both of those sizes have been refused on this card before
now, so the saving is not only bandwidth.

**The correctness condition is the part to check first.** The splitter is
known to produce chains where every middle pass consumes and produces a
routed coordinate, which is the good case; a schedule where a later pass
reads an earlier pass's output directly is not, and would need its own
slot.  Liveness answers that per shader rather than by assumption.

## The stride is a descriptor field, not a consequence of the pass count

The record descriptor carries an independent dword stride in bits 7:0 and an
independent dword offset in bits 15:8, and vendor preprocessing records use
strides of 4, 10, 12, 16, 20 and 60 dwords with fields packed at arbitrary
dword offsets and in two widths (CAPTURED,
`docs/parhelia-m9-varying-transport-re.md` section 3).  So the liveness
allocation above needs no new hardware capability at all: it is a different
pair of numbers in the same two fields the driver already emits through
`m9_record_desc()`.

## Open

This board's memory bandwidth has still not been measured.  It no longer
matters for these two scenes: halving their record traffic changed neither
frame rate, so whatever the bandwidth is, it is not what they are waiting
for.  It would still be worth knowing before any future reading attributes a
cost to memory traffic on arithmetic alone - which is the mistake this note
records.
