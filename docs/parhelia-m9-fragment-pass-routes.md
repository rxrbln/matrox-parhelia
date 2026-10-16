# M9 inter-pass record: descriptor and route word field layout

`docs/parhelia-m9-fragment-pass-splitting.md` section 4 establishes that a
fragment shader split across raster passes hands values over through a
per-fragment record in memory, and leaves the field boundaries inside the
route word **OPEN**, advising a scheduler to emit the captured triples
verbatim.  This note closes those boundaries.  The model below reproduces
**every** record descriptor and route word in both independent captures --
the seven two-pass GLSL cases and the five-sampler schedule, twenty values in
all -- and is checked by `test_record_routes` in
`src/gallium/drivers/m9x/m9_batch_test.c` (OFFLINE).

Labels: **CAPTURED** read from a saved stream, **DERIVED** a join, **OPEN**
not decoded.

## 1. The descriptor

A record descriptor appears as the producing pass's `b1` and as the consuming
route's `0x79`.  It is not an ordinary surface descriptor: its low sixteen
bits hold no row span (DERIVED).

```text
bits 31:25  0x77   record format class, every captured value (CAPTURED)
bits 24:21  0
bits 20:19  form, below
bits 18:16  1      the layout every captured value carries (CAPTURED)
bits 15:8   the dword inside the record the operand starts at
bits  7:0   the record's stride in dwords
```

so `m9_record_desc(form, stride, offset) = 0xee000000 | form << 19 |
0x00010000 | offset << 8 | stride`.

The `form` field takes three of its four values across the captures
(DERIVED; the fourth is unobserved):

| form | descriptor prefix | where it appears |
|---:|---|---|
| `1` | `ee09` | a two-dword operand: the two-scalar producers (`fs-tex2d-swizzle`, `fs-texrect`) and every five-sampler consumer |
| `3` | `ee19` | a four-dword producer slot, and a consumer whose third component is a **sampler modifier** -- the LOD bias of `fs-tex2d-bias`, the projector of `fs-tex2d-proj3` |
| `2` | `ee11` | a consumer whose third component **addresses the resource** -- the comparison reference of `fs-shadow2d`, the `r` of `fs-tex3d`, the major axis of `fs-texcube` |

The pass-splitting note reads bit 20 as moving with the record size.  That
holds for producers but not for consumers: the five-sampler consumers read a
sixteen-dword record with bit 20 **clear** (CAPTURED), so the field is a form
selector, not a size (DERIVED).

## 2. The route word

The word emitted at `0x74 + k`, immediately before that route's `0x78` base
and `0x79` descriptor:

```text
bits 31:24  component 3 source
bits 23:16  component 2 source
bits 15:8   component 1 source
bits  7:0   0x10 a coordinate route, 0x0c a value route, 0x00 idle
```

Component 0 comes from the descriptor's own start dword; components 1 to 3
are **record dword indices**, and `0xff` drops a component (CAPTURED, the
`00ff` of every two-component coordinate).  An idle route carries the
captured `0x00000100`.

A coordinate route (`0x10`) feeds the full sampler slot it precedes; a value
route (`0x0c`) arrives in the consuming pass as the register pair a texel
would, `(0x00 + 4k, 0x02 + 4k)`.

Reading the captures with that model:

| case | descriptor | route | components |
|---|---|---|---|
| `fs-tex2d-swizzle`, `fs-texrect` consumer | `ee090002` | `00ff0110` | `(d0, d1)` |
| `fs-tex2d-bias`, `fs-tex2d-proj3` consumer | `ee190204` | `01000310` | `(d2, d3, d0)` |
| `fs-shadow2d`, `fs-tex3d` consumer | `ee110204` | `01000310` | `(d2, d3, d0)` |
| `fs-texcube` consumer | `ee110004` | `03020110` | `(d0, d1, d2)` |
| five-sampler passes 1..4 | `ee0900{00,08,04,0c}10` | `00ff0{1,9,5,d}10` | `(dn, dn+1)` |
| five-sampler final, route 1 | `ee190210` | `0706030c` | `(d2, d3, d6, d7)` |
| five-sampler final, route 2 | `ee190a10` | `0f0e0b0c` | `(d10, d11, d14, d15)` |

