# What the M9128 does with a split fragment shader

`docs/parhelia-m9-fragment-pass-splitting.md` and
`docs/parhelia-m9-fragment-pass-routes.md` decode the vendor's multi-pass
fragment schedule from saved captures.  This note records what happened when
that schedule was emitted at the M9128 for the first time, through the m9x
Gallium driver's own pass splitter.

Labels: **CAPTURED** read from a saved vendor stream, **MEASURED** read back
from the hardware, **OFFLINE** produced by the driver's own compiler without a
device, **DERIVED** a join, **OPEN** not decided.

## 1. The result: the record binds, once the pass says where it sits

**A raster pass may bind the inter-pass record as its render target.  What the
engine needs beyond the record descriptor is index `ef` bits 10:9, which place
the pass inside its multi-pass sequence** (MEASURED).  With that field emitted
the producing pass runs with the captured producer descriptor `b1 = ee190004`,
the captured record `89 = fe007ffc`, the record coverage words and the routes,
a two-pass schedule runs end to end without stopping the engine, and fixture
`tex5` -- five texture fetches the driver's own splitter cuts into two raster
passes -- renders pixel-exact.

The field is uniform across both independent capture sets (CAPTURED, the
raster range only; the preprocessing range carries its own unrelated `ef`):

| schedule | `ef` per raster pass |
|---|---|
| every single-pass draw (`fs-color`, `fs-add`, `fs-abs`) | `00000000` |
| all seven two-pass cases | `00000200` then `00000600` |
| the five-sampler schedule | `00000200`, `00000400`, `00000400`, `00000400`, `00000600` |

so bits 10:9 hold the pass's place: **0 the only pass, 1 the first of several,
2 a middle one, 3 the last** (DERIVED, and the only reading that fits both a
two-pass and a five-pass schedule).  m9x emitted zero there, which told the
engine every pass was a complete draw on its own.

This **refutes** the earlier conclusion of this note, that the record
descriptor's `0x77` format field was what the engine would not take as a
render target.  Two markers tell the engine a pass writes a record -- the
`0x77` descriptor at `b1`, and the render format class `4` that index `89`
carries for target 0 -- and while `ef` left the pass unplaced, **either one
alone stopped the engine** (MEASURED, section 2).  Neither is a blocker; both
are consistent descriptions of a mode the pass had not been placed into.

Index `89` is not a bitfield of enables: `docs/parhelia-m9-mrt-retirement-re.md`
records it as one three-bit render format class per bound target at bit
`3*s`.  The record's class is `4` and an ordinary colour target's is `0`, so
the `fe007ffc`/`fe007ff8` difference the captures show is that class, not a
record-enable bit.  Bit 21, where m9x's own colour value `fe207ff8` differs
from the vendor's `fe007ff8`, plays no part: `fe207ffc` stops the engine
exactly as `fe007ffc` did (MEASURED).

## 2. The ladder

All rows: M9128 at PCI 01:00.0, kernel 7.2.5-t2 at `f930663`, the m9x Gallium
driver with its pass splitter, fixture `tools/m9-mesa-glsl` case `texaff` at
320x240 with one draw, forced into two raster passes with
`M9X_FS_FORCE_SPLIT=2` so that each producing pass binds one texture -- the
shape of the captured five-sampler schedule.  **Every row was measured on a
freshly booted target whose engine had first been proved alive by
`m9-mesa-glsl texture`**, for the reason section 3 gives.

With `ef` left at zero, as the driver emitted it until now:

| configuration | outcome |
|---|---|
| one raster pass through the multi-pass emitter (`M9X_PASS_LOOP=1`) | pixel-exact PASS |
| producing pass alone, colour target at `b0` (`M9X_SPLIT_STAGE=1`) | no stall |
| record object at `b0`, colour descriptor, colour-pass `89`/`a9`/`ae` (stage 7) | **no stall** |
| record object at `b0`, colour descriptor, record `89` = `fe007ffc` (stage 4) | stall |
| the same, with the plain coverage words (`M9X_SPLIT_COVERAGE=0`) | stall |
| the same, with bit 21 kept set (`M9X_RECORD_89=0xfe207ffc`) | stall |
| record descriptor `b1 = ee190004`, colour class in `89` (`M9X_RECORD_89=0xfe207ff8`) | stall |
| record descriptor `b1 = ee190004`, record `89`, the real schedule (stage 3) | stall |

and with `ef` bits 10:9 emitted:

| configuration | outcome |
|---|---|
| producing pass alone, the real schedule (stage 3, `M9X_SPLIT_PASSES=1`) | **no stall** |
| both passes, `texaff` and `vary2tex` forced into two passes | **no stall**, values wrong |
| the whole fragment fixture suite with the schedule off | 97 PASS, 0 FAIL |

Two rows retire earlier readings of this note:

* **the placement hypothesis is ruled out.**  Stage 7 binds the record object
  at `b0` and changes nothing else, and it renders; where the allocator put
  the object relative to the colour target does not matter.  The captures say
  the same: the vendor's record at `0x10ec1000` is the context's *primary
  temporary resource*, 4 MiB from `TempBufferSize` (`docs/parhelia-m9-be-backing-re.md`),
  with the `be` resource at `0x112c1000` exactly 4 MiB above it and the first
  transient at `0x113c1000` (CAPTURED).  Its extent crosses the 16 MiB
  boundary at `0x11000000`, so the object does not sit inside one 16 MiB
  window at all and no window rule of the 0528's shape can be read from it.
* **the old "record object at `b0` with an ordinary colour descriptor runs"
  row was stale.**  It predates the commit that started emitting the captured
  record `89`; re-measured at HEAD that configuration carries class `4` and
  stalls.  The isolation it supported -- that `b1` alone decided -- was
  therefore not single-variable.

## 3. Method, and what a lost measurement looks like

**A stream timeout leaves the device in a state a module reload does not
clear.**  The reload succeeds, but `m9x_requester` finds `0x400`/`0x404`
mismatched, refuses bus mastering with `-EBUSY`, and every later job returns
"Device or resource busy; context lost" without reaching the engine.  A run in
that state renders nothing and logs no timeout, which reads exactly like a
configuration that runs cleanly.  Reboot the target between a stall and the
next measurement, and open every session with a known-good fixture.

The `e6`/`44` inter-draw transition, which this note once put first, is
decoded (`docs/parhelia-m9-fragment-pass-splitting.md` section 6.1), accepted
by `m9_parse_commands` and emitted around every raster draw of a split job.
It is neither the cause nor the cure: a pass loop emitting one pass renders
pixel-exact with it, and a producing pass stopped the engine with it while
`ef` was zero (MEASURED).

