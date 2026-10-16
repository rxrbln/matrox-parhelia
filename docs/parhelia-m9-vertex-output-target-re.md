<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 preprocessing pass: which vertex output register feeds which bound target

The native vertex backend can fill exactly one of the four targets the m9x
driver binds in its preprocessing pass, because the register-to-target rule
was undecoded (`m9_vs_compile.h`, `M9_VS_MAX_VARYINGS 1`;
`docs/parhelia-m9-native-vertex-isa.md` section 6 item 1).  This note closes
it out of the saved vendor captures, states how a pass declares how many
targets it writes, and says what the driver has to emit.

Labels: **CAPTURED** is read from a saved vendor command stream or program
image in this tree; **DERIVED** is joined from captured facts, with the join
given; **MEASURED** is an earlier hardware result cited by its note;
**UNTESTED** is named where it is.  Nothing here was run on hardware.

Everything below is re-derivable with the tool added alongside this note:

```sh
python3 tools/m9-vertex-output-target-evidence.py --rule \
        captures/m9x-vendor-glsl-programs-20260915        # the join, one row per pass
python3 tools/m9-vertex-output-target-evidence.py \
        captures/m9x-vendor-glsl-programs-20260915/vs-varyings-4   # one case in full
```

## Result, in one paragraph

**Output register pair `(0x11 + 4j, 0x13 + 4j)` goes to the target bound at
index `0xb0 + 2j`, for `j = 0..3`; register `0x21` goes to `0xb8`.**  A pass
declares how many of them it writes in **index `0xef`**: bit 19 enables target
1 (`b2`), bit 21 target 2 (`b4`), bit 23 target 3 (`b6`), and bit 15 enables
the clip-code output `0x21` into `b8`.  Target 0 (`b0`) is always on.  Both
halves hold in **310 of 310** captured preprocessing passes with no exception.
The lead that the preprocessing pass reuses the raster pass's
`(0x11 + 4j, 0x13 + 4j)` convention is **right**; the reading of `0x02` as an
output register is **wrong** — `0x02` is an ordinary temporary that the
appended tail happens to copy into `0x11`.

## 1. The register-to-target rule (CAPTURED)

The join is mechanical: for every draw, take the targets bound at `b0/b2/b4/b6`
in that draw's own packet run, and take the destination registers of the
program the draw enters at `c3`.

| case | `c3` | `ef` | targets bound in the draw | output registers written |
|---|---|---|---|---|
| `vs-varyings-4` | `13` | `00000200` | `b0` | `11 13` |
| `vs-varyings-2` | `0b` | `00280200` | `b0 b2 b4` | `11 13 15 17 19 1b` |
| `vs-varyings-4` | `18` | `00a80400` | `b0 b2 b4 b6` | `11 13 15 17 19 1b 1d 1f` |
| `vs-varyings-4` | `27` | `00a80600` | `b0 b2 b4 b6` | `11 13 15 17 19 1b 1d 1f` |
| `vs-varyings-max` | `95` | `00080600` | `b0 b2` | `11 13 15 17` |
| `vs-varyings-2` | `1b` | `00088e00` | `b0 b2` + `b8` | `11 13 15 17` + `21`, and `19` as a temporary |

The rule is `(0x11 + 4j, 0x13 + 4j) -> b0 + 2j`, low register first, plus
`0x21 -> b8`.  This is the same statement
`docs/parhelia-m9-raster-stream-limit-re.md` section 3 makes for the raster
half; the preprocessing half uses it unchanged.  `vs-varyings-4`'s gather pass
(`c3=27`) is the clearest single witness: four targets, four `mov`s, one per
pair, nothing else in the program.

Two readings that the join **refutes**:

* `0x02` is not an output register.  `m9_vs_compile.h` documents `REG_VAR_XY`
  as `0x02`, but no bound target ever receives `0x02`.  What happens is that
  the appended tail contains `r11 = mov r02` (slot 16 of
  `m9_passthrough_c2_program`), so the head's `r02` reaches target 0 through
  `0x11`.  `REG_VAR_ZW = 0x13` is a genuine output register — it is `j = 0`'s
  high half, and the head writes it directly because the tail does not.
* the output pairs are not the register pairs the hardware counts elsewhere.
  Target `j` uses the odd register of hardware pair `8 + 2j` and of pair
  `9 + 2j`; the even registers `0x10, 0x12, 0x14 ... 0x20` are never output
  registers in any capture.

### Where the m9x driver already sits on this rule

`m9_draw.c` binds `b0 = VIEW(0)`, `b2 = VIEW(3)`, `b4 = VIEW(1)`,
`b8 = VIEW(2)`, and `m9_raster_fs_init_textured()` declares the raster streams
as `view 0 = attribute 1` (the varying), `view 1 = attribute 0` (the
rasterizer position), `view 2 = attribute 12` (the clip code, stride 6),
`view 3 = attribute 13` (the clip position).  Running that through the rule:

