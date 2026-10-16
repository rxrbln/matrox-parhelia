<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 Mesa driver: what four recorded features actually cost to land

**Historical scope:** This September 18 investigation predates the working
four-target MRT, query and multiple-varying implementations. Use the
[October 6 M9 topic ledger](parhelia-m9-current-topics.md) for the current
queue; the sections below preserve the original evidence and implementation
boundaries at their stated date.

Four features were queued for the m9x Gallium driver on the strength of
`docs/parhelia-m9-driver-readiness.md`: multiple render targets, the
occlusion query, several constant spans per job, and a second vertex
varying.  Two of them are blocked on work in files this note does not own,
one is blocked on a decode the readiness table itself lists as open, and one
turned out to be attacking the wrong half of the driver.  This note records
what was measured, so the next attempt starts from evidence.

**Status 2026-09-18.**  Two of the four have moved since this note was
written.  MRT's surface side is implemented and the target-enable field in
index `ef` is now CAPTURED, but binding a second colour target stops the
engine and the fragment destination for target p is still only DERIVED -
`parhelia-m9-mrt-routing.md` supersedes section 1 below.  The occlusion query
is being implemented.  Sections 3 and 4 stand.  The conclusion of this note -
that MRT is blocked on the fragment output export rather than on target
binding - was confirmed, and the backlog entry claiming MRT was fully reverse
engineered was wrong: the vendor's MRT fragment programs were never captured.

Labels: **MEASURED** is a hardware result on the dual-DisplayPort M9128;
**CAPTURED** is read from a saved vendor stream or binary in this tree;
**DERIVED** is joined from those; **UNTESTED** is named where it is.

## 1. MRT is blocked on the fragment output export, not on target binding

`parhelia-m9-openbits-gl-capture.md` section 4 proves four attachments
render and read back exactly through the vendor ICD, and shows the four
address/descriptor pairs `b0/b1` .. `b6/b7` in the command stream.  Binding
those pairs is a small change in `m9_draw.c`, and the kernel already permits
it: `m9_index_allowed()` covers `0xb0 .. 0xbb` and `m9_index_reloc_kinds()`
gives `0xb0/0xb2/0xb4/0xb6` `COLOR | STREAM` (CAPTURED, read from
`drivers/gpu/drm/m9x/m9_submit.c`).

What is missing is the other half: **how a fragment program says that a
value goes to render target 1 rather than 0.**  Two rows of the readiness
table say so directly:

* the termination row - "terminal variants do not encode MRT target index";
* the blocker ledger - "four-target MRT export and retirement | descriptor
  and reloc plus Windows RT0..3 job/fence membership are exact, but **native
  output export** and Linux per-RT job-BO membership are unjoined".

The `(0x11 + 4j, 0x13 + 4j) -> 0xb0 + 2j` rule of
`parhelia-m9-vertex-output-target-re.md` is a **vertex**-stage rule; the
raster half reuses it for multi-pass target binding, where each pass writes
one target, not for several colour outputs of one fragment program.
`m9_fs_compile.c` refuses anything but `FRAG_RESULT_COLOR`/`DATA0` today
("unsupported fragment output"), and there is no decoded encoding for it to
emit instead.

So MRT needs the RT1 export decode first.  Until then the honest driver
state is `caps->max_render_targets = 1` and `fs->max_outputs = 1`, which is
what `m9_screen.c` already carries.  When the decode lands, the two lines to
change are, in `m9_screen.c`:

```c
   caps->max_render_targets = 4;
```

and in `m9_init_shader_caps()`:

```c
   fs->max_outputs = 4;
```

and neither should be applied before a per-target hardware probe confirms
the driver's own output-to-pair mapping.  The vendor's pairs appear in
reverse attachment order and the capture cannot say whether the hardware
numbers them downwards or the ICD permutes the outputs (DERIVED, ambiguous).

## 2. The occlusion query is blocked in the kernel command validator

`parhelia-m9-occlusion-query-re.md` recovers the whole record: one indexed
packet with header `0xe6e6e61e`, a relocated writeback address at index
`0x1e`, then three literals at index `0xe6` - `0x64000004 | (base & 0xf) << 8`,
`0x64000001 | ((base + 1) & 0xf) << 8` and `0x40008000` - with the fixed
selector pair 0/1 and a modulo-32 end-minus-begin result.

Every part of that is refused by `m9_submit.c` as it stands (CAPTURED, read
from the kernel tree):

* `m9_index_allowed()` does not list `0x1e`; the packet is rejected with
  `-EPERM` before any relocation is looked at.  Its neighbours `0x18` and
  `0x19` are listed, so the omission is the captured-triangle whitelist, not
  a decision.
* `m9_index_reloc_kinds()` has no `0x1e` entry, so the writeback address has
  no relocation kind.  The uapi enum already has `PARHELIA_M9_RELOC_QUERY`
  and nothing uses it.
* the three `0xe6` body literals are refused: index `0xe6` only accepts
  `m9_transition_control()`'s eight values, `m9_transition_length()`'s
  `0x49d00000 | n`, `0x00000003` and `0x42000000`.