## 3a. What the hand-off delivers

A shader that genuinely needs two passes hands its values over correctly:
fixture `tex5`, five texture fetches cut into two raster passes by the
driver's own splitter, renders **0 of 76800 pixels wrong** (MEASURED).  The
schedule is therefore on by default and `tex5` is a listed fixture.

`M9X_FS_FORCE_SPLIT=2` is a different thing and its results do not carry:
forcing a one-fetch shader such as `texaff` or `vary2tex` into two passes
completes without stopping the engine but draws a wrong image, because the cut
that path takes is arbitrary rather than one the partitioner would choose.
Use it to exercise the emitter, not to judge the hand-off.

### 3b. The record's granularity, measured

**The record is a flat array the engine indexes by its own fragment counter
inside a raster range, one slot a shaded fragment** (MEASURED).  It is not
per 2x2 quad and not per shaded sample, and it does not follow the target's
area.

`tools/m9-record-probe.c` renders a five-fetch shader -- which the driver
splits in two -- over a rectangle of a 320x240 target, sampling a 1:1 NEAREST
texture whose texel `(x, y)` carries its own coordinates, and dumps the record
through `M9X_RECORD_DUMP`:

| covered rectangle | fragments | highest record index written |
|---|---:|---:|
| 320x240, the whole target | 76800 | 77431 |
| 160x120, a quarter of it | 19200 | 19495 |

Both rows are the same 320x240 target and the same 81920-slot record, so a
screen-position index is ruled out: a quarter-coverage draw indexed by
`y * width + x` would have reached 38239 and would have left the record
sparse, and instead it filled a dense run ending at 19495.  The count runs
about 1.5% past the covered pixels, which is the quads a shared triangle edge
shades twice.

So the object costs `stride * (fragments a pass shades)` and there is no
granularity to exploit: rounding to whole quads with a measured edge margin
replaces the 64-pixel alignment the driver guessed at, which is about 5%
smaller and no longer accidental.

**What actually refuses the record is the driver's VRAM pool, not its size**
(MEASURED).  `effect2d`'s 3x3 kernel asks for **35 MiB at 1920x1200 with a
stride of 4** and is refused on a card with `0x40000000` bytes -- 1 GiB -- of
VRAM, because `drmm_vram_helper_init` gives TTM only
`min(BAR0, M9_ADDRESS_CPU_BYTES)` = **224 MiB**, the identity-mapped part of
BAR0, which the scanout surfaces, framebuffers, textures and render targets
already occupy.  Sharing one record across the screen instead of one a
context does not help, because a single scene cannot get its first one.
Reaching the rest of the card means allocating VRAM through the surface
window at `[224M, 256M)` rather than the identity view, which is a
VRAM-addressing change and not a record change.  The producers of
every glmark2 shader the splitter cuts hand over four components, so the
four-dword slot is not padding either -- a four-pass shader at 1920x1200
genuinely needs about 106 MiB, and where VRAM cannot hold it the draws are
still skipped with a named reason.

### 3bb. A split job must cover the whole of its render target

**A multi-pass job whose geometry leaves part of its render target uncovered
stops the engine** (MEASURED, 2026-09-18, M9128).  This is the fault that took
glmark2's `desktop` scene down, and none of the properties the earlier rounds
named is the discriminator.

What the scene actually issues, traced from the scene itself (CAPTURED,
`M9X_SPLIT_TRACE`):

```text
m9x splitjob 10: 420x420   passes 2 draws 1 records 4 extent 0,0..420,420
m9x splitjob 11: 1920x1200 passes 2 draws 1 records 4 extent 1230,390..1650,810
```

The 420x420 job covers its target; the 1920x1200 job covers a 420x420
rectangle of it, because glmark2's `RenderWindowBlur::render_to` blurs the
window *back out* over the screen at the window's own position.  Dropping only
the large-target jobs renders the scene at 90 fps over 7208 split jobs
(MEASURED, `M9X_SPLIT_SKIP_ABOVE=200000`), so the partial one is the whole of
the fault.

Corrections to what was recorded before:

* The pass count is **not** the variable.  `desktop:effect=shadow` renders at
  124 fps and `blur-radius=1` at 96 fps because neither splits; `blur-radius=2`
  splits into **two** passes (record stride 4) and stops the engine, as
  radius 5's four passes do (MEASURED).
* The target size is **not** a threshold.  A partial-coverage job stops the
  engine at 320x240 as well; a smaller target only makes it less likely per
  draw (MEASURED: `splitdesk:3:4:5:320x240:1` rendered once and stopped the
  engine on the repeat).
* The record is **not** involved.  `M9X_SPLIT_STAGE=1` leaves the record
  unbound and the routes idle and the fixture stops the engine just the same
  (MEASURED).

