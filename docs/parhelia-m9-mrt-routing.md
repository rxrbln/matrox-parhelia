# M9 multiple render targets: what is decoded, and what routes an output

Two notes in this tree disagreed about MRT.
`docs/parhelia-m9-implementation-backlog.md` recorded it as fully reverse
engineered and needing only `caps->max_render_targets = 1` raised, while
`docs/parhelia-m9-mrt-retirement-re.md` recorded the routing of a colour
export to target 1 as **[OPEN]**.  This note settled the surface side from
the captures, and the fragment side is now MEASURED on the card: four
targets carrying four different expressions render exactly, and the driver
advertises four.

Labels: **CAPTURED** read from a saved command stream, **BIN** read from a
vendor binary, **DERIVED** a join of those, **MEASURED** rendered and checked
on this hardware, **OPEN** not decoded.  Checked by
`tools/m9-mrt-routing-evidence.py`.

## Result

The *surface* side of MRT is exact and needs no guessing.  The *fragment*
side was never captured -- the vendor's MRT fragment programs live in the
`c2` VRAM allocation and no capture in this tree dumps one -- so it was
measured instead, by binding four targets and asking which surface a given
destination reaches.  Target p owns the destination pair `0x2c + 4p` /
`0x2d + 4p`, and a program with more than one colour output may not hold a
live value in the scratch pair `0x11`/`0x13`, which is the colour output's
own staging.  With both, `mrt:4` renders **0 of 76800 pixels differing on
every one of four targets** (MEASURED 2026-09-19).

| piece | state |
|---|---|
| one address/descriptor pair a target at `b0 + 2s` | CAPTURED, one to four attachments |
| render format class a target, raster `89` | CAPTURED, four cases |
| enable bit a target, raster `ef` | CAPTURED, four cases (**new, below**) |
| write mask a target, raster `ab` and `ac` | CAPTURED, four cases (**new, below**) |
| blend a target | BIN, compiled into the target's own program fragment |
| kernel acceptance of `b0/b2/b4/b6` as `COLOR` | already in `m9_index_reloc_kinds` |
| **native destination that names target p** | **MEASURED, section 2: `0x2c + 4p` and `0x2d + 4p`** |
| the scratch pair a multi-output program may not use | MEASURED, section 2c |
| clear of the attachments past the first | implemented on the CPU, section 2d |

## 1. The four capture cases

`captures/m9x-vendor-openbits-20260917/cap-mrt_{1,2,3,4}`, decoded with
`tools/m9-command-decode.py --execute`.  Each case is one submission holding
the clears, the preprocessing pass and the raster draw.  Everything that
moves between the four raster draws, with the stale words named:

| index | mrt_1 | mrt_2 | mrt_3 | mrt_4 |
|---|---|---|---|---|
| `b0/b1` | `120c9000`/`c8210100` | `12111000` | `12159000` | `121a1000` |
| `b2/b3` | — | `120c9000` | `12111000` | `12159000` |
| `b4/b5` | — | — | `120c9000` | `12111000` |
| `b6/b7` | — | — | — | `120c9000` |
| `89` | `fe007ff8` | `fe007fc0` | `fe007e00` | `fe007000` |
| `ab` | `1aebffff` | `ffffffff` | `ffffffff` | `ffffffff` |
| `ac` | `00002b4d` | `00002b72` | `0000ffff` | `ffffffff` |
| `ef` | `00000000` | `00080000` | `00280000` | `00a80000` |
| `88` | `0110fff0` | `0110ffc8` | `0110ff30` | `0110ff20` |
| `8c` | `00500600` | `00500700` | `00500800` | `00500900` |
| `8e - 8d` | `0x30` | `0x38` | `0x40` | `0x48` |

Every other raster word is identical across the four.  `b8`/`ba` carry
leftovers of the preceding pass (`mrt_4` has the literal upload word
`ffff931c` in `b8`), which is what a probe with no depth attachment leaves
behind, not state.

### 1a. Index `ef`, one enable bit a target past the first (CAPTURED)

The four cases differ in `ef` by exactly one bit a target, two apart, from
bit 19:

```text
1 target   0x00000000
2 targets  0x00080000    bit 19
3 targets  0x00280000    bits 19, 21
4 targets  0x00a80000    bits 19, 21, 23
```

Target 0 claims no bit; it is always enabled.  This is the piece a driver
binding `b2` without it will miss, and no earlier note in this tree records
it.  `m9_color_enable` in `m9_draw.c` builds it.

