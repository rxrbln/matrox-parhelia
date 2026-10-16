# M9148 GL logic ops: the captured fragment tail

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

Status 2026-10-02: **implemented** in m9x 6780573; all 15 non-COPY functions
are exact on hardware (MEASURED: 16384 channel checks per function, texture and
vertex-colour sources, GL_DITHER on and off).

## No state word

The M9 has no logic-op state.  The vendor GL appends a fragment-program tail
that reads the destination colour, exactly like its blend lowering.  Compared
with a COPY draw, a logic-op draw differs only in that tail and in the
destination-read raster words (CAPTURED, pix and misc agents, 2026-10-02):

| word | value | note |
|---|---|---|
| `88` | `01080000` | bit 19 set, no-texture bit 20 clear, all register pairs used |
| `8c` | `00500b00` | |
| `ef` | `000000aa` | same as blending |
| `f0` | `c5008200` | destination read, same as blending |

## The tail

All 15 non-COPY functions use the same 39-slot program; COPY has no tail.
Sources: `captures/m9x-linux-gl15-ff-20260915/fragops/part-2.tgz`, cases
`logic_*` (each verified by the vendor stack at 144 points with 0
mismatches), cross-checked against the gl2ext captures
(`captures/m9x-vendor-glsl-programs-20260915`, logic-xor/and/or/invert).
The slot words and a register liveness listing are in
`tools/m9-logic-op-regs.py`: the tail reads the destination colour from
`18`/`1a` before writing them, writes its result to `2d`, and uses temporaries
in `00`..`1d`; the misc agent's decode also names special registers `7d`/`7e`.

Slots 35 and 36 select the function; they differ only in W0's class (0 or 6)
and W1[15:4] (the op2 = 0x16 bitwise form):

| function | cls / W1[15:4] | function | cls / W1[15:4] |
|---|---|---|---|
| clear | c0 / c140 | nor | c6 / c140 |
| and_inverted | c0 / c170 | copy_inverted | c6 / c170 |
| and_reverse | c0 / cd40 | invert | c6 / cd40 |
| xor | c0 / cd70 | nand | c6 / cd70 |
| and | c0 / f140 | equiv | c6 / f140 |
| noop | c0 / f170 | or_inverted | c6 / f170 |
| or_reverse | c6 / fd40 | or | c0 / fd70 |
| set | c6 / fd70 | copy | no tail |

Constants, in order: `38800006 3ffff000 47800000 477fff00 3b800000 40800000
37800006 3ffff088`, then `(44800000, 0)`, then the standard `(0,1)`,
`(1,0.5)` tail pairs.  The conversion ops `0x18`, `0x16`, `0x01`, `0x02` in
the tail (unpacking the colour to integer bytes and back) are replayed
verbatim, not decoded.

## Implementation plan

Replay the tail after the program's colour output: map the colour into the
tail's input registers, rename the tail's temporaries by liveness so they
avoid live texel/varying pairs, relocate the constants into the program's
constant layout, and set the four raster words.  Expose logic op only with
all 16 functions.  Width-1 lines need a line raster class with a destination
read, which is **not captured**; drawing them as quads avoids it.

Piglit: gl-1.1-xor, gl-1.1-xor-copypixels and the LogicOp part of
gl-1.0-no-op-paths.

## Dither in the captured tail (MEASURED)

Replayed verbatim the tail is *not* exact: `7d`/`7e` hold 1024 plus an 8x8
ordered dither value per channel, which the tail adds to the source colour,
moving exact bytes by one in 2-4% of channels (the vendor's own capture shows
the same "one lower" deviation).  The driver loads the constant 1056 instead;
every value 1030..1072 is exact, 1024 and 1087 are not.  The tail is never
dithered, which GL permits.

Refused: 10-bit targets (only the 8-bit tail is captured), several colour
targets, constant templates, and programs out of constants or temporaries.
Width-1 lines are drawn as quads.  gl-1.1-xor passes; gl-1.0-logicop passes all
16 subtests (its multisample subtests skip).

Follow-up: `m9_dither_ef` always returns dither-on for 8-bit targets, so the
plain COPY path from vertex colours is one low in places and GL_DITHER is
ignored; gl-1.1-xor-copypixels fails its first check (before any logic op)
probably for that reason.