| `j` | register pair | index | transient view | raster attribute |
|---|---|---|---|---|
| 0 | `0x11 / 0x13` | `b0` | 0 | 1, the varying |
| 1 | `0x15 / 0x17` | `b2` | 3 | 13, the clip position |
| 2 | `0x19 / 0x1b` | `b4` | 1 | 0, the rasterizer position |
| 3 | `0x1d / 0x1f` | `b6` | — | not bound |
| — | `0x21` | `b8` | 2 | 12, the clip code |

and that is exactly what the appended tail writes: `0x11 = mov r02`,
`0x15/0x17 = mov r00/r01`, `0x19/0x1b` the viewport-transformed position,
`0x21` the clip code (CAPTURED, disassembly of
`m9_passthrough_c2_program` slots 14..38).  The driver's binding order, which
looks arbitrary in the source, is the tail's own semantic order.  **`b6` is
the one free slot and `0x1d/0x1f` is the register pair that feeds it.**

## 2. How a pass declares the targets it writes: index `0xef`

`0xef` is the index the driver already emits as the constant `0x00288800` for
its preprocessing pass and as `cull | depth_ef` for a raster pass.  In a
preprocessing pass its upper half is the output declaration (CAPTURED, all
310 passes of `captures/m9x-vendor-glsl-programs-20260915`, and the same
values across `m9x-vendor-gl2ext-2d-video-modes-20260915`,
`m9x-vendor-glamor-shaders-20260915` and `m9x-vendor-glamor-blend-20260915`):

| bit | meaning | check |
|---|---|---|
| 19 | target 1 (`b2`, register pair `0x15/0x17`) enabled | 310/310 |
| 21 | target 2 (`b4`, pair `0x19/0x1b`) enabled | 310/310 |
| 23 | target 3 (`b6`, pair `0x1d/0x1f`) enabled | 310/310 |
| 15 | clip-code output `0x21` into `b8` enabled | 310/310 |

so the byte at `ef[23:16]` takes exactly four values: `0x00` (one target),
`0x08` (two), `0x28` (three), `0xa8` (four).  Both checks are two-sided: no
pass binds a target the declaration does not enable, and no program writes
`0x21` unless bit 15 is set.  A program that writes an output register whose
target the declaration does not enable is simply using it as a temporary —
`vs-builtin-light`, `vs-lighting` and `vs-texlod` all write `0x1d`/`0x1f`
with `ef` bit 23 clear, and only three targets bound.

Bits 11:9 are **decoded** in
`docs/parhelia-m9-varying-transport-re.md` section 4: bits 10:9 say where the
draw's vertex inputs come from -- 1 opens a chain on the source record, 2
continues it there, 3 reads the intermediate record through the `0x74`
routes, 0 is a draw that is the whole chain -- and bit 11 marks the draw that
produces the final vertex outputs, which is why it moves with the clip-code
enable at bit 15.  The five values below are all five combinations that
occur:

| pass role | `ef[11:8]` | example |
|---|---|---|
| a head pass, first preprocessing draw of the submission | `2` | `vs-varyings-2 c3=0b`, `fs-abs c3=07` |
| a head pass that follows another head pass | `4` | `vs-varyings-4 c3=18`, `vs-varyings-max c3=49` |
| a pass fed by `0x74` route bindings, no clip code | `6` | every gather pass |
| the fused single pass: head, tail and clip code in one draw | `8` | `vs-texcoord c3=04`, and the m9x driver |
| the split tail: route-fed, clip code | `e` | `fs-abs c3=1d`, 95 identical instances |

Nothing outside that set of five occurs.

### Index `0x88` carries the input declaration, not the output one

Decoded here as a by-product, because the driver hardcodes it and one of its
fields is wrong for anything but the captured shape:

* **`88[27:24]` is the unary count of declared source attributes** — `0x1`,
  `0x3`, `0x7`, `0xf` for one to four.  308/310 (the two exceptions are passes
  that inherit a declaration emitted by an earlier draw, which the tool's
  per-draw grouping loses, not counter-examples).  `vs-uniform-color` is the
  clean witness: the same fused pass shape as the driver's, the same
  `ef = 0x00288800`, the same tail, one attribute instead of two, and
  `88 = 0x0100bf00` against the driver's `0x0300bf00`.
* **`88[23:20]` is the unary count of bound `0x74 + k` route bindings**
  — 310/310.  A route left at its idle value `0x00000100` does not count.
  This is the same field `docs/parhelia-m9-raster-stream-limit-re.md` section 4
  calls the unary texture-unit count in a raster pass; the four hardware source
  slots are shared between the two stages.