The five-sampler record is sixteen dwords wide and each of its four producing
passes owns a four-dword slot, which is why the final pass can still gather
`a` from dwords 2..3 and `b` from dwords 10..11 after four passes have run
(DERIVED, and it is what the shader needs: `a.r + d.r`, `b.g + e.g`, `c.b`).

## 3. What stays open

* How the engine addresses the record **per fragment** is narrowed but not
  closed.  The descriptor is a **flat record array**: it carries a stride and
  a start offset and no pitch, width or height at all, which is the same
  shape `docs/parhelia-m9-raster-stream-limit-re.md` section 3 records for
  the preprocessing output streams
  (`b1 = 0xee19 | slot * 4 << 8 | slots * 4`, and `vs-varyings-4`'s
  `ee190014 ee190414 ee190814 ee190c14 ee191014`).  Those are indexed by
  vertex; a raster record is therefore indexed by the engine's fragment
  sequence (DERIVED).  What stays **OPEN** is the granularity -- one record
  per fragment, or per 2x2 quad, or per shaded sample -- and nothing in this
  tree measures it, so a driver sizes the record for at least one record per
  covered pixel and measures before trusting a tighter bound.
* The captured record strides are 2 dwords (8 B) for a one-pair producer,
  4 dwords (16 B) for a two-pair one, and 16 dwords for the five-sampler
  schedule's four-slot record.  At 1920x1200 and one record per pixel that
  is 17.6, 35.2 and 141 MiB respectively (DERIVED).
* The unobserved fourth `form` value is **OPEN**.

## 4. What the glamor captures close

The two items this note and the pass-splitting note left open were open only
because both were written from `m9x-vendor-glsl-programs-20260915` and the
five-sampler schedule.  `captures/m9x-vendor-glamor-shaders-20260915` settles
them (CAPTURED).

* **A consuming pass may declare interpolated inputs and a route at the same
  time.**  `xv-uyvy`'s final raster pass binds three textures, takes a
  coordinate route at `75` and another at `76`, and still declares attribute
  1 and attribute 5 -- so one unit interpolates its own coordinate while two
  read theirs from the record, alongside an ordinary varying
  (`22 = 00002403`, `88 = 0170033f`, `75 = 00ff0314` with `79 = ee090204`,
  `76 = 00ff0118` with `79 = ee090004`).  `glyphs-1x1-normal` is the
  two-unit form of the same shape and the five-sampler schedule's pass 2
  declares `00100d88` beside its coordinate route.
* **The route word's low byte names the slot it feeds: `0x10 + 4k`.**  A
  census over six capture sets finds `0x10` only at `74` (84 occurrences),
  `0x14` only at `75` (12) and `0x18` only at `76` (2), with no
  counter-example; index 3 never appears, so the driver does not extrapolate
  there.  `m9_record_route` took no index and hard-coded `0x10`, which would
  have emitted `00ff0110` at `75` where the hardware was captured taking
  `00ff0114`; it now takes the index.
* **Component 0 of a route is implicit**: it is the descriptor's own offset
  field, and the three bytes name the remaining components as record dword
  indices.  `01000310` with `ee190204` reads `(d2, d3, d0, d1)`;
  `00ff0314` with `ee090204` reads `(d2, d3)`.  The same encoding serves a
  value route with kind `0x0c`, and the vendor uses arbitrary, sometimes
  out-of-order indices there -- the driver's `4s+1, 4s+2, 4s+3` is its own
  layout choice, not a hardware rule.
* **Which byte carries a third operand depends on the consumer's form**:
  `ee09` has none (c2 is `0xff`, c3 `0x00`), `ee19` reads a scalar sampler
  modifier from component 3 (forced by `fs-tex2d-bias`, whose c2 is `s` and
  c3 the bias), and `ee11` reads a resource component from component 2
  (forced by `fs-shadow2d` and `fs-tex3d`, and confirmed by `fs-texcube`'s
  `03020110` at offset 0 reading `(1.0, 2t-1, 2s-1)`).  A consumer
  re-describes the producer's bytes with the form it needs, which need not
  be the producer's.