Two independent checks, both CAPTURED.  Every single-target vendor raster
draw in the same capture set leaves all three bits clear: `cap-samplers_4`,
`cap-shadow_d24_lequal` and `cap-attribs_8` write `ef` as `00000000` or the
sequence values `00000200`/`00000400`/`00000600`.  And the **preprocessing**
pass of every one of these submissions, which binds three output streams at
`b0`, `b2` and `b4`, writes `ef = 00288800` -- bits 19 and 21 set, the two
bits that name targets 1 and 2, for the two targets it binds past the first.
The same field, in a pass whose targets are streams rather than colour.

### 1b. Index `89`, one three-bit class a target (CAPTURED)

Bits 14:0 are five three-bit fields.  Field `s` holds the render format
class of the target bound at `b0 + 2s`, and a field no target claims reads
`7`.  The fifth field, bits 14:12, reads `7` in all four cases including the
four-target one, so no target ever claims it.  RGBA8 is class `0`, which is
what the existing single-target word `fe207ff8` already carries.

```text
1 target   0x7ff8   0 7 7 7 7
2 targets  0x7fc0   0 0 7 7 7
3 targets  0x7e00   0 0 0 7 7
4 targets  0x7000   0 0 0 0 7
```

`m9_raster_89` in `m9_draw.c` builds this and keeps bits above 14 from the
word the pass already carries.

### 1c. Indices `ab` and `ac`, one write mask a target (CAPTURED, new)

`ab` bits 15:0 are target 0's sixteen-bit colour write mask and bits 31:16
target 1's; `ac` holds targets 2 and 3 the same way.  The four cases fix
this exactly: `mrt_1` writes a mask only in `ab`'s low half and leaves the
rest stale, `mrt_2` fills `ab` and leaves `ac` stale, `mrt_3` fills `ab` and
`ac`'s low half and clears `ac`'s upper, and `mrt_4` fills both.

This joins the Windows construction: `0x1389a0` translates a target's RGBA
write enables to `0x4444` (R), `0x2222` (G), `0x1111` (B) and `0x8888` (A)
and stores the sixteen-bit result at state `+0x358 + 2*target`
(`0x138b78..0x138bd0`) [BIN].  Four halfwords, two registers.  The driver's
existing `m9_color_mask` already builds one target's halfword;
`m9_color_mask_pair` packs two.

## 2. What routes a colour output to its target (MEASURED)

This was the piece nothing captured, and the note's own `0x2d + p` was
**DERIVED and wrong**.  It is now measured.

### 2a. The probe

Four colour attachments are bound, every write mask is left open
(`M9X_MRT_MASK_ALL=1`), and the shader writes **one** colour output whose
destination `M9X_FS_MRT_DEST0` names.  Whichever of the four surfaces comes
back holding that value names the target the destination feeds, so one run
reads the map outright rather than reporting "every pixel differs".  The
fixture is `mrt:4:1` in `tools/m9-mesa-glsl.c` and `M9_GLSL_MRT_ROUTE=1`
prints, for every surface, how many sampled pixels match each target's own
function.

### 2b. The map

MEASURED on the M9128 on 2026-09-19, `GL_RENDERER` asserted as `Matrox M9`
on every run and a known-good `texture` fixture passing after each:

| destination | surface it reaches | how |
|---|---|---|
| `0x2d` | target 0 | the single-output probe, and every captured program |
| `0x30` | target 1 | the single-output probe |
| `0x34` | target 2 | the single-output probe |
| `0x38` | target 3 | the single-output probe |
| `0x31`, `0x35`, `0x39` | targets 1, 2, 3 | the multiple-output ladder renders them exactly |
| `0x2e`, `0x2f`, `0x32`, `0x33`, `0x36`, `0x37`, `0x3a`..`0x40` | none | the single-output probe finds no surface holding the value |

So **target p owns a destination pair four apart**: `0x2d + 4p` is measured
for all four targets, `0x2c + 4p` follows for p = 1, 2 and 3 from the even
halves that also land, and `0x2c` itself is DERIVED rather than measured --
nothing needs it, because the captured single-target program uses `0x2d` and
the driver emits `0x2d + 4p`.

`0x2d + p` -- this note's earlier DERIVED value -- is refuted outright:
`0x2e` reaches no surface at all.  The virtual registers `0xffec + 2p` and
`0xffed + 2p` the vendor's blend compiler writes [BIN] still step by two a
target; the physical step is four, so two virtual registers map to four
physical destinations a target, not to one.  The competing routing-class
shape (`cls = 4 + p` on one destination) was never needed and is not
measured; `M9X_FS_MRT_CLS` still emits it for anyone who wants to.