* **`88[15:0]` is a per-register-pair mask** (pair `p` = registers `2p`,
  `2p + 1`), with bit `p` **clear** meaning the pass uses that pair.  Read as
  "pairs the program writes that are not enabled outputs" it reproduces 215 of
  310 values exactly, including every pass the driver copies
  (`0xbf00` = pairs 0..7 and 14, which is precisely `r00..r0f` plus the tail's
  `r1c`/`r1d`).  The other 95 mark more pairs than that (`fs-abs` reserves
  pair 3 with nothing in it) or mark pairs that hold live outputs
  (`vs-lighting`'s `0x3c00`), so the exact rule is **OPEN**.  A driver that
  keeps the captured value for the pass shape it copies is safe; one that
  invents a value is not.

**A latent bug this exposes (DERIVED, UNTESTED).**  `m9_draw.c` emits
`88 = 0x0300bf00` unconditionally, which declares **two** source attributes.
`tools/m9-user-vertex-probe.c` inherits the same word from
`tools/m9-user-triangle-data.h` and never rewrites it, while its
`build_decl()` does grow the index-23 declaration for a third attribute.  That
is exactly the failure `m9_vs_compile.h` records for `M9_VS_HW_ATTRIBUTES`:
"a third draws geometry from neither its own value nor a decoy, both in the
userspace probe and through this driver".  The prediction is that the third
attribute works as soon as `88[27:24]` is `0x7`, and the fourth at `0xf`.  The
probe change is one line; see section 5.

## 3. Whether the appended tail can carry a second varying

**No, not as it stands, and for two independent reasons (CAPTURED).**

The 25-slot tail the driver appends (`m9_passthrough_c2_program` slots 14..38)
already occupies three of the four targets and the clip code.  The driver's
whole 36-slot vertex program, head and tail, is byte-identical to the vendor's
in `ff-color` and `ff-alpha-test` (CAPTURED), so the corpus reads on it are
reads on the shipping program:

```text
slot 16  r11 = mov r02                 target 0 low  <- the one varying
         r13                            target 0 high <- written by the head
slot 19  r15 = mov r00                 target 1      <- clip position
slot 21  r17 = mov r01
slots 25,27,28  r19, r1b               target 2      <- viewport position
slots 29,38     r21                    b8            <- clip code
```

and it uses `r1c` (live across slots 24..27) and `r1d` (live across slots
37..38) as temporaries.  `r1d` is target 3's **low** register, so a head that
wrote a second varying into `0x1d/0x1f` would have `0x1d` overwritten by the
tail before the draw.  Every one of the fifteen distinct clip-code-producing
epilogues in the corpus writes `r1d`; the vendor treats it as scratch
precisely because it never enables target 3 in the same pass.

Nor does any capture show the combination the fused form would need.  Across
the preprocessing pass of **every** submission under `captures/`, `ef` bit 23
occurs only as `0x00a80200`, `0x00a80400` and `0x00a80600` — never once with
bit 15.  **No vendor
preprocessing pass writes four targets and the clip code in the same draw.**
Whether that is a hardware restriction or only the vendor's scheduling habit
is **OPEN**; the captures cannot distinguish them.

The vendor's own answer to more than one varying is to split the
preprocessing into several draws inside one range, and the corpus contains
the complete alternative epilogue that goes with it:

* the **split tail** (`fs-abs c3=0x1d` and 94 byte-identical instances,
  `ef = 0x00088e00`, `88 = 0x0010ef03`) reads the clip position back through
  route binding 0 as `r00`/`r02`, writes target 0 = the clip position,
  target 1 = the viewport position and `0x21` = the clip code, and uses
  `r04..r0f` and `r18`/`r19` as temporaries.  It leaves `r1c..r1f` free — but
  it clobbers `r19`, which is target 2, so a pass using it can bind at most
  two targets and the clip code, exactly as its `ef` says.

So both captured epilogues leave room for **one** varying beside the
positions.  The way to a second one is another draw, not another register.

If the fused single-draw form is wanted anyway — it would carry two varyings
and save a draw — the minimal change is to move the tail's scratch out of
`r1c`/`r1d`.  Their live ranges are disjoint and `r03` is untouched by the
tail (the head only reads it as the partial dot's `c` selector), so both can
become `r03`, a two-word patch with the fields the disassembler already
decodes:

```text
slot 37   W0 437f9d1c -> 437f831c        d: 0x1d -> 0x03
slot 38   W0 bfffa11c -> 7fffa11c        a: 0x1d -> 0x03
          W1 ac8401c3 -> ac8401c0        (a's high bits live in W1[3:0])
```

This is **UNTESTED**, it edits an instruction whose opcode
(`op2 = 0x04`) is one of the two the ISA note calls undecoded, and it still
depends on `ef = 0x00a88800` being accepted.  It is an experiment, not a plan.

## 4. What the driver has to change

The recommended shape uses **two preprocessing draws in two ranges**, both of
them shapes the captures already contain, and needs no new hardware feature,
no route binding, no intermediate arena and no kernel change (`b6`/`b7` are
already inside `m9_index_allowed()`'s `0xb0..0xbb`, and `m9_index_reloc_kinds()`
already gives `0xb6` `COLOR|STREAM`).  Draw 1 carries varyings 1..4, draw 2 is
today's pass unchanged and carries varying 0 and the three position streams.
Up to five varyings, which is inside the raster declaration limit of six.

Ordered, by file and function:

1. **`m9_vs_compile.h` / `m9_vs_compile.c`** — raise `M9_VS_MAX_VARYINGS` to 5
   and let `struct m9_vs_program` hold an array of varyings
   (`varying_location[]`, `varying_components[]`, `varying_count`) instead of
   the single `has_varying` / `varying_location` pair.  `m9_vs_compile()` keeps
   pinning varying 0 to `REG_VAR_XY`/`REG_VAR_ZW` as today.  Varyings 1..4 are
   compiled into a **second entry**: a head that computes only those values and
   writes them to `(0x11, 0x13)`, `(0x15, 0x17)`, `(0x19, 0x1b)`,
   `(0x1d, 0x1f)` with **no tail and no stop-pair tail constants**, just the
   stop pair.  `m9_vs_emit()` gains an `entry` selector or a second emitter;
   `m9_vs_c2_image()` lays both entries out and reports both entry slots.  The
   extra entry reuses the same attribute declaration and the same constant
   span, so `m9_vs_decl_words()` and `m9_vs_constants()` are unchanged.
2. **`m9_draw.h`** — `struct m9_draw_job` gains the second preprocessing pass:
   `unsigned vs_extra_entry` (0 = absent), `unsigned vs_extra_targets` (1..4)
   and `uint32_t vs_extra_view[4]`.
3. **`m9_draw.c`, `m9_batch_emit_draw_job()`** — this is the part that is not
   mine to edit; it needs, in order:
   * factor the existing preprocessing block into a helper taking `(entry,
     ef, r88, const_pairs, targets[], target_count, bool clip_code)`;
   * emit the **new draw first**, with
     `ef = 0x00000200 | (n > 1 ? 0x00080000 : 0) | (n > 2 ? 0x00200000 : 0) | (n > 3 ? 0x00800000 : 0)`
     for `n` extra varyings — that is `0x00000200`, `0x00080200`, `0x00280200`,
     `0x00a80200`, all four CAPTURED — binding
     `EMIT(R(0xb0 + 2 * k, job->transient, VIEW(vs_extra_view[k]), STREAM),
      V(0xb1 + 2 * k, 0x68002000))` for `k < n`, **no `0xb8` bind and no
     `0x21`**, `c3 = job->vs_extra_entry`, and otherwise the same `88`, `22`,
     `8c/8d/8e`, source-record, declaration and linked-record words the
     existing pass emits;
   * close it with `m9_batch_emit_draw_numeric()` and
     `m9_batch_emit_range_break()`, exactly as the existing preprocessing pass
     is closed — the vendor separates its preprocessing draws with an `e6`
     epilogue the m9x kernel refuses, and the range break is the stronger
     barrier the same way `docs/parhelia-m9-fragment-pass-splitting.md`
     section 6 concluded for raster passes;
   * shift the raster ranges' dependency base, which is
     `dep = 0x20 + M9_PASS_DEPENDENCY_STEP * index` in
     `m9_emit_raster_range()`, by one step when the extra draw is present;
   * fix `EMIT(V(0x88, 0x0300bf00), ...)` in the existing pass to
     `0x0000bf00 | ((1u << job->vs_attr_count) - 1) << 24` so the attribute
     field follows the declaration instead of being pinned at two — see the
     prediction in section 2, and keep the low half at `0xbf00`, which is the
     captured value for this exact pass and tail.
4. **`m9_draw_params.h`** — `M9_TRANSIENT_VIEWS` already allows 11 views and
   `m9_stream_is_cpu()` only diverts views `>= M9_TRANSIENT_CPU_VIEW` when
   `job->cpu_streams` is set, which a native-vertex job leaves at zero.  So
   native varying `k >= 1` can take view `3 + k` with no constant change at
   all; add a comment saying so.
5. **`m9_context.c`, `m9_job_native()` and `m9_varying_from_pass()`** —
   `m9_varying_from_pass()` becomes a lookup returning the varying's index in
   the vertex program, and `views[k]` becomes `0` for index 0 and `3 + index`
   for the rest; the `cpu` counter keeps numbering only the varyings the
   vertex program does not produce.  Fill `vs_extra_entry`, `vs_extra_targets`
   and `vs_extra_view[]` in the same place `job.vs_decl` is filled.
6. **`m9_context.c`, `m9_native_vs_raster_ok()`** — drop
   "the fragment program reads more than one varying" and instead require that
   every varying the fragment program reads is one the vertex program writes,
   and that `fp->varying_count <= M9_VS_MAX_VARYINGS`.
7. **`m9_vs_compile_test.cpp`** — an offline case per target count asserting
   that varying `i` lands on register pair `0x11 + 4i` / `0x13 + 4i` in the
   extra entry, and that the fused entry still writes `0x13` and leaves
   `0x1d` alone.

Nothing in the kernel changes.  Nothing in `m9_fs_compile.c` changes: the
fragment side already interpolates six inputs from `0x40 + 8k` and
`m9_raster_fs_init_textured()` already declares attributes `1..4, 9, 10`, one
stream slot each — the raster half has been ready since
`docs/parhelia-m9-raster-stream-limit-re.md`.

## 5. The shortest hardware tests

Two, in increasing cost.  Both belong in `tools/m9-user-vertex-probe.c`, which
already builds a one-draw preprocessing pass with the same fused tail, so the
existing pass/fail machinery and traceable colours carry over.

**(a) The `88` attribute-count prediction, four lines.**  The probe's `attr3`
variants already fail.  Its `submit()` loop already rewrites index `0x22` with
`ndecl - 1` and feeds index `0x23` from `build_decl()`, but leaves index `0x88`
at the captured `0x0300bf00`.  Add the matching branch next to them:

```c
else if (idx == 0x88)
        *p = (*p & ~0xff000000u) |
             ((1u << v->color_attr) - 1) << 24;
```

which reproduces `0x0300bf00` for the two-attribute variants and gives
`0x0700bf00` for `attr3`.  If `attr3` then paints the traceable colour it was
given, `88[27:24]` is confirmed and `M9_VS_HW_ATTRIBUTES` goes from 2 to 4.
This costs one submission and settles a limit that is currently costing the
driver two vertex attributes.

**(b) The target rule itself, one new variant.**  In the same probe:
bind `b6` to a fifth transient view with `b7 = 0x68002000`; set
`ef = 0x00a88800`; extend the raster declaration with attribute 2 on a new
stream slot reading that view with stride `0x10` (index `22`'s word count
grows by two); patch the tail's `r1d` to `r03` in the two words of section 3;
and have the head write a value that identifies both the view and the vertex
into `0x1d`/`0x1f`.  Three outcomes, all informative:

* the fifth stream interpolates that value — the rule and the fused four-target
  form are both confirmed, and section 4 can drop its second draw for the
  two-varying case;
* the draw is refused or the engine stops — `ef` bit 23 and bit 15 are
  mutually exclusive, and section 4's two-draw plan is the only route;
* the stream reads as another input's value — the tail patch is wrong, and
  re-running with `ef = 0x00a80200`, no clip code and the clip-code stream
  left from the previous draw isolates which half failed.

Run either inside `flock -w 1800 /root/tmp/m9-hw.lock`.  Note the standing
warning in `docs/parhelia-m9-native-vertex-isa.md`: the probe must never read
an attribute register it did not declare, which locks the engine until the
machine is restarted.

## 6. What is still open

* ~~`ef[11:9]`, the five-valued pass-role field of section 2.~~  Decoded in
  `docs/parhelia-m9-varying-transport-re.md` section 4, and the first thing
  to re-test: the extra draw's `1` was right and the fused pass's `4` was
  wrong, because a chain that opens must close with `7`.
* `88[15:0]`, the register-pair mask: a "pairs the program uses" reading fits
  215 of 310 passes and fails the other 95 in both directions.  Whether it is
  a correctness requirement or a scheduling hint is undecided; the driver
  already ships a value that does not match its compiled heads, which is weak
  evidence for a hint.
* whether one pass may write four targets **and** the clip code.  Not captured
  either way; test (b) settles it.
* the split tail's constant-span layout, which a driver adopting the vendor's
  three-draw chain would need.  Not decoded, and not needed by section 4.

## The attribute-count prediction is confirmed on hardware (MEASURED 2026-09-17)

Test §5a run on the dual-DisplayPort M9128 at 192.168.1.111.  The probe's
`submit()` loop now rewrites index `0x88` the way it already rewrites `0x22`
and `0x23`:

```c
else if (idx == 0x88)
        *p = (*p & ~0xff000000u) |
             ((1u << v->color_attr) - 1) << 24;
```

A/B on the same binary tree, same board, same run:

| variant | `88[27:24]` | t0 | t1 | t2 | decoy |
|---|---|---|---|---|---|
| `attr3-r56`, unpatched | `0x3` (two attributes) | 0.0% | 11.6% | - | 0 |
| `attr3-r56`, patched | `0x7` (three attributes) | **100.0%** | **100.0%** | **100.0%** | 0 |

So **`88[27:24]` is the unary count of declared source attributes**, and the
third attribute works the moment it is declared.  The other two variants place
the read correctly: `attr3-r46` reads attribute 1's register and sees nothing,
`attr3-r4e` reads the decoy's, and `attr3-r56` is `0x46 + 8 * (3 - 1)`, which
is the documented `REG_ATTR0 + 8 * (a - 1)` rule.

This closes the failure recorded in `m9_vs_compile.h` - "a third draws geometry
from neither its own value nor a decoy, both in the userspace probe and through
this driver".  It was never a hardware limit: `m9_draw.c` emits
`EMIT(V(0x88, 0x0300bf00), ...)` unconditionally, which declares two source
attributes whatever the declaration says, so a third attribute was read from a
slot the pass never declared.

**Driver change still to make:** drive `0x88[27:24]` from the attribute count in
`m9_draw.c`'s preprocessing block, then raise `M9_VS_HW_ATTRIBUTES` from 2.

## The fourth attribute reads exactly too (MEASURED 2026-09-18)

`tools/m9-user-vertex-probe.c` gained the `attr4-*` variants the note above
asked for: four declared attributes, decoy values in the three below the one
read, and the read register swept over all four.

The captured stream holds only **eight** index-23 value slots in its
preprocessing pass, and four attributes need nine words - two per attribute
plus the terminator.  The first run of the variants therefore wrote no pixel
at all, because the declaration was never terminated; that is a probe limit,
not a hardware one.  `submit()` now grows the pass by one single-value
index-23 packet per missing word and shifts the relocation sites behind it,
which is also what a driver has to do for any declaration longer than the
captured one.

| variant | register read | interior correct | decoy |
|---|---|---|---|
| `attr4-r46` | attribute 1, the position | 0.0% | 0 |
| `attr4-r4e` | attribute 2 | 0.0% | 100% |
| `attr4-r56` | attribute 3 | 0.0% | 100% |
| `attr4-r5e` | attribute 4 | **100.0%** | 0 |

So `REG_ATTR0 + 8 * (a - 1)` holds through `a = 4`, `88[27:24] = 0xf`
declares four, and each lower register holds exactly the decoy it was given.
`M9_VS_HW_ATTRIBUTES` is 4.

Through the driver, `tools/m9-mesa-vs.c` A/B on one board and one run:

| `M9_VS_HW_ATTRIBUTES` | `attr4` | `attr4b` |
|---|---|---|
| 3 | native vertex draws 0, draw module 1 | native 0, draw module 1 |
| 4 | **native vertex draws 1**, draw module 0 | **native 1**, draw module 0 |

both exact in either path, and the whole 14-case native-vertex suite passes.

## Section 4's two-draw plan does not deliver its streams (MEASURED 2026-09-19)

The plan of section 4 was implemented in full and run on the dual-DisplayPort
M9128 at 192.168.1.111.  **The driver side works and the engine does not.**
The vertex backend compiles up to five varyings and the second head verifies
against the NIR reference offline, but every draw whose fragment shader reads
a varying the extra preprocessing draw writes renders **nothing**, and the
submission comes back `EBUSY` with the context lost.

What was built, all of it as section 4 specifies: `M9_VS_MAX_VARYINGS` 5; a
second, tail-free head writing varyings 1..4 to `(0x11, 0x13)` ..
`(0x1d, 0x1f)` and stopping; a preprocessing draw of its own ahead of the
fused one, binding one stream target a varying at `0xb0 + 2k` with
`0x68002000`, no `0xb8`, no `0x21`, `ef = 0x00000200` plus one enabled bit a
target past the first, closed with a numeric draw and a range break; two
dependency steps for that range and every range behind it shifted by `0x20`;
varying `k >= 1` on transient view `M9_TRANSIENT_VS_EXTRA_VIEW + k - 1`, with
the job binding no CPU stream object so those views stay local.

| case | varyings | targets the extra draw binds | result |
|---|---|---|---|
| `vary2` | 2 declared, **1 after the linker packs two `vec2`s into one slot** | none | exact |
| `vary3` | 3 | 2 | **nothing rendered, context lost** |
| `vary4` | 4 | 3 | **nothing rendered** |
| `vary5` | 5 | 4 | **nothing rendered, context lost** |

Every one of the four is exact through the draw module in the same binary
(`M9X_VS_VARYINGS=1`), against the same software reference, so the fixtures
and their references are sound and the vertex transform is not what differs.
`vary2` is not evidence that one target works: GLSL varying packing leaves it
with a single varying, so its job emits no second draw at all.  **No
configuration has been observed in which the extra draw delivered a stream.**

Two hypotheses were tested and refuted:

* **index `0x88`'s pair mask.**  The fused pass's captured `0xbf00` leaves
  pair 14 -- registers `0x1c`/`0x1d` -- clear, and the tail-free head writes
  `0x1d` as target 3, so carrying `0xbf00` over looked like the cause.  Every
  captured head pass declares a run of pairs up to 15 instead (`0xffff`,
  `0xfffe`, `0xfffc`, `0xfff8`, `0xfff0`, `0xffe0`).  Emitting `0xffff`
  changed nothing.
* **the barrier between the two preprocessing draws.**  A range break is what
  section 4 proposed, the vendor using an engine transition the kernel
  refuses.  `M9X_VS_EXTRA_BREAK=0`, both draws inside one range, changed
  nothing.

What the captures say that section 4 did not draw out: **no vendor head pass
writes four targets into four separate stream views.**  Re-reading the corpus
target by target:

| pass | targets | where they point |
|---|---|---|
| fused, 3 targets + clip code (`ff-color` `c3=04`) | `b0 b2 b4 b8` | four separate views `0x16800` apart, `desc 68002000` |
| head, 4 targets (`fs-abs` `c3=07`, 83 instances) | `b0 b2 b4 b6` | **one** buffer `0x10ec1000`, `desc ee1900/0410/0810/0c10` -- four dword offsets of one interleaved record |
| head, 4 targets (`vs-varyings-4` `c3=18`) | `b0 b2 b4 b6` | **one** buffer, `desc ee190014/0414/0814/0c14` |
| gather, 4 targets (`vs-varyings-4` `c3=27`) | `b0 b2 b4 b6 b8` | separate views, `desc 68002000`, but route-fed from `0x74..0x77` |

So the separate-view form with `68002000` is captured only for the fused pass
and for a route-fed gather pass, never for a source-record-fed head pass
writing more than one target.  The vendor's own multi-target head passes write
**one packed record**, which a later route-fed draw reads back.

**The inference drawn here, that the raster stage then reads that record as
several streams at dword offsets of one address, is wrong.**
`docs/parhelia-m9-varying-transport-re.md` section 5 surveys 814 captured
raster passes: every stream stride is `0x10` except the clip code's `6`, so
the raster stage only ever reads a flat 16-byte-per-vertex array an
interpolant, and every multi-draw preprocessing chain ends in a draw that
de-interleaves the record into exactly those.  `m9_raster_fs_init_textured`
does not need to express anything new.

The lead that replaces it is index `ef` itself — which was emitted, run, and
**refuted**; see the closing section below.  Bits 11:9 of a preprocessing
pass state the draw's place in its chain and move with the clip-code enable at
bit 15, in 628 of 628 captured passes with no exception: the only draw of a
chain is `4`, the first of several `1`, a later source-record-fed one `2`, a
route-fed one `3` and the last of several `7`.  The run above emitted `1` for
the extra draw and left the fused pass at `4`, *the only draw of a chain* --
a chain that opens and never closes, which is not a state the grammar has.
The vendor's word for a fused-shaped final draw behind one other
preprocessing draw is `0x00288e00`, and `captures/m9x-vendor-openbits-20260917/cap-attribs_14`
carries it with the same three bound targets and clip code this driver binds.
`m9_draw.c` now emits it (`M9_PRE_ROLE_LAST`), offline-gated and untested on
hardware; the fixtures below are the run that settles it.

Status in the tree: the vertex backend, the second head, the extra draw and
the view mapping are all present and covered by tests; the driver's default
cap is **one varying** (`M9X_VS_VARYINGS`, `m9_context.c`), so the extra draw
is emitted only when the knob is raised.  The fixtures `vary3`, `vary4` and
`vary5` in `tools/m9-mesa-glsl.c` are the reproducer.

## The chain role does not deliver either (MEASURED 2026-09-19)

`M9X_VS_VARYINGS=5` on the M9128 with the chain-role word in place, each run
opened by a passing `texture` fixture: `vary3`, `vary4` and `vary5` render
every pixel wrong and stall the engine with a kernel stream timeout.  `vary3`
was re-run alone after a reboot and failed identically, so it is not a side
effect of an earlier stall.  The submission was dumped on the card and carries
exactly the intended words — `ef = 0x00000200` on the extra draw and
`ef = 0x00288e00` on the fused pass.

Two corrections to the table above.  Mesa's varying optimiser now folds
`vary3`'s third varying into the second as a swizzle, so `vary3` declares two
interpolants and its extra draw binds **one** target, not two; the minimal
second preprocessing draw is what fails.  And the failure's outward shape is a
**kernel stream timeout**, which the `EBUSY`/context-lost of the earlier run
is the downstream symptom of.

So three hypotheses were refuted here: index `0x88`'s pair mask, the barrier
between the draws, and the chain role.  The one deviation left — separate
`0x68002000` stream views where every captured chain writes a packed
intermediate record that a route-fed draw reads back through `0x74 + k` —
was then built in full and run, and it renders nothing either
(`docs/parhelia-m9-varying-transport-re.md` section 12).  Four shapes have
now been refused.

**The second preprocessing draw needs something that is not in any capture
this tree holds.**  The next step on *that* route is a new vendor capture of
a multi-draw chain, not a further reading of the ones here -- and the second
varying no longer waits on it: the section below carries two of them in the
single fused draw, which raises the cap to two.  One defect was found on the way and is worth keeping: the
tail-free head never carried the slot marker at W1 of slot `n - 4` that every
captured native program has, because the fused pass inherits it from its
appended tail.  Both tail-free programs place it now, and the gather the
driver emits is byte for byte the vendor's own.

## Two varyings in one draw, on the clip position's target (MEASURED 2026-09-19)

The fifth and sixth experiments on this feature were run on the
dual-DisplayPort M9128 at 192.168.1.111, each hold bracketed by the native
`varying` fixture, each stall followed by a reboot and a module reload before
the next measurement.  **One of them works**, and it needs no engine state
the driver was not already emitting.

### Output target 3 is refused, in every shape tried

The obvious free target is 3: the appended tail writes targets 0, 1 and 2 and
scratches `r1c`/`r1d`, and `r1d` is target 3's low half.  Moving that scratch
to `r1e` -- two decoded register fields, the destination at W0 bits 14:8 of
tail slot 23 and the `a` operand at W0 bits 31:29 of slot 24 -- frees the
pair, and `r1e` is an ordinary temporary in the vendor's own fused passes
(CAPTURED, `vs-texlod` `c3=08` writes `0x1c`, `0x1d`, `0x1e` and `0x1f`).

**That edit is sound**: with it forced on every program, `varying` and `mul`
are pixel-exact and take the native path -- the driver says so itself under
`M9X_DEBUG` -- and `texture`, which falls back to the draw module, is exact
too (MEASURED).  What fails is enabling the target.

| configuration, all with `ef` bit 23 set | result |
|---|---|
| clip code bound as usual, mask `0x3f00` | stream timeout |
| clip code bound, mask `0xff00` | stream timeout |
| clip code bound, mask `0xffff` | stream timeout |
| clip code bound, index `ba = 0` | stream timeout |
| clip code dropped, `b8` simply not restated | stream timeout |
| chain role forced to the captured head word `0x00a80200` | stream timeout |
| four targets into one packed record, `ee19` descriptors | stream timeout |
| clip code disabled the vendor's way, `b8 = 0`, `b9 = 0xb8010001`, `ba = 0` | **runs**, wrong values |

and `ef` bit 23 clear -- the target bound at `b6` but not enabled -- renders
wrongly and does **not** stall, so the binding is harmless and the enable is
what the engine refuses.  This is exactly what the corpus says: of 628
captured preprocessing passes, the 91 that enable target 3 carry `ef`
`0x00a80200`, `0x00a80400` or `0x00a80600` and **not one of them enables the
clip code at bit 15**; 87 of the 91 state the disabled clip-code form
`b8 = 0`, `b9 = 0xb8010001` explicitly.  A pass may have its fourth output
target or its clip code, not both.

### The clip position is what the raster stage can do without

The raster pass declares three fixed attributes beside its interpolants:
0 the rasterizer position, 12 the clip code at stride 6, 13 the clip
position.  Dropping either of the latter two from the declaration and its
stream renders **the same pixels** (MEASURED, the `varying` fixture, three
targets, known-good before and after).  So the target the fused pass can
spare is **1**, the clip position's, whose register pair is `0x15`/`0x17`.

The two tail slots that write it -- slots 5 and 7 of the appended tail --
have no reader, and the tail's scratch `r1c` is written afresh further down
before anything reads it, so pointing both destinations at `r1c` frees the
pair and leaves index `88` exactly the captured `0x0300bf00`.  The head then
writes the second varying into `0x15`/`0x17`, `b2` is bound to that varying's
transient view instead of the clip position's, and the raster pass declares
no attribute 13.

**Nothing else changes**: `ef` stays `0x00288800`, three targets and the clip
code, the packet count is identical, and the pass is the same one the
one-varying job has always run.

| fixture | cap | result |
|---|---|---|
| `varying` | 1 | exact, native, 1 varying |
| `vary3` (two packed varyings) | 2 | **exact, native, 2 varyings** |
| `varying` again | 1 | exact, native |

no stream timeout in the hold, and the known-good fixture passes after.

### The fixture suite

At the cap of two the m9-mesa-glsl suite fails exactly the sixteen cases the
last recorded run does -- the ten `dxt*` ("texture levels beyond the captured
32x16 tail need the kernel texture window"), the four `giga10*`, `mrt10:2`
and `mrt10:4` -- with 0 non-M9 and 0 stream timeouts.  One earlier run of the
same suite also failed `conv16d`, `conv3mixd` and `conv3zunder`; those three
**pass in isolation on this build and on the base build alike** (MEASURED,
both prefixes in one hold) and passed in the later full run, so they are an
ordering effect inside a suite run rather than this change.

### What is still open

`vary3big` draws the same two varyings on a quad three times the target, with
every attribute extrapolated along its own triangle's plane so the reference
image is unchanged, and it is **exact** (MEASURED) -- so eight ninths of the
geometry outside the target costs the picture nothing without the clip
position.  What that does not cover is geometry behind the eye, `w <= 0`, for
which no fixture in this tree has a reference; that stays **OPEN**.