A userspace query therefore cannot reach the engine at all.  The kernel
change is four small pieces: allow `0x1e`, give it `M9_RELOC_BIT(QUERY)`,
resolve a `QUERY` relocation to a writeback address with `BO_WRITE` access,
and accept the three query literals at `0xe6` (they are fixed, so an exact
match is enough).  Only then is the Mesa side - two two-dword snapshots, the
record at begin and end, and an unsigned 32-bit subtraction - worth writing.
Overflow, the second snapshot word and multisample scaling stay open, so the
result should be clamped and the extension advertised without them.

## 3. Several constant spans: it is the preprocessing span, not the raster one

The premise is right and the diagnosis in the queue was not.  On glmark2
`ideas` at 1920x1200 (MEASURED):

```text
jobs 50  draws 212  records 4322  frame 8.33 ms  submit 5.52 ms  ioctl 5.01 ms
splits: target 0  program 5  depth 2  mask 0  clip 0  uniforms 43  vertices 0
```

so 43 of the 50 jobs a frame end because the constants moved.  An
instrumented build separated the two spans and printed, for 31214 of 31214
splits over the scene:

```text
m9x: span split fs 0 vs 1 floats 0 fits 1 copies 1 arena 32 new 32
```

**Every one of them is the preprocessing (vertex) constant span; not one is
the raster (fragment) span.**  That is the per-object transform the queue
described, and it means a per-draw `8c/8d/8e` in the raster range - which was
implemented, offline-tested and then reverted - buys nothing:

* it changed `ideas` not at all: 50 jobs, 43 uniform splits, 8.33 ms, before
  and after (MEASURED);
* and it made glmark2 `build:model=horse` stop with SIGSEGV inside
  libgallium, reproducibly, where the same tree without it runs clean
  (MEASURED, A/B on one build tree).  The library is built stripped, so the
  fault was not localised; anyone reviving the idea should build with
  symbols first.

The change that would pay is several **preprocessing** draws in the
preprocessing range, one per constant span, each covering its own
four-aligned run of source records with `8c/8d/8e` restated ahead of it.
Two things make it more than a transcription of the raster case:

* the first span is the system prefix itself.  `M9_VS_CONST_OFFSET` is
  `0x10`, the appended tail reads its viewport and depth range from constant
  pairs 9..21, and `m9_draw_params_viewport()` writes those as prefix
  dwords.  A second span has to carry a copy of pairs 9..21, and it has to
  live somewhere with room: `0x10 .. 0x300` holds about three spans, so the
  spans beyond the first belong in their own region of the system object
  above the raster constant arena.
* whether a preprocessing draw at `(first, count)` writes the transient
  stream at `first` rather than at zero is **UNTESTED**.  It is what the
  raster draws assume of the single covering draw today, and what the split
  shader's `vertex_offset = i * records` assumes of the copies, so it is
  likely - but it is the first thing to probe, because getting it wrong
  overwrites the streams of the draws before it.

The prize is worth the care: `ideas` spends 5.01 ms of its 8.33 ms frame in
the submit ioctl across 50 jobs, and collapsing the 43 constant splits would
leave about 7.

## 4. A second vertex varying: the recipe stands, the cost is two programs

`parhelia-m9-vertex-output-target-re.md` section 4 is still the plan, and
nothing measured here contradicts it.  The cost that section understates is
step 1: the extra preprocessing entry is a **second compilation of the same
shader**, not a second emission of the same head.  `compile_impl()` pins the
position to `REG_POS_XY`/`REG_POS_ZW` and the one varying to
`REG_VAR_XY`/`REG_VAR_ZW` and writes exactly those two outputs; an entry
that writes varyings 1..4 into `(0x11, 0x13)`, `(0x15, 0x17)`, `(0x19, 0x1b)`
and `(0x1d, 0x1f)` needs the output set to become a table the compiler walks,
and both entries then have to fit one `c2` image.

The fused single-draw alternative of section 3 - patching the tail's `r1c`
and `r1d` scratch to `r03` and asking for `ef = 0x00a88800` - is two words
and would carry two varyings without a second draw, but it edits an
instruction whose opcode the ISA note calls undecoded and needs a
combination no capture contains.  It is the cheaper experiment and should be
run first, exactly as section 5(b) describes.

## 5. What did land

* the projective divide precondition (`m9x: refuse a projective fetch whose
  projector varies over a primitive`), with fixtures `projconst` and
  `projvary`: before, `projvary` differed in 76588 of 76800 pixels; after,
  it passes and the refused draw leaves the target untouched (MEASURED);
* four vertex attributes (`m9x: preprocess four vertex attributes, not
  three`), settled by the probe's new `attr4-*` variants and confirmed
  through the driver by an A/B on `tools/m9-mesa-vs.c` (MEASURED).

Suite after both: 96 PASS 0 FAIL on `m9-mesa-glsl`, 14 PASS 0 FAIL on
`m9-mesa-vs` in either path, `m9-batch-test` rc 0, `m9-vs-compile-test` 18/18
plus one refused, `m9-fs-compile-test` required exact 23/23.