Everything else measured against the fixture and refuted: the sequence words
cleared (`M9X_SPLIT_SEQ=0`), the vendor transition dropped (`M9X_TRANSITION=0`,
which renders once and stops the engine on the repeat), index 30 restated,
one submission for every pass (`M9X_SPLIT_SUBMIT=0`), a record of its own per
job (`M9X_RECORD_FRESH=1`), a depth pair of its own (`M9X_TARGET_FRESH=1`),
the engine-only placement (`M9X_ENGINE_VRAM=0`), the back-end object cleared
before every job (`M9X_BE_CLEAR=1`, after fixing it to clear the object the
job binds rather than the context's own), one back-end object for the context
(`M9X_BE_PER_TARGET=0`), the depth schedule (`M9X_SPLIT_DEPTH=0`), the
coverage words (`M9X_SPLIT_COVERAGE=0`), the clip window rounded to the
allocation (`M9X_CLIP_ROUND=1`) and the clip window narrowed to the geometry
in both axes (`M9X_SPLIT_CLIP`).

Two controls bound it from the other side.  A **single-pass** shader over the
same rectangle renders 64 draws over, and the same partial geometry drawn as
two triangles rather than a strip stops the engine exactly as the strip does
(MEASURED), so neither partial coverage nor the topology is the fault on its
own: it is the multi-pass schedule over part of a target.  Only
`M9X_SPLIT_PASSES=1`, which emits the producing pass and drops the consumer,
renders.

It is a **back-end** hang.  The fetch pointer read back from
`M9_REG_COMMAND_BASE` reaches the end of the submission -- dword 171 of 171 --
and the writeback fence never retires, so the stream the parser sees is
well formed and the engine hung executing it.

The count a context survives is not sharp: 1, 2, 8, 10, 12, 16 and 20 partial
draws at 1920x1200 rendered, 24 and 32 stopped the engine, and 32 draws of a
smaller rectangle stopped it as well.  A driver cannot rely on a count, so the
driver refuses the schedule outright; `M9X_SPLIT_PARTIAL=1` submits it again
for the probe.

The fix that lets these draws render is to give the job a target it does
cover: render it into a scratch surface the size of the pixels the geometry
covers and copy that into place with the engine copy, which is a whole-target
draw on both halves.  That is implemented and measured in 3bc.

### 3bc. The scratch surface a partial draw is rendered on

A multi-pass job whose geometry covers part of its target is held until the
driver is outside the draw module, and then rendered into a **scratch colour
surface of exactly the pixels it covers**, which it covers whole, and copied
into place (`m9_deferred_run`).  The destination seeds the scratch first, so
blending, a partial colour mask and geometry that does not fill its own box
all read and keep what the destination held, and the copy back writes only the
box.  `M9X_SPLIT_SCRATCH=0` refuses the draw instead, which is the behaviour
3bb describes, and `M9X_SPLIT_PARTIAL=1` still submits it as it stands.

The box is the pixel centres the geometry's screen extent encloses --
`ceil(x - 0.5)` at both ends -- and not the rounded-out bounding box.  That is
what makes the geometry cover the scratch surface *exactly* however the draw
sits inside a pixel, which a rounded-out box does not: the scene's own blur
quad at x 808.4 leaves the column at 808 half covered, which is the same
partial coverage again.  The origin is whole, so the translated records keep
the target's own pixel grid and the copy back is a plain 1:1 rectangle.  A
record carries x and y multiplied by w with 1/w beside them and counts rows
from the bottom, so the translation is `p[0] -= x_box / p[3]` and
`p[1] += (H_scratch + y_box - H_target) / p[3]`.

Two hardware requirements were measured on the way, both of them back-end
state rather than coverage:

* **A multi-pass job needs the back-end object it binds cleared when draws
  into another target have run since that object was last used.**  The scratch
  path walks the engine between two targets on every draw, and without the
  clear it stops the engine at the **ninth** such draw (MEASURED:
  `splitdesk:12:1:5:1920x1200:5:420x420` stalls, `M9X_SPLIT_PARTIAL=1` on the
  same case renders 12, `M9X_BE_CLEAR=1` and `=2` both render 32).  With
  `async_submit=0` the failure is reported at the submission *after* the
  scratch job and the kernel's fetch pointer reaches that job's fence target
  with the writeback one step short, so it is the same back-end hang as 3bb.
  `M9X_BE_CLEAR=1` for *every* job is not a usable form of this: the
  `texture` fixture itself stops the engine under it (MEASURED).
* **The pair a scratch surface binds has to be its own.**  Back-end objects
  were kept per target *shape* (3bb), which is not enough here: `desktop`'s
  window target is exactly the size of the box its blur covers, so the scene's
  own target and the scratch surface shared the engine state and the scene
  stopped the engine in its first frames even with the clear.  A scratch
  surface now keys its own depth, HZ and back-end trio.

**A scratch surface only helps a draw that fills the box it spans.**  The
surface is the size of that box, so the geometry covers it whole only when it
fills it: a rectangle does, a mesh does not, and handing a mesh a scratch
surface of its extent leaves that surface partly uncovered in exactly the way
this section exists to avoid.  The driver checks it rather than assuming --
every record has to sit on a corner of its own extent with all four corners
drawn, which covers a strip, a fan, a quad and a rectangle cut into two
triangles alike -- and asks it **before** the depth attachment, because it is
the property that decides whether a scratch surface could serve the draw at
all.

That ordering is what let the refusals be counted, and the count settles a
question that looked like the next piece of work: **no draw anywhere in
glmark2's default list of thirty-three is refused for the depth attachment**
(MEASURED, `M9X_SKIP_REPORT=1` over the whole run).  Every multi-pass draw the
driver refuses is refused for geometry -- `refract` hands over 156 records a
draw and `terrain` 576, both meshes over part of their target
(`M9X_SPLIT_TRACE`) -- or for the fragment backend's own limits.  Carrying the
destination's depth box into the scratch surface would therefore restore
nothing.  **`refract` was then closed by neither route**: its extent test was
computing the box wrongly, multiplying by w where a record requires a divide,
so the bunny's extent landed outside the target and the draw was refused for
covering none of it.  With the divide it is conformant.

`terrain` remains, and for it the statement holds: its mesh under a seven-pass
shader can cover no target of any size, and what would close it is a fragment
backend that compiles that shader into **one** pass, where the coverage
requirement does not apply.  Padding does not rescue it - MEASURED, it stalls
under `M9X_SPLIT_PAD=1` regardless.

What the path refuses rather than submitting, each with a reason of its own
(`m9_split_defer`): a draw whose geometry does not fill the box it spans, one
that tests or writes depth, which the scratch surface does not carry, one
whose geometry a program of its own preprocesses, one carrying a scissor of
its own, and one with a vertex behind the eye.

MEASURED on the M9128 at 1920x1200, glmark2's own default list of
thirty-three benchmarks, one process each, no stream timeout:
`desktop:windows=4:effect=blur:blur-radius=5:passes=1:separable=true` draws
its blur at **38 fps** where it used to stop the engine and take the seventeen
benchmarks after it with it, and 29 of the 33 are at or above 60 fps.  The
frame is the engine's, not the driver's: 26.77 ms/frame of which submit 23.13
and ioctl 22.84, against draw 2.73, emit 2.66 and transfer 0.00
(`M9X_PROFILE`), over 28 jobs and 24 draws a frame with nothing refused or
skipped.

### 3bd. What the back end counts as covering a target

**The geometry a job rasterises, not the pixels it writes** (MEASURED,
2026-09-18, M9128).  Four fixtures against `conv3sub`, which stops the engine:

| case | what it draws | result |
|---|---|---|
| `conv3pad` | the whole target, writes scissored to the sub-rectangle | renders |
| `conv3padz` | the sub-rectangle in front, the rest behind the cleared depth | renders |
| `conv3frame` | the sub-rectangle and a **one-pixel frame** at the target's edges | renders, image exact |
| `conv3subpad` | the same padded to the whole of the rest of the target | renders, image exact |
| `conv3hole` | the target covered except a rectangle in the middle | renders |

So padding that is scissored out counts, padding the depth test rejects
counts, and a hole in the middle is not counted against the job at all -- it is
reaching the target's **extent** that matters, and a frame a pixel wide is
enough.  `conv3hole` renders fourteen of its 76800 pixels with the blue
channel about one tap high along a shared triangle edge, which is not the
coverage question and is the reason it is dispatchable by name rather than
part of the suite.

`M9X_SPLIT_PAD=1` implements the consequence: the draw is padded with four
rectangles a pixel wide at the target's edges, two triangles of each winding
apiece so no cull state removes them, its topology expanded into one indexed
triangle list, and the clip window narrowed to the pixels the geometry covers.
Twenty-four records, against a scratch surface and two copies.

**It is off by default**, but the reason it could not be trusted has been
found and fixed.  It needs the back-end object cleared ahead of *every*
multi-pass job rather than only the scratch path's own (`M9X_SPLIT_BE=3`):
without that, four windows of `desktop`'s blur stop the engine
(`splitdesk:8:4:11:1920x1200:1:420x420` stalls, and renders 32 frames with
it).  With both on, the fixture suite used to stop the engine at `conv3rep`.

**What `conv3rep` hit was the clear, not the padding** -- see 3bf.  With the
back-end object swapped rather than zeroed under a job in flight, MEASURED
2026-09-19 with `M9X_SPLIT_PAD=1 M9X_SPLIT_BE=3`: `conv3`, `conv3sub`,
`conv3subw`, `conv3small`, `conv3strip`, `conv3mag`, `conv3rep`, `conv3mix`,
`convrtt`, `convrttin`, `convrtt2`, `conv3notest`, `conv3nowrite` and
`texture` all pass pixel-exact with no stream timeout, and `desktop` runs its
whole benchmark on the padded path at **37 fps over 16 jobs a frame** where
the scratch path takes 28 -- no draw refused, no draw skipped, no timeout.
So padding is now a working path; it is not yet the default because it is not
faster (the frame is the engine's either way) and the scratch path is the one
the thirty-three benchmarks were measured on.

`terrain` under `M9X_SPLIT_PRESSURE=1` also **no longer stops the engine**
with padding and the swap: MEASURED 2026-09-19, 1055 split draws a frame are
padded and drawn, and what is left is the fragment backend refusing 14 draws
a frame for five samplers against four units and 2 for geometry that does not
fill the box it spans.  So padding *does* carry a seven-pass mesh schedule;
what blocks `terrain` is the compiler, not the coverage rule.

### 3bf. A back-end object cleared under a job in flight

**A job posted asynchronously retires only at the next submission**
(CAPTURED from the kernel: `m9_submit_execute` opens with
`m9x_transport_drain_locked`, and the comment on
`m9x_submit_user_stream_locked` says why -- "the command image and the engine
state are shared, so a job posted earlier has to retire before this one is
built").  The Mesa driver's clear of the back-end object is a plain CPU
`memset` of a VRAM mapping, so it overwrites state the engine may still be
reading.  The scratch path makes that certain rather than likely: its seed
copy is a draw into the very surface whose object is cleared next.

That is what "the boundary is not sharp" always meant -- 1, 2, 8, 10, 12, 16
and 20 partial draws rendering and 24 and 32 stopping the engine is a race,
not a count.

**There is no drain to take.**  `m9x_submit_noop` -- the only fence a client
can post -- writes the shared command image *without* draining first, so
asking for a fence to order the clear corrupts the job in flight instead of
waiting for it.  MEASURED: a build that posted a no-op fence before each
clear took the whole machine off the network mid-`desktop`.

So a target shape keeps **two** back-end objects and a clear swaps to the
spare and zeroes that, which the engine cannot be reading because the job in
flight bound the other one.  The spare is allocated the first time a shape
wants a clear.  MEASURED, one binary, `M9X_SPLIT_BE=3`, `M9X_BE_SWAP` the
only variable: with the swap off, `conv3rep` fails 76800 of 76800 pixels on
its first run and takes the engine with it; with it on, `conv3rep` passes
three times over and `conv3`, `conv3mix`, `conv3sub` and `texture` pass
behind it, no timeout.

### 3be. A record's screen position is the quotient, not the product

A record carries x and y **multiplied by w** with w itself in the fourth slot
(`m9_render_record`), so reading a screen position back divides.  The
partial-coverage test multiplied, which is the same number only where w is 1 --
every 2-D draw, which was every fixture it had been written against.  A
projecting draw came out w squared too large: `refract` handed over a bunny at
`6694,1583..10158,5042` on a 1920x1200 target, its extent fell outside the
target entirely, and the draw was refused as covering none of it.  The scene
rendered a frame short of one of its two bunny passes and reported 40 fps for
it.

With the divide, `refract` draws both passes: skipped draws go from one a
frame to none and its CPU varyings from 0.4 KiB a frame to 101.6 (MEASURED,
`M9X_PROFILE`).  `conv3subw` is the fixture that fails without it -- `conv3sub`
at w = 2, the same pixels and the same image, and a record whose fourth slot
is not 1.

### 3c. A split job carries one raster draw

**Two restated raster draws in one split job stop the engine** (MEASURED,
reproduced twice from a freshly booted target with
`m9-record-probe 320 240 160 120 2`, which draws the same rectangle twice;
the single-draw run of the same probe renders and dumps its record).  The
driver now closes a split job after one draw, which also bounds the record:
its fragment counter then covers a single draw rather than a whole job, so
the size above is an upper bound rather than a hope.  `M9X_SPLIT_DRAWS`
raises the limit for re-measurement.

### 3d. A split job's records are the ceiling over the pass count

A split job preprocesses **one copy of the geometry per raster pass**, from one
arena and through one index run per copy, so the records it can carry are
`m9_draw_max_source_vertices() / passes` less the padding a partial draw needs
- 2856 of 11520 at four passes, 1620 at seven.  Two consequences:

* A mesh under a split shader is cut into many single-draw jobs.  refract's
  208998 corners at four passes were 103 jobs a frame as the draw module's own
  blocks divided them at the old 11520 ceiling, and 70 at the 23040 the driver
  now defaults to (MEASURED).  That job count is **not** the scene's frame
  time: what a job cost was the clear of the engine's back-end object it took,
  a megabyte of VRAM written by the CPU, and with the clear sized by the extent
  the object uses the same 70 jobs cost 16 ms a frame rather than 82
  (`docs/parhelia-m9-glmark2-status.md`, the fifth standing).
* `records * passes` also has to fit the **16-bit** index space, because each
  copy's indices are the first copy's plus `i * records`.  So the ceiling
  cannot usefully exceed 65536 however much memory there is, and 16384 records
  a job is the most a four-pass shader can ever hold.  **VRAM stops it first on
  this board**: the transient stream views are `20 * ceiling * 16` bytes, so
  65532 asks for 21 MiB of them and a 1920x1200 run then reports "cannot
  allocate target depth", draws nothing and times the stream out (MEASURED).
  23040, which is 7.4 MiB of views, is where the gain is taken: 130 jobs and
  210.8 ms a frame at 11520 against 70 and 166.5 here, 55 and 165.5 at 46080.
* The **CPU-gathered varying views need no copy a pass.**  Pass `i` reads at
  the first copy's index plus `i * records`, so the gather is written once at
  the tail of the view and each pass's stream relocation is pulled back by its
  own copy's offset; the pull-back is never negative because `passes * records`
  is the view's own ceiling.

Producers that sized their runs by the whole ceiling had them refused here and
dropped the geometry silently; `m9_job_max_vertices()` is the single statement
of the rule (`98be636`).

## 4. What the driver does today

The compiler-side splitter is implemented and gated off.  `m9_fs_compile_passes`
partitions a shader the engine cannot run in one pass by cutting it at a
single value: the producing pass computes that value and hands it on, the
consuming pass reads it as an extra texel, and the cut is chosen to absorb as
many fetches as a pass has units.  The partition is checked by trial
compilation, so a cut that the backend cannot encode is rejected and the next
one tried.

Pass counts it produces for the glmark2 shaders (OFFLINE, the shaders rebuilt
from `src/scene-effect-2d.cpp` and `src/scene-desktop.cpp` and compiled with
`m9-fs-compile-test --glsl`):

| shader | fetches | passes |
|---|---:|---:|
| `effect2d` 3x3 edge kernel | 5 | 2 |
| `effect2d` 5x3 kernel | 15 | 5 |
| `desktop` separable blur, radius 5 | 11 | 4 |

which is the `4 + 3(P-1)` the pass-splitting note predicts.

With the relocation budget bounded by the command image rather than by a
constant, the two scenes it refused now render (MEASURED, 1920x1200, no
stream timeout):

| scene | passes | fps | outcome |
|---|---:|---:|---|
| `effect2d` default | 2 | 222 | renders |
| `desktop` separable blur | 4 | 35 | renders, where it was refused before |

`desktop`'s run still logs one "cannot allocate the inter-pass record", which
section 3b explains: at four passes and a large target the object is over
100 MiB and the honest answer is that the hardware's record costs that.

A glmark2 sweep at 1920x1200 with the schedule off reports 33 scenes, 25 of
them at or above 60 fps (MEASURED).  `terrain`, `shadow` and `refract` are
refused for an unrelated reason, a texture coordinate that is not a varying.

With the sequence field emitted, the six scenes this note listed as blocked
sweep at 1920x1200 as follows, with **no stream timeout anywhere**
(MEASURED):

| scene | passes | fps | outcome |
|---|---:|---:|---|
| `effect2d` default | 2 | 222 | draws, where it was refused before |
| `effect2d` 3x3 Laplacian | 2 | 346 | draws, but see the record size below |
| `effect2d` 5x3 | 5 | -- | refused: the relocation budget |
| `desktop` separable blur | 4 | -- | refused: the relocation budget |
| `terrain`, `shadow`, `refract` | -- | -- | refused: a texture coordinate that is not a varying |

Two limits stand between that and the rest, and neither is the record:

* **the relocation budget of one submission.**  A shader beyond three raster
  passes needs more relocation sites than `M9_MAX_RELOCS` carries, so the
  variant is refused at creation.  That is what now stops `desktop` and
  `effect2d`'s 5x3 kernel.
* **the record's size.**  The driver sizes it for one record per covered
  pixel, which is 37 MiB for a two-pass shader at 1920x1200, and a second
  context asking for one while the first still holds its own failed to
  allocate during the sweep ("cannot allocate the inter-pass record"), so the
  Laplacian scene's draws were skipped for part of its run and its 346 fps is
  not a clean number.  The granularity that would shrink it -- one record per
  fragment, per 2x2 quad or per shaded sample -- is **OPEN**.

The schedule is on by default, `M9X_SPLIT=0` refuses it instead, and the
fragment fixture suite holds at 97 PASS, 0 FAIL with either setting; `tex5`
joins the listed cases and raises the bar to 98 PASS, 0 FAIL (MEASURED).  Nothing in
the Gallium caps advertises the feature: `fs->max_texture_samplers` was already
8 rather than the four-unit binding limit, so no cap claimed or claims that a
five-fetch shader renders.

## 6. A coordinate the fragment stage computes

The engine samples before the program runs, so a coordinate computed in the
fragment stage cannot feed a fetch in the same pass -- which is what refused
`terrain`, `shadow` and `refract` with "texture coordinate is not a varying".
It can feed one in the **next** pass, through a coordinate route, and
`docs/parhelia-m9-fragment-pass-routes.md` section 4 decodes that from the
captures.  **m9x emits one by default and the fixtures render pixel-exact**;
`M9X_COORD_ROUTE=0` refuses a routed coordinate instead.

### 6a. What the route needed, and what was wrong

| step | outcome |
|---|---|
| the splitter cuts at the computed coordinate | `shadow.frag` becomes two passes, `light-refract.frag` four (OFFLINE) |
| the consuming unit keeps the interpolated selector word `00040118` | **stops the engine** (MEASURED) |
| the routed selector word `0004111c` | **no stall**, the schedule runs (MEASURED) |
| the producing pass wrote the coordinate | correct values, **wrong storage**: `M9X_RECORD_DUMP` held `18431665` at slot 0, two half-precision values a dword |
| the consuming unit sampled there | texel `(0, 0)` for **every** pixel (MEASURED, 76794 of 76800 differing; the six that matched are the pixels whose own answer is texel `(0, 0)`) |
| the producer's root written to `0x11`/`0x13` instead of `0x2d` | the record holds 32-bit floats and the fixture renders **0 of 76800 pixels wrong** (MEASURED) |

**The fault was in the driver's own encoder, not in the route.**  A pass that
hands a value to the next one writes it through registers `0x11` and `0x13`,
and `encode_lops` computes that destination -- but the two move classes
(`L_MOV_OUT`, `L_MOVC_OUT`) ignored it and wrote the literal colour output
`0x2d`.  Any producing pass whose root reaches the output through a plain
move therefore went down the colour path, which packs a fragment into the
render target's pixel format: the record came back as four dwords a 2x2 quad,
each holding one pixel's `(s, t)` as two half-precision values.  `tex5`'s
producer, whose root arrives through arithmetic, wrote the same record as
four 32-bit floats a fragment and rendered pixel-exact all along, which is
why one route worked and the other did not.

Read as a float, `0x18431665` is 1.6e-24, so both components arrived as zero
and the unit sampled texel `(0, 0)` everywhere -- a whole-image failure, not
an off-by-one lane.

### 6b. The record's layout, closed

**A producer's record slot is four dwords of 32-bit float a fragment**
(MEASURED, `M9X_RECORD_DUMP` after `coordroute`: `3acca3d0 3b993d6d ...` =
`0.001561, 0.004677, ...`, and after `tex5`).  Component 0 is at the
descriptor's own offset dword and the route word's `c1` names the dword the
second component sits in -- exactly the layout the driver already emitted, so
**nothing about the route word, the selector word or the descriptor changed**.
The two readings retired earlier stay retired; neither was the fault.

### 6c. Fixtures

`coordroute` samples at the product of two varyings, which no affine fold can
reach.  `coordroutev` adds an interpolated varying to the consuming pass, so
a unit steered by the record sits beside an ordinary attribute declaration --
the shape every glmark2 scene that needs a route has, and the one `xv-uyvy`
captures.  Both render **0 of 76800 pixels wrong** and both are listed.

Sixteen further cases cover a split shader against a depth attachment, the
state every windowed scene draws under; section 6f is what they found.
`convoff` takes five fetches at one interpolated coordinate plus four
constant texel offsets and `conv3` sums the same five taps with per-tap
weights, which is exactly the shape glmark2's `effect2d` compiles to;
`conv3rep` repeats the split job sixteen times and `conv3mix` puts an
ordinary single-pass draw between the repeats.  `conv3notest` and
`conv3nowrite` separate the attachment from the test and the write, and
`conv3zover` and `conv3zunder` check that the test still decides which of an
overlapping split draw and plain draw survives.  Each is listed twice, plain
and with the `d` suffix that binds the attachment.

### 6d. What blocked the scenes: depth, not the route

With the coordinate no longer refused, `shadow`, `refract` and `terrain` all
built their multi-pass schedule, reached the engine, and **the engine
stopped** (MEASURED, 640x480 and 1920x1200, kernel `m9x_drm-bocap` so the
record allocates): `m9x: two-pass job failed (Input/output error); context
lost`, with one `M9 kernel stream timeout` in the kernel log.  The submission
that reports the error is the one *after* the stalled job, because submission
is asynchronous.

Two earlier readings of `effect2d` were taken to contradict each other.  They
do not: they are different scenes.  Measured on one binary, one boot each:

| case | 640x480 | 1920x1200 |
| --- | --- | --- |
| `effect2d` default (identity kernel, one tap) | 1262 fps, clean | 220 fps, clean |
| `effect2d:kernel=0,1,0;1,-4,1;0,1,0;` (5 taps, splits) | stream timeout | stream timeout |
| `effect2d:kernel=1,1,1,1,1;...` (15 taps) | stream timeout | record refused, falls back |

So the scene that renders and the scene that stalls were never the same
scene, and `effect2d` with a real kernel belongs with `shadow`, `refract` and
`terrain` rather than against them.  It is also the smallest of them: one
full-screen quad, no indexed geometry, no render-target switching.  That
retired real geometry and target switching as candidates and left the depth
attachment, which is the one thing `effect2d` still shares with the other
three and no fixture had.

It is not the coordinate route itself and not the record's size:

* `coordroute` and `coordroutev`, which route a **coordinate**, render
  pixel-exact at 320x240 under EGL.
* Neither `M9X_SPLIT_PREPROCESS=0` nor `M9X_TRANSITION=0` changes the scene
  result (MEASURED).

A stall leaves the device in a state a module reload does not clear, so every
row above was taken on a freshly booted target whose engine had first been
proved alive by `m9-mesa-glsl texture`.

### 6e. `terrain`'s depth attachment

`terrain` is assessable only at runtime -- the offline harness's synthetic
vertex shader will not link.  It asked for a **Z16** depth attachment at the
target size (MEASURED, `m9x: resource 640x480 levels 1 kind 3 tex 0 format
z16_unorm bind 0x9`), and the driver advertised Z16 as a depth format while
rendering depth only into D24S8, because only D24S8 carries the hierarchical
surface `m9_resource_attach_hz` builds.  Every depth clear and depth test on
that attachment was therefore dropped with "depth clear needs a D24S8
attachment of the target size".

The driver now claims Z16 for sampling only.  st falls back, `terrain` gets
`x8z24_unorm` at the target size and the warning is gone (MEASURED), and
fixture `depthtex16` -- a D16 depth texture, unlisted until now for exactly
this reason -- renders 0 of 76800 pixels wrong and is listed.

### 6f. A split job evaluates depth once, in its first pass

Closing the gap from the working side named it in four fixtures (MEASURED,
one boot each, `m9x_drm-bocap`):

| fixture | what it adds | result |
| --- | --- | --- |
| `conv3`, `convoff` | five taps, split, no depth attachment | render |
| `conv3rep`, `conv3mix` | the split job sixteen times, interleaved | render |
| `conv3notestd` | depth attachment bound, `GL_DEPTH_TEST` off | renders |
| `conv3nowrited` | depth test on, depth writes masked off | **stream timeout** |
| `conv3d`, `convoffd` | depth test and write on | **stream timeout** |

So neither the attachment nor the depth write is the fault: **the depth test
enabled in a continuation raster pass stops the engine.**  The schedule the
driver emitted gave the first pass the caller's depth state and left the
later passes testing without writing, and both of the arrangements that keep
the test alive past the first pass time out:

* first pass tests and writes, later passes test only -- stops the engine;
* first pass with depth off, last pass tests and writes -- stops the engine;
* the caller's depth state in the **first pass only**, later passes with
  `a8 = 0038038a` and no depth bits in `ef` -- **renders**.

The passes of a split shader are one fragment pipeline in sequence, not
independent draws, and the sequence evaluates depth once at its head; the
passes behind it inherit that coverage.  That also fits the captures, whose
later passes restate `a8` with only the write bit cleared.  `conv3zoverd` and
`conv3zunderd` check that the test still decides: a split draw in front
occludes a later plain draw and a split draw behind is rejected, both
pixel-exact.

`M9X_SPLIT_DEPTH` selects the schedule -- 1 (default) the caller's state in
the first pass only, 0 no depth at all in a split job, 2 the arrangement that
stops the engine, which is how the new fixtures were confirmed to fail before
the change.

### 6g. What is left: three passes, and `refract`

With depth evaluated once, `shadow` renders at **142 fps** and `effect2d`'s
3x3 kernel at **84 fps**, both at 1920x1200 with no stream timeout
(MEASURED).  `refract` and `terrain` still stop the engine.

`refract` compiles to **four** raster passes (109, 151, 100 and 127 slots,
textures 0/2/1/1).  `conv7` and `conv10`, seven and ten taps, rendered about
half their pixels wrong, the blue channel far out, with and without a depth
attachment, and never stalled; the same binaries rendered them exactly under
llvmpipe.  Section 6h is what that turned out to be, and it was not the
pass count.

### 6h. What renders a split wrong is a four-unit pass, not a split

**`conv7` is a two-pass split and `conv10` a three-pass one** -- the same
count as `conv3`, which rendered pixel-exact throughout (MEASURED, the record
stride the driver logs is `4 * (passes - 1)`: 4 for `conv7`, 8 for `conv10`;
OFFLINE, `m9-fs-compile-test --glsl` with `M9_FS_SPLIT=1` reports 2 and 3
passes and the driver's own `M9X_DEBUG_FS` slot and unit counts match it
exactly).  The earlier reading of three and four passes was wrong, and with
it the conclusion that a hand-over past two passes was at fault.

What `conv7` and `conv10` have that `conv3` does not is a **consuming pass
that binds four texture units and reads the high register of the last texel
slot**.  A pass's texel slot `k` occupies registers `4k` and `4k + 2`, the
first holding r and g and the second b and a, so a four-unit pass reads b or
a of its last slot out of register `0x0e`.  Fixtures that do only that,
with no split, no record and no route, reproduce the fault exactly
(MEASURED, one boot, 320x240):

| fixture | units | texel register read | result |
| --- | ---: | --- | --- |
| `texhi:2:0`, `texhi:2:1` | 2 | `06`, `02` | 0 of 76800 wrong |
| `texhi:3:0` .. `texhi:3:2` | 3 | `0a`, `06`, `02` | 0 of 76800 wrong |
| `texhi:4:1` .. `texhi:4:3` | 4 | `0a`, `06`, `02` | 0 of 76800 wrong |
| `texhi:4:0` | 4 | **`0e`** | **37822 of 76800 wrong** |
| `tex4b` (every unit's b), `texlohi4` (`0c` and `0e`) | 4 | `0e` among them | **wrong** |

`texhi:N:P` binds N units to one traceable texture and reads the blue channel
of exactly one fetch, the one at output component P; `assign_texels` puts
fetch `i` in slot `N-1-i`, so P is the only variable and it names the
register.  Reading `0c` as well does not rescue `0e` (`texlohi4`), and the
value that arrives there is not another unit's texel but leftover data.

**What decides it is index `88` bit 7** (MEASURED, swept a bit at a time
against `texhi:4:0` with `M9X_R88_AND`/`M9X_R88_OR`, which the backend now
carries for exactly this purpose):

| index `88` | `texhi:4:0` |
| --- | --- |
| as emitted, bit 7 set | 37850 of 76800 wrong |
| `OR 0x40`, bit 6 set as well | **0 of 76800** |
| `AND ~0x80`, bit 7 cleared | **0 of 76800** |
| every other single bit, `0x10`..`0x80000` and `0x1000000`..`0x8000000` | unchanged |

Bits 15:4 flag the register pairs `08/09` .. `1e/1f` the program leaves
unused, and a pair flagged unused is one the engine reuses.  A four-unit pass
was declaring the pair at `0e/0f` free while its own program read a texel
out of it.  Clearing bit 7 says what is true; setting bit 6 works as well,
which reads as the field being taken as a run from the top rather than as
independent pairs, and the honest statement is the one the driver now emits:
**every texel pair the bound units occupy is marked live.**  The record path
already did so by accident -- a producing pass ORs `0x0c << texture_count`,
which for four units sets bits 6 and 7 -- and that is why `conv3`'s four-unit
**producer** read `0e` correctly all along while a four-unit colour pass did
not.

With the rule emitted, `conv7` and `conv10` render **0 of 76800 pixels
wrong**, plain and with a depth attachment, and both are listed, as are the
eleven single-pass fixtures above.  The fragment fixture suite holds at
**132 PASS, 0 FAIL** (MEASURED).  `M9X_TEXEL_HIGH=0` restores the encoding
that renders wrong, which is how each of these fixtures was confirmed to
fail before the change.

Two things this retires: the pass count was never the variable, and the
inter-pass hand-over -- record, routes and sequence field alike -- was
correct past two passes all along.

### 6i. What still stops `refract` and `desktop`, and what it is not

`refract` and `desktop`'s blur still stop the engine with the rule of
section 6h emitted, and neither is the pass count.  Five fixtures were
written to take one property at a time; every one of them renders **0 of
76800 pixels wrong** with no stream timeout (MEASURED, one boot, 320x240,
`m9x_drm-bocap`, `M9X_ENGINE_VRAM=0` so the VRAM work cannot enter the
result):

| fixture | the property it adds | passes |
| --- | --- | ---: |
| `conv13`, `conv16` | a longer split, plain and against a depth attachment | 4, 5 |
| `coordchain` | a chain of computed coordinates, so every pass but the first consumes a coordinate route and every pass but the last produces one | 4 |
| `convrtt`, `convrttin` | a split draw either side of a render-target switch, reading an offscreen target and writing one | 2, 4, 5 |
| `convrtt2` | a split draw sampling what the previous split draw wrote, which is what a separable blur does | 2, 4 |
| `convuni` | uniform tap offsets and uniform weights, two of them shared, which is how glmark2 writes its blur | 2 |

So the pass count to five, the coordinate-route chain, the render-target
switch in both directions, a split draw feeding another split draw, and a
uniform-driven convolution are all **retired** as candidates.

What the two scenes themselves say (MEASURED, one boot a row, 640x480):

| row | outcome |
| --- | --- |
| `refract` at 640x480 and at 1920x1200 | stops the engine at both, so the record's size is not it |
| `desktop:effect=blur:blur-radius=1:separable=true` (three taps, one pass) | 772 fps, renders |
| `desktop:...blur-radius=2` (five taps, **two** passes) | stops the engine |
| the same with `M9X_SPLIT=0` | 846 fps, renders, the blur draws skipped |
| the same with `M9X_SPLIT_SUBMIT=0` | stops the engine |

`desktop` therefore stops at a **two-pass** split -- the same count as
`conv3`, `convoff`, `convrtt` and every other split fixture that renders --
and the control run pins the fault in the split job rather than elsewhere in
the scene.  It is not the wrong-pixel fault of section 6h either: that one
never stalled and is fixed, and `desktop` stalls with the fix in place.
What is left is a property of those two scenes' draws that no fixture has
yet: the remaining differences are the geometry they draw, the size of the
offscreen target against the drawable, and the compositing state around the
blur.

`terrain` no longer stops the engine (MEASURED, 1920x1200, 0 stream
timeouts).  It refuses one shader instead, `terrain.frag`, and its draws are
skipped: that refusal is a **real register-pressure ceiling**.  Under
`M9X_DEBUG_SPLIT` the allocator now names what it wanted, and the passes ask
for **25 to 33 values live at once** against a pool of 16 registers
(`0c..0f` and `14..1f`, three texture units bound) or 20 (two units); the
widest pool a pass can have is 28, at no texture units at all (OFFLINE).
So it is about 1.7x over, not an accounting slip, and what would answer it
is a cut chosen to shorten lifetimes rather than to absorb fetches -- the
splitter cuts only at values that absorb texture fetches today.

## 5. Reproduction

```sh
# on the target, inside flock -w 1800 /root/tmp/m9-hw.lock
cd /root/parhelia-m9x-mesa/src && gcc -O2 -o m9-mesa-glsl m9-mesa-glsl.c -lEGL -lGL -lm
export LD_LIBRARY_PATH=/root/parhelia-m9x-mesa/lib:/root/parhelia-m9x-mesa/dri
export LIBGL_DRIVERS_PATH=/root/parhelia-m9x-mesa/dri MESA_LOADER_DRIVER_OVERRIDE=m9x

# the emitter, one pass at a time: every one of these is pixel-exact
M9X_PASS_LOOP=1 ./m9-mesa-glsl tex4

# the producing pass alone, with the record descriptor: runs
M9X_SPLIT=1 M9X_FS_FORCE_SPLIT=2 M9X_SPLIT_PASSES=1 ./m9-mesa-glsl texaff
# both passes: runs, with the wrong values
M9X_SPLIT=1 M9X_FS_FORCE_SPLIT=2 ./m9-mesa-glsl texaff

# the five-fetch fixture
M9X_SPLIT=1 ./m9-mesa-glsl tex5
```

`M9X_SPLIT_STAGE` selects which binding a split job makes: 1 leaves the record
unbound, 2 binds it with the routes idle, 3 is the real schedule, 4 binds the
record object with a colour descriptor, 5 with the captured two-dword producer
descriptor, 6 with the record format over the colour target's row span, and 7
with the colour descriptor and the colour pass's own state words, which leaves
the bound object as the only difference from an ordinary draw.
`M9X_RECORD_88` replaces the derived index-`0x88` low byte of a record pass,
`M9X_RECORD_89` replaces the whole of index `0x89`, and
`M9X_SPLIT_COVERAGE=0` keeps its plain coverage words, so each can be ruled out
on its own.  `M9X_TRANSITION=0` drops the engine transition of section 3 from a
split job, which separates a fault in that block from one in the record;
`M9X_SPLIT_SUBMIT=0` puts every pass of a split shader in one submission, in
one raster range, which is the vendor's own layout.

To reproduce the stall the sequence field cured, build with
`pass->sequence_ef` cleared; the first ladder of section 2 then returns.

## 6. A split draw that returns to an earlier render-target shape

MEASURED 2026-09-18 on the M9128, one boot a row, the engine proved alive
before and after each case by a plain draw the fixture then checks.  What
stops the engine in glmark2's `desktop` is not the pass count, the shader or
the record: it is the **third** split draw of a sequence whose render target
alternates between two particular shapes.

| case | split draws | targets | result |
| --- | --- | --- | --- |
| `splitsame:20` | 20 | 320x240 twice | renders |
| `splitpair:96x96:3` .. `:20` | 3, 20 | 96x96 / 320x240 | renders |
| `splitpair:88x88:20` | 20 | 88x88 / 320x240 | renders |
| `splitpair:84x96:20`, `:96x84:20` | 20 | .. / 320x240 | renders |
| `splitpair:128x128:20`, `:160x120:20`, `:320x120:20` | 20 | .. / 320x240 | renders |
| `splitsize3:3` | 3 | 84x84, 320x240, 160x120 | renders |
| `splitpair:84x84:2` | 2 | 84x84 / 320x240 | renders |
| `splitpair:84x84:3` | 3 | 84x84 / 320x240 | **stops the engine** |
| `splitpair:168x168:640x480:20` | 20 | the scene's own shapes | **stops the engine** |
| `deskblur` | 2 | one desktop frame | renders (13 of 76800 differ) |
| `deskblur:3` / `deskblurt:3` | 6 / 3 | the blur pair repeated | **stops the engine** |

`84x84` and `88x88` have the same render layout - row span 96, allocation
height 96, same allocation size - so the layout is not what separates them;
only the declared width and height differ.

Retired as the cause, each by a fixture that still stalls with it changed:

- the record object's size (the stalling draw keeps a record ten times larger
  than it needs, and stalls just the same),
- the record object's shape (a record allocated for the stalling draw's own
  target stalls too),
- a fresh record object per split job (`M9X_RECORD_FRESH=1`),
- a fresh depth and HZ surface per target bind (`M9X_TARGET_FRESH=1`),
- the depth schedule (`M9X_SPLIT_DEPTH=0`),
- the preprocessing schedule (`M9X_SPLIT_PREPROCESS=1`),
- the engine transition (`M9X_TRANSITION=0` renders once in four runs, so it
  is a flake and not a cure - it must be run more than once),
- asynchrony: `deskblurf`, which retires every split job with `glFinish`
  before the next binds another target, stalls the same way.

`M9X_SPLIT_PASSES=1`, which emits only the first raster pass of each split
shader, **renders**: it takes a second raster pass to reach the fault, and
`M9X_SPLIT_STAGE=1`, which leaves the record unbound and the routes idle,
still stalls - so the second pass reaches it without the record.

Every fixture named here is in `tools/m9-mesa-glsl.c` and runs without X in
about three seconds:

```sh
./m9-mesa-glsl splitpair:168x168:640x480:20   # the scene's own shapes
./m9-mesa-glsl splitpair:WxH:W2xH2:N          # any pair, any draw count
./m9-mesa-glsl deskblur                       # one glmark2 desktop frame
```

They are dispatched but not listed in the suite, because they stop the
engine; list them the moment the alternating-target split renders.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