### 2c. The scratch pair belongs to the colour output

With the destinations right, four targets still came back wrong, and the
pattern was not in the hardware.  MEASURED, before the fix:

| colour outputs | result |
|---|---|
| any one target | exact |
| any two targets (`{0,1}`, `{0,2}`, `{0,3}`, `{1,2}`) | both exact |
| `{1,2,3}` | all three exact |
| `{0,1,2}` | target 2 wrong |
| `{0,1,3}` | target 1 wrong |
| `{0,1,2,3}` | target 2 wrong |
| `{0,2,3}` | **stream timeout** |
| one `gl_FragColor` broadcast to four targets | all four exact |

The broadcast case is what named the cause: four colour destinations written
from **one** value are exact, so the hardware routes four targets, and only
programs whose targets hold *different* expressions failed.  Those are the
programs whose register allocator reaches for the scratch pair `0x11`/`0x13`.

That pair is the colour output's own staging: `encode_lops` already records
that a producing pass hands its root over in `0x11`/`0x13` *instead of*
writing `0x2d`, and the allocator already refused to hold a value there
across a program's last slot.  A program with several colour outputs writes
several output moves, so a value in the pair is destroyed by the first of
them and every later target reads rubbish.  Reserving the pair for any
program with more than one colour output turns `mrt:4` -- four different
expressions into four targets -- into **0 of 76800 pixels differing on every
one of the four** (MEASURED).  `M9X_FS_MRT_SCRATCH=1` puts the pair back,
which is how the cause was confirmed rather than assumed.

One more shape stops the engine and is worth recording: **four colour
outputs all writing one destination** (`M9X_FS_MRT_STEP=0`) produced a kernel
stream timeout and cost a reboot.  The driver never emits it.

## 2d. What the clear does, and the garbage it used to leave

The quad clear runs the fixed-function program, whose one colour output
reaches target 0 alone, so a `glClear` of a multiple-target framebuffer left
every other attachment holding whatever was in its object.  `m9_clear_locked`
now fills the attachments past the first on the CPU: a solid colour is the
same word in tiled order as in linear order, which is what makes that correct
without the engine.  A single-target framebuffer -- every frame of every
benchmark in this tree -- does none of it.

That alone was not enough, and the difference is a measurement worth keeping.
The clear quad's *job* still bound every attachment the framebuffer held, so
the engine ran a program with one colour output against four enabled targets
and wrote undefined data into the three it had no output for -- racing the
CPU fill that was supposed to have cleared them.  It showed as a few hundred
altered pixels in target 1 and none in targets 2 and 3, which is exactly what
the clear job's own mask words predict: `ab`'s upper half left target 1 open
while `ac`, zero in a key the clear memsets, gated targets 2 and 3 off.  The
count moved between runs, which is the race.  The clear job now binds target
0 alone (`m9_job_key.single_target`), and `mrt:4:5` renders **0 of 76800
differing on all four targets**, the two unwritten ones holding their clear
exactly (MEASURED).

An engine-side clear of the extra targets is still open, and would want a
native program writing the clear colour to every bound destination.

## 2e. Two limits the GL version imposes, not the engine

* **Mixed attachment formats cannot be asked about.**  `EXT_framebuffer_object`
  requires one internal format across the colour attachments, and only
  `ARB_framebuffer_object`, which needs GL 3.0, relaxes it.  The driver
  advertises GL 2.0, so an RGBA8 target beside an RGB10_A2 one is reported
  incomplete by Mesa before the driver sees it (MEASURED); the same
  framebuffer is complete against llvmpipe at GL 4.6.  The surface side is
  ready for it: `89` carries a render format class a target and both packed
  32-bit colour formats take class 0.  What *can* be asked -- whether a
  target of the other packed format works at all -- is `mrt10:2` and
  `mrt10:4`, which bind every attachment as RGB10_A2.  Both pass against
  llvmpipe.  On the card they need the kernel module the GigaColor work
  built: under the baseline module the **single-target** `giga10` fixture
  fails the same way, "de-tiled render target access failed" out of the
  tiled read ioctl, so the multiple-target version would measure the module
  rather than MRT.  It is left for a run under that module.
* **A bound target the shader does not write** is left undefined by GL.  The
  driver gates every byte lane of such a target off, which is stricter than
  GL asks, so it keeps its contents.  MEASURED with `mrt:4:5` (four bound,
  targets 0 and 2 written): all four are exact, the two the shader leaves
  alone holding their clear colour to the pixel.  It took a second fix to get
  there -- see below.