* **A unit the record steers names its route in its own selector word.**
  Index `0x70 + i` carries `0x18` in its low byte for a unit that
  interpolates its own coordinate and `0x1c + k` with bit 12 set for one
  whose coordinate comes from route `k`: the five-sampler schedule's pass 0
  is `00040118` and its passes 1..4 are `0004111c`, and `xv-uyvy` binds
  `70 = 00040118` for its interpolated unit beside `71 = 0004111d` and
  `72 = 0004111e` for the two routed at `75` and `76` (CAPTURED).  **Emitting
  the interpolated `0x18` on a routed unit stops the engine** (MEASURED,
  m9x's first coordinate route); with `0x1c + k` the engine takes the
  schedule and runs.  Nothing else about the unit changes -- it keeps its
  own `0x78`/`0x79`/`0x7a` texture packet and its `0x7c` sampler word.

## 5. What a record slot holds

**A producer's slot is four dwords of 32-bit float a fragment** (MEASURED,
`M9X_RECORD_DUMP` after m9x's own `coordroute` and `tex5` fixtures).
Component 0 sits at the descriptor's own offset dword, and the route word's
`c1`..`c3` name the dwords the rest sit in, exactly as section 2 reads them.

This was worth measuring because the format is not the descriptor's to
choose: a producing pass writes its root through registers `0x11` and `0x13`,
and a pass that writes the colour output `0x2d` instead sends the same value
down the colour path, which packs it into the render target's pixel format.
m9x did that by accident and the record came back as four dwords a **2x2
quad**, one pixel's `(s, t)` as two half-precision values a dword -- correct
values in a storage the route cannot address.  Read as a float, that dword is
about 1.6e-24, so a routed coordinate arrived as `(0, 0)` and the unit
sampled texel `(0, 0)` for every pixel.  So **the register a producing pass
writes, not the descriptor, decides the record's element format**, and a
driver that emits the captured descriptors can still produce an unreadable
record.

## 6. The record flags overlap the register-pair field

A record-writing pass sets two flags in index 88 one place higher for every
unit it binds (`0x0c << units`, `docs/parhelia-m9-fragment-pass-splitting.md`
section 3).  Index 88 also flags, at bits 15:4, the register pairs
`08/09`..`1e/1f` the engine may reuse.  **From two units up the record flags
land inside that field.**  A two-unit producer's flags are bits 4 and 5, the
pairs `08/09` and `0a/0b`, which is exactly where the register allocator's
general run starts for two bound units (`4 * units`).

MEASURED, `coordconv:4`: the producing pass carried both its taps' blue
through registers `08` and `0a` with 88 `= 0030fb3f`, and blue came back
short on 54092 of 76800 pixels -- by exactly `w * (red - blue)` of the tap
that pass weights at 0.5, with red, green and alpha exact.  Keeping those
pairs empty renders the fixture exact.

Which field wins at those bits is **OPEN**: no capture has a producing pass
that binds two or more units, so whether the flags really shift that far, or
the `0x0c << units` rule is only right for the zero- and one-unit producers
it was derived from, is undecided.  A capture of a vendor producer with two
bound units settles it.  A four-unit producer is a counter-example to the
simplest reading: its 88 is `00f003cf`, flagging the pairs texel slot 3 lives
in, and `conv7`, `conv10` and `conv16` render exactly (MEASURED).  The driver
reserves the pairs for the two-unit case because that is what is measured to
close `coordconv:4` at no cost to 228 fixtures, not because the field is
decoded.

## 7. What stays open

What stays **OPEN**: a coordinate route at index 3; whether the low byte
names the route index or the destination unit, which every capture leaves
indistinguishable because they are always equal; and c2 against c3 for a
projector specifically, since `fs-tex2d-proj3` is the only captured projector
and its two candidates both hold 0.5.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