## 2f. What the fixture ladder measures, and one instability worth naming

MEASURED on the M9128 on 2026-09-19, against a library md5-verified on both
sides, `GL_RENDERER` asserted as `Matrox M9`, and the known-good `texture`
fixture passing before and after every run:

| case | result |
|---|---|
| `mrt:2` | both targets, 0 of 76800 pixels differ |
| `mrt:3` | all three, 0 of 76800 |
| `mrt:4` | all four, 0 of 76800 |
| `mrt:4:5` | all four exact: 0 and 2 carry their shader's value, 1 and 3 their clear |
| `mrtcolor:2` | one `gl_FragColor` in both draw buffers, 0 of 76800 each |
| `mrt10:2`, `mrt10:4` | not evaluated: the baseline kernel module fails the single-target `giga10` fixture too, section 2e |

The ladder above was taken holding **both** hardware locks, with the
known-good `texture` fixture passing before and after it.  That matters: the
target carries two lock files, `/tmp/m9-hw.lock` and `/root/tmp/m9-hw.lock`,
and they do not exclude each other, so three agents were running on one card
believing they each held it.  It was seen directly -- another agent's
`glmark2` rendering **on this session's own X server**, halving two
benchmark frame rates -- and it is the explanation for the results this note
originally recorded as an unexplained instability: two runs on which `mrt:2`
or `mrt:3` failed every target and logged a stream timeout, and one on which
the card rendered nothing at all with no timeout and even the control
failed.  Every one of those was retaken under both locks and passed.

Anything measured on this target without both locks held is suspect.  The
destination map in section 2b, the scratch-pair ladder in section 2c and the
fixture ladder here were all retaken under both locks with a control after;
the two negative controls -- `M9X_FS_MRT_STEP=1`, the DERIVED `0x2d + p`, and
`M9X_FS_MRT_SCRATCH=1`, the pair put back -- reproduce their exact original
signatures there, `mrt:2` target 1 wrong for the first and `mrt:4` target 2
wrong for the second.

## 3. What the driver now does

`m9_draw.c` binds up to four colour attachments: `job->color` at `b0/b1` and
`job->color_extra[s - 1]` at `b0 + 2s` with its own descriptor and its own
`COLOR` relocation, packs `job->color_class[]` into raster `89` through
`m9_raster_89`, emits the enable bits into `ef` through `m9_color_enable`,
and emits `job->color_mask` and `job->color_mask_hi` into `ab` and `ac`.
The kernel's `m9_index_reloc_kinds` already accepts `COLOR` at `b0`, `b2`,
`b4` and `b6`, so no kernel change is needed.  `test_mrt_job` in
`m9_batch_test.c` pins all of it offline, and the single-target job stays
byte for byte the captured stream.

`m9_fs_compile.c` compiles up to four colour outputs.  Every target's root
shares one expression graph, so a value two targets read is evaluated once;
the roots are computed into registers and each is then moved to its own
target's destination, which costs two slots a target over the single-target
path.  That path is untouched -- one root still writes the output as it is
computed, as the captured programs do -- and the corpus result is unchanged
at 158 supported, 59 exact, 1462 slots.

`m9_context.c` binds what the framebuffer holds, keyed on the extent all
targets share, and gates a bound target the shader does not write.
`gl_FragColor` is broadcast into every bound draw buffer by
`m9_fs_lower_fragcolor` -- `nir_lower_fragcolor` rewrites `store_deref` and
st hands this backend IO intrinsics, so it finds nothing -- and the variant
cache is keyed on the target count that lowering depends on.

What a multiple-target shader is still **refused** for, each because the one
colour output of the captured programs is where that feature lives:

* a discard or an alpha test;
* blending;
* not fitting one raster pass -- the splitter's cut machinery follows the
  cone of a single output, so a multiple-target shader that needs more than
  one pass is refused rather than split along the last of them.

`M9X_MAX_RENDER_TARGETS=1` holds the cap back to one, which separates an MRT
fault from anything else without a rebuild.

## 4. The backlog line that was wrong

`docs/parhelia-m9-implementation-backlog.md` listed MRT as needing only the
cap raised.  What that line described -- descriptors, relocations, masks and
blend, verified live on 2026-09-17 -- was all true of the *vendor* stack and
of the surface side of ours.  It omitted that the vendor's shader-side
routing was never captured and that our own fragment backend had no second
output at all.  Both are now done and measured, and the line is retired
there.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
