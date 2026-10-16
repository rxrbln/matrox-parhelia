# M9 fragment multi-pass scheduling: the split contract

How the M9148 fragment pipeline splits one GLSL fragment shader across several
raster draws, and what a clean-room scheduler must emit for each.  Everything
below is read out of the saved vendor captures in this tree; the decode
commands are given so every line can be re-derived.

Labels:

* **CAPTURED** — read from a saved vendor command stream or program image.
* **MEASURED** — a vendor readback checked against a CPU reference.
* **DERIVED** — a conclusion joined from several captured facts; the join is
  given.
* **OPEN** — not decoded here.

## 1. The result: passes hand values over through memory, not registers

**The carrier between raster pass `N` and pass `N+1` is an intermediate render
surface.**  The producing pass binds that surface as render target `b0` instead
of the application colour target and writes it through the output register
pairs `0x11`/`0x13`; the consuming pass binds the same address back through the
descriptor-only route index `0x74 + k` with `0x78` the base and `0x79` the
consumer descriptor.  Fragment registers do not survive a draw boundary
(CAPTURED, section 2).

This **refutes** the reading that the two-pass computed-coordinate family
(`fs-tex2d-bias` and its six siblings) is *not* a render to an intermediate
surface because its first program never writes the colour output `0x2d`.
Writing `0x11`/`0x13` and not writing `0x2d` is exactly what a render to an
intermediate record looks like on this engine: `docs/parhelia-m9-raster-stream-limit-re.md`
section 3 states the rule for the raster half — output register pair
`(0x11 + 4j, 0x13 + 4j)` goes to the `j`-th bound target — and the capture
shows the bound target (CAPTURED):

```text
$ python3 tools/m9-command-decode.py --execute \
      captures/m9x-vendor-glsl-programs-20260915/fs-tex2d-bias/m9x-exec-8350-000015-in.bin \
      captures/m9x-vendor-glsl-programs-20260915/fs-tex2d-bias/m9x-exec-8350-000015-bm2-in.bin

range 1 (the raster range), first draw:
  b0=10ec1000  b1=ee190004          <- intermediate surface, not the colour target
  88=0110fffc  8c=00500100  c3=00
range 1, second draw:
  b0=11f09000  b1=cc210080          <- the application colour target
  74=01000310  78=10ec1000  79=ee190204   <- the same surface bound back
  88=0010ffe3  8c=00500200  c3=04
```

The consequence for a scheduler is that a computed coordinate and a shader
needing more than four texture fetches are **the same feature**: both need an
intermediate surface, a producing pass that writes `0x11`/`0x13`, and a route
binding in the consumer.  They differ only in the route word and in whether the
consumer reads the record as a coordinate or as a value.

## 2. Evidence that registers do not persist

Three independent captured facts, none of which a surviving register file can
explain:

1. The producing pass binds an intermediate address as `b0` and the consuming
   pass binds that same address through `0x78` (CAPTURED, the decode above, and
   identically in all seven two-pass cases — section 4).
2. `docs/parhelia-m9-five-sampler-schedule-re.md` records four intermediate
   render passes followed by sampled reads of the same address, and states the
   negative directly: "no register-only interpretation can explain four RT0
   writes followed by sampled reads of the same address".
3. `docs/parhelia-m9-raster-stream-limit-re.md` section 3 gives the renaming
   rule: a pass writes `0x11 + 4j` / `0x13 + 4j` to target `j`, and a later
   pass that binds slot `s` as route `k` reads it as the register pair
   `(0x00 + 4k, 0x02 + 4k)` — the same registers a texel arrives in.  A value
   that survived in a register would not be renamed.

## 3. Per-pass state, from the captures

Shared by every two-pass case (CAPTURED; `fs-tex2d-swizzle` differs only where
noted):

| register | producing pass | consuming pass |
|---|---|---|
| `c3` | `0x00` | first slot after the producing program |
| `88` | `0110fffc` (`0110effc` for `fs-tex2d-swizzle`) | `0010ffe3` |
| `8c` | `00500000 \| nconst << 8` | `00500000 \| nconst << 8` |
| `8d`/`8e` | the pass's own constant span | the next span, starting where the first ends |
| `b0`/`b1` | intermediate surface + producer descriptor | colour target + its descriptor |
| `a8` | `0038038a` (depth write set) | `00380388` (depth write cleared) |
| `a9` | `88ffff00` | `08ffff00` |
| `ae` | `32444442` | `22444442` |
| `ab`/`ac` | `ffffffff` / `ffffffff` | low 16 bits of `ab` are the colour mask, `ffff` here |
| `0x74` | `00000100` (idle) | the route word, section 4 |
| `0x70`/`78`/`79`/`7a`/`7c` | the 1x1 placeholder texture, `7c=00000200` | the application texture, `7c=02409200` or `12409200` |
| index-23 declaration | one interpolated input `00100d88` plus the fixed trio `00010688 00c231b6 00d30688` | the fixed trio alone, `00000688 00c131b6 00d20688` |
| streams | four slots, strides `00100010` / `00100006` | three slots, strides `00060010` / `00000010` |

Both passes share **one** `c2` image, **one** `6c` linked record and one
preprocessing range; the two constant spans are consecutive inside the same
arena.  The depth and auxiliary surfaces stay bound across both passes.

Three readings of that table need qualifying (CAPTURED, all seven cases
re-decoded):

* the colour **write mask is the low sixteen bits of `ab`**, and it is
  `ffff` in both passes of all seven cases.  The captured pass-2 upper
  halves (`ba51ffff`, `c023ffff`, ...) and `ac` values differ per case with
  no schedule meaning; they are the process-varying fragment
  `docs/parhelia-m9-cpu-tnl-raster-state.md` already describes.  A
  clean-room stream emits `ab = mask | 0xffff0000`, `ac = 0xffffffff`.
* **depth is off throughout this fixture**: `a8` bit 0 is clear and `ef`'s
  depth field is zero in both passes.  The `0038038a` / `00380388`
  difference is the depth-write bit, but it is inert here, so the capture
  does not show what a depth-**enabled** split must emit.  Measured
  separately: only the **first** pass of a sequence may drive the depth
  unit.  A continuation pass that keeps the depth test alive stops the
  engine, whether or not it also writes (MEASURED, fixtures `conv3d`,
  `convoffd` and `conv3nowrited` all time out against that schedule while
  `conv3notestd` renders).  The driver therefore emits the caller's `a8`
  and `ef` in pass 0 and `a8 = 0038038a` with no `ef` depth bits in every
  pass behind it; `conv3zoverd` and `conv3zunderd` confirm the test still
  decides the result.  This is the "at most once" depth gate of section 6,
  and the hardware enforces it by stopping rather than by mis-rendering.
* the index-23 rows are right, and incomplete: declaration writes come in
  groups of four, so the consuming pass emits a fourth `00000000` pad word,
  and index `22` bits 11:8 hold the declaration word count **minus one**.

Three further facts the table does not carry (CAPTURED):

* index `30 = 0` is emitted in the **consuming** pass only.
* the producing pass's 1x1 placeholder texture is not a dedicated
  allocation: the vendor points slot 0 at `113c1000`, an unused slice of the
  same transient stream arena, read with `79=cc010001 7a=00010001`.
* the consuming pass **drops stream slot 0 and shifts slots 1..3 down**,
  address and stride together, because the interpolated input the producer
  carried is gone.  Producer `(113d0400,0x10) (113fe000,0x10)
  (1140d400,0x06) (113eec00,0x10)`, consumer `(113fe000,0x10)
  (1140d400,0x06) (113eec00,0x10)`.
* the shared `c2` image holds the two fragment programs first and then
  **three** preprocessing/vertex programs, not one, each with its own `c3`.
* the two constant spans are literally consecutive -- pass 2's `8d` equals
  pass 1's `8e` -- and `fs-tex2d-swizzle`'s producer span is **empty**
  (`8d == 8e`, count 0), which an emitter has to handle.

Note the two facts a scheduler is most likely to get wrong:

* the producing pass still binds a texture — the opaque-black 1x1 placeholder —
  because index `88` bit 20 is set and setting it without a bound unit-0
  texture stops the engine (MEASURED, `docs/parhelia-m9-raster-stream-limit-re.md`
  section 4);
* the consuming pass of every captured coordinate case declares **no**
  interpolated inputs at all.  Whether a consuming pass may declare
  interpolated inputs *and* a route at the same time is **OPEN** — no captured
  case does both, and glmark2 `shadow` needs it (its second pass still reads
  normals and colours).

## 4. Producer descriptor, consumer descriptor and route word

The intermediate record is addressed by a `0xee` descriptor whose low byte is
the record stride in dwords and whose second byte is a start offset in dwords.
The route word at `0x74 + k` has `0x10` as its low byte and three component
selector bytes above it, `0xff` meaning "absent".

All seven captured cases (CAPTURED; the coordinate each program builds is read
from the program image with `tools/m9-glsl-program-extract.py`):

| case | what pass 1 writes | producer `b1` | route `0x74` | consumer `0x79` |
|---|---|---|---|---|
| `fs-tex2d-swizzle` | `0x11 = (t, s)` | `ee090002` | `00ff0110` | `ee090002` |
| `fs-texrect` | `0x11 = (s', t')` | `ee090002` | `00ff0110` | `ee090002` |
| `fs-tex2d-bias` | `0x11 = (·, lod)`, `0x13 = (s, t)` | `ee190004` | `01000310` | `ee190204` |
| `fs-tex2d-proj3` | `0x11 = (q, ·)`, `0x13 = (sq, tq)` | `ee190004` | `01000310` | `ee190204` |
| `fs-shadow2d` | `0x11 = (ref, ·)`, `0x13 = (s, t)` | `ee190004` | `01000310` | `ee110204` |
| `fs-tex3d` | `0x11 = (r, ·)`, `0x13 = (s, t)` | `ee190004` | `01000310` | `ee110204` |
| `fs-texcube` | `0x11 = (c0, c1)`, `0x13 = (c2, ·)` | `ee190004` | `03020110` | `ee110004` |

Reading the table (DERIVED, and consistent with all seven):

* the **producer** descriptor sizes the record: `0002` = two dwords (one
  register pair, `0x11` only), `0004` = four dwords (`0x11` and `0x13`).  Bit
  20 of the descriptor (`ee09` vs `ee19`) moves with that size.
* the **consumer** descriptor's second byte is where the unit's `(s, t)` pair
  starts inside the record: `02` for the four-dword cases, whose `(s, t)` sits
  in `0x13`, and `00` for the two-dword cases and for the cube, whose first
  two components sit in `0x11`.
* the **route word** supplies the remaining components.  `00ff0110` is "take
  the record's own two scalars", used both for a two-component coordinate and,
  in `docs/parhelia-m9-five-sampler-schedule-re.md`, for a value handed between
  passes (`00ff0110`, `00ff0910`, `00ff0510`, `00ff0d10` there, the second byte
  following the record's slot offset).  `01000310` and `03020110` add a third
  component for the projector, the LOD bias, the shadow reference, the 3-D `r`
  and the cube's major axis.

The exact field boundaries inside the route word are closed in
`docs/parhelia-m9-fragment-pass-routes.md`: one model rebuilds all twenty
descriptor and route values of this table and of the five-sampler schedule,
and the m9x driver checks it offline.  The reading of bit 20 above holds for
producers only -- the five-sampler consumers read a sixteen-dword record with
that bit clear -- so bits 20:19 are a form selector, not a size.

## 5. Pass budget

* Four independent fetches is the ceiling for one pass, in the four physical
  slots `0x70..0x73` (CAPTURED, `docs/parhelia-m9-openbits-gl-capture.md`
  section 2, table rows `indep_2` .. `indep_4`).
* A continuation pass carries at most three application textures, because one
  slot holds the previous pass's record.  Measured pass counts: 5 -> 2,
  6 -> 2, 8 -> 3 (MEASURED, same table).  `ceil(N / 3)` above four is the
  doc's extrapolation from those three points.
* A fully dependent chain costs one pass per sampler, because each fetch needs
  the previous result as its coordinate (MEASURED, rows `samplers_4`,
  `samplers_5`, `samplers_9`).
* Six interpolated inputs per pass, bounded by the four-bit declaration word
  count at index `0x22` (CAPTURED/MEASURED,
  `docs/parhelia-m9-raster-stream-limit-re.md`).  With texture coordinates the
  bound is `2 * varyings + coords + 3 <= 16`.

### The tap counts, measured

Neither the pass count nor the tap count previously recorded for `effect2d`
is right (MEASURED offline: the shaders glmark2 generates were rebuilt from
`src/scene-effect-2d.cpp` and `src/scene-desktop.cpp` and compiled with the
driver's own `m9-fs-compile-test --glsl`, counting `tex` instructions in the
optimised NIR):

| scene shader | `texture2D` calls in the GLSL | fetches reaching the backend |
|---|---:|---:|
| `effect2d kernel=0,1,0;1,-4,1;0,1,0;` | 9 | **5** |
| `effect2d kernel=1,1,1,1,1;` x3 | 15 | **15** |
| `desktop` separable blur, radius 5 | 11 per direction | **11** |

The 3x3 edge kernel normalises to `0, .25, 0, .25, -1, .25, 0, .25, 0`, and
Mesa folds the four `texture2D(...) * 0.0` terms away before the driver sees
them, so it is a **five**-fetch shader.  glmark2's second default kernel is
`1,1,1,1,1;1,1,1,1,1;1,1,1,1,1;` -- five columns by **three** rows, fifteen
taps, not twenty-five; nothing folds.  At four fetches in the first pass and
three in each continuation that is 2, 5 and 4 passes respectively.

### A correction to the tap claim

`docs/parhelia-m9-openbits-gl-capture.md` section 2 asserts that "many taps of
one sampler stay in one pass, which `taps_49` confirms", and concludes that
glmark2 `effect2d` "needs none at all".  **That pass count is not supported by
anything in this tree.**  What is supported is only correctness:
`result-matrix.txt` records `taps_4/9/16/25/49 PASS checked=64 mismatches=0`
(MEASURED), and `tools/m9x-openbits-probe.c` builds those shaders as
`a += texture2D(s, uv + vec2(dx, dy) / TS)` — a distinct coordinate per tap.
No `cap-taps_*` submission was saved, so the number of raster passes the vendor
used for them was never captured.

Each tap is an independent fetch at a distinct coordinate, so by the engine's
own four-slot rule it consumes a slot, and a 49-tap shader is an `N = 49`
independent-fetch case, not a free one.  Until a `taps_*` submission is
captured, treat the pass count for many taps as `ceil(N / 3)` like any other
independent fetch set, and treat the "needs none at all" sentence as an
unsupported inference.

The same correction applies to the comment on `M9_FS_MAX_TEXTURES` in
`m9_fs_compile.h`, which cites corpus section 6 for "repeated taps of one
sampler take a unit each".  The *unit* claim is right; what section 6 does not
say, and what the openbits note wrongly adds, is that many taps avoid the
split.

## 6. What a scheduler has to emit

For a shader that needs `P` passes, in submission order:

1. One `c2` image holding the `P` fragment programs back to back, then the
   vertex program.  Pass `i` enters at the cumulative slot offset of the
   programs before it; the preprocessing draw still enters at the vertex
   program.  One `6c` linked record serves all of them.
2. One constant arena whose `P` spans are consecutive: pass `i` emits
   `8d` = span start, `8e` = span end, `8c = 0x00500000 | count << 8`.
3. For each pass `i < P - 1`: `b0` = an intermediate record surface with the
   producer descriptor of section 4, `a8` with the depth-write bit set on the
   first pass only, `88 = 0110fffc`, the 1x1 placeholder texture in slot 0, and
   a program that writes `0x11` (and `0x13` when the record is four dwords)
   and never `0x2d`.
4. For the final pass: `b0` = the application colour target, `a8` with the
   depth-write bit cleared, `88 = 0010ffe3`, the route words at `0x74 + k` with
   `0x78`/`0x79` for each record it consumes, then the application textures in
   the remaining full slots.
5. The index-`43` dependency advanced for each pass; the five-sampler
   capture and all seven two-pass cases advance it by `0x20`.

   There is **no range break between the passes** (CAPTURED, all seven: both
   raster draws live inside range 1, `[0x784,0xd38)`).  What separates them
   is a draw epilogue and a draw prologue of `e6` words, with two writes of
   index `0x44`.  Section 6.1 decodes them; the m9x driver emits them and
   the kernel accepts them, and doing so changes nothing about the record
   (section 6.2).  The `43` advance is emitted as captured.

   The full dependency trace of a two-pass case (CAPTURED): `e3` advances by
   `0x10` on every write across the whole submission, and `43` is written
   once per draw with the value of the `e3` immediately before it, so
   consecutive `43` values are `0x20` apart -- `e3=380`, `e3=390` `43=390`,
   `e3=3a0`, draw 1, `e3=3b0` `43=3b0`, `e3=3c0`, draw 2.

### 6.1 The inter-draw transition, decoded

The block is **not** special to a pass boundary: the vendor puts it around
**every** draw of a native fragment job, the three preprocessing draws
included (CAPTURED, 343 prologues and 230 epilogues over
`captures/m9x-vendor-glsl-programs-20260915` and
`captures/m9x-vendor-openbits-20260917`).  A draw that opens a range carries
a prologue and no epilogue; a draw inside one carries both.

```text
epilogue   454545e6  e6 = 38000000                          end the block
           454444e6  e6 = 40004000  44 = 40004000  44 = 40004000
           454545e6  e6 = 0c000000                          record writers only
prologue   45e6e6e6  e6 = 60600060 | 604fff9f               open | resume
                     e6 = 2c000002 | 2c000000               the matching word
                     e6 = 49d000nn                          the block length
           454545e6  e6 = 08000000                          start the block
```

Reading the fields:

* `e6 = 0x60600060` with `0x2c000002` opens a range's **first** block and
  `0x604fff9f` with `0x2c000000` a **continuation** block (CAPTURED: all 113
  range-opening prologues take the first form).  The two words are literal
  presets; their internal fields are **OPEN**.
* `44 = 0x40004000`, twice, always behind `e6 = 0x40004000` in the one
  packet `454444e6` (CAPTURED, 460 writes of `44`, every one that value).
  This is the same index the Windows late-state helpers `0xfc620` and
  `0xfcb60` write eight of, which
  `docs/parhelia-m9-texture-coherency-re.md` describes as address-domain
  synchronisation with resource completion ordering.
* `e6 = 0x0c000000` closes the epilogue of a draw that wrote the
  inter-pass record and of no other: present behind every one of the eight
  captured `ee190004` producers and absent behind every `68002000` stream
  target (CAPTURED).
* `e6 = 0x49d000nn` carries **the length of the block the prologue opens**,
  in dwords, counted from the prologue's own packet header through the draw
  packet that ends the block, less one -- and, for a draw that closes its
  range, up to but not including the draw packet, because the range close
  bounds it.  That rule reproduces the low byte of **all 343** captured
  prologues exactly (DERIVED, `tools/m9-command-decode.py` over both capture
  sets).  Captured lengths run `0x80 .. 0xcc` with bits 15:8 always clear,
  so the length is read as the low sixteen bits.

Note that the vendor's own stream pads every packet of fewer than four
values with one `0x45454545` dword, which the m9x driver does not; a
clean-room emitter therefore computes the length over its own layout.

### 6.2 What emitting it buys: nothing, measured

`m9_parse_commands` now accepts these literals and bounds the length against
the command stream it has validated, and `m9_draw.c` emits the block around
every raster draw of a split job.  On the M9128 (MEASURED,
`docs/parhelia-m9-fragment-pass-hardware.md` section 2):

* a one-pass shader driven through the multi-pass loop **with** the block
  renders pixel-exact, for four fixtures -- so the engine accepts it;
* a producing pass that binds the record at `b0` with `b1 = ee190004`
  **still** stops the engine, with the block emitted exactly as captured.

The transition is therefore not what lets the engine take the record's
format as a render target.

Correctness gates that the captures do **not** settle, restated from
`docs/parhelia-m9-five-sampler-schedule-re.md` because they bound what may be
scheduled: blending must read the application destination and run exactly once,
on the final pass; depth and stencil updates must happen at most once; a
`discard` in an early pass must suppress every later write; derivatives must be
computed before a value crosses a record boundary; an occlusion query must
count application fragments once.  A shader whose compiler cannot prove these
must be refused, not split.

## 7. Reproduction

```sh
# the two-pass raster range of any of the seven cases
c=captures/m9x-vendor-glsl-programs-20260915/fs-tex2d-bias
python3 tools/m9-command-decode.py --execute \
        $c/m9x-exec-8350-000015-in.bin $c/m9x-exec-8350-000015-bm2-in.bin

# the programs and per-draw state of a case
python3 tools/m9-glsl-program-extract.py $c
```

## 8. The splitter as implemented, and what it does not yet buy (2026-09-19)

The m9x backend now partitions a shader by **the resources a raster pass
holds**, not by the fetch count alone, and the three defects that stood in
front of that are fixed.  What that buys and what it does not is measured
below.

### 8.1 The three resources, and the cut for each

`m9_fs_compile_passes()` compiles the whole shader, reads the refusal, and
cuts for whichever resource ran out:

| refusal | cut | scored on |
|---|---|---|
| more texture fetches than one raster pass can bind | texel slots | the slots the producer absorbs, at least two, leaving the consumer strictly fewer than the whole |
| out of temporaries | the register file | the records the consumer is left reading, then the widest half |
| too many interpolated inputs | the interpolation registers | the widest half's interpolant count |
| texture coordinate is not a varying | the coordinate itself | -- the blocker names the cut |

A **texel slot is a fetch or a record**, because the consumer binds both the
same way.  Scoring the unit cut on fetches alone made no *gather* admissible
-- a pass that reads several records and writes one -- so a shader whose
fetches were all placed could still be refused for needing more slots than a
pass has just to read its own records back (MEASURED on the card, glmark2
`terrain`: five cuts, then a remainder with no fetch at all, five records and
no candidate).

A register-file cut is scored on **the records the consumer must read back**
rather than on the widest half, because a record that stays live costs its
four dwords of stride for every fragment of every pass.  `terrain.frag` goes
from seven passes over six record slots to four over three on that change
alone (OFFLINE).  `M9X_SPLIT_RANK=1` restores the widest-half score.

Register-file cuts are on by default; the `M9X_SPLIT_PRESSURE` knob that
gated them is gone.

### 8.2 Three defects the splitter was standing behind

* **The coordinate's varying location was re-derived by chasing operand 0**
  of the transform, which lands on the *uniform* load for the common
  `scale * varying + bias` written with the scale first.  The stream was then
  built from whatever location that load happened to read -- 2, the built-in
  secondary colour, for every one of `terrain.frag`'s four transformed
  fetches.  The parse already names the varying; it is used.
* **Two units could not read one varying location** when the coordinate pair
  starts at an odd component.  That was a stand-in for the driver keying a
  coordinate stream by the slot *half*, which also gave a pair at component 0
  and one at component 1 the same stream.  The stream is keyed by the
  component it starts at, and the refusal is narrowed to the one location
  with no CPU view, `VARYING_SLOT_COL0`.
* **The layout gate counted one coordinate stream per sampler.**  Four units
  sharing one transformed stream therefore left room for two interpolants
  instead of four.  It counts the streams the emitter builds
  (`m9_fs_coord_stream_count`).

Each is checked by a case in `m9-fs-compile-test`, and each case fails when
its change is reverted (OFFLINE, all three reverted in turn).

### 8.3 Two driver defects a deep split found

* `m9_vbuf_render::extra_offset` was sized for the CPU views of a **single
  pass**.  A split variant has a view per interpolated input and per
  coordinate stream of every pass, so a seven-pass shader wrote eight entries
  past it and crashed in `ralloc_size` on the next allocation (MEASURED).
* A partial multi-pass draw whose covered box encloses **no pixel centre**
  was refused and charged to the skip count.  It writes nothing -- MEASURED
  on `terrain`, two draws a frame spanning a band 0.37 pixels high -- so it
  is dropped without being counted.

### 8.4 What it buys: every draw, and still the wrong picture

MEASURED on the M9128 at 1920x1200, `glmark2 -b terrain`:

| | before | after |
|---|---|---|
| draws skipped a frame | 24 | **0** |
| stream timeouts | 0 | 0 |
| frame rate | 1 fps | 1 fps |
| picture | a flat clear | the mesh silhouette and the sky, with the body wrong |

CAPTURED on scanout after the index-88 fix of 8.5, 1920x1200 and 800x500,
10 s into the run, a known-good fixture passing after it: **the mesh is
there, with relief and detail, and the picture is still not right.**  The
distinct-colour count over the root window goes from 1297 to **104000** and
the frame from a flat near-white wash to a lit landscape, but the whole scene
is blue-dominant where the reference is brown and green, and the near field
carries a **regular striped block pattern** fixed in screen pixels that the
far field does not.  So `coordconv:4` was one of terrain's defects and not
all of them.  The earlier reading that the body comes back with R and G
exactly zero is withdrawn: it described a frame captured at a different point
of the run, and the channels of the present frame are all non-zero.

**The two symptoms are two scenes, and the scene option separates them.**
`terrain` is a chain: the terrain itself to an offscreen target, a bloom
pair, then a nine-tap tilt-shift blur to the window, and each half of the
chain can be turned off from the command line.  CAPTURED at 1920x1200, one X
server and one process a variant, the known-good fixture rendering after
each, against llvmpipe frames of the same four variants taken on the build
host (`tools/` has no capture of its own for that; `maim` off an `Xvfb`
root):

| variant | picture |
|---|---|
| `terrain:bloom=false:tilt-shift=false` | silhouette and relief exact, **colour wrong**: the far field saturated blue, the near field dark green with saturated red patches |
| `terrain:bloom=true:tilt-shift=false` | the same colour fault, no stripes |
| `terrain:bloom=false:tilt-shift=true` | the same colour fault **and** the striped blocks |
| `terrain` (both) | both |

So the **colour** fault is in `terrain.frag` drawn straight to the window,
before any offscreen target or post-processing pass exists, and the
**striped blocks** are the tilt-shift blur alone.  They are two defects, not
one, and the colour one is first because the blur's input is the wrong
picture.

### 8.4a Where the colour fault is, and what it is not

A ladder of terrain.frag variants -- the stock shader with one diagnostic
value written to `gl_FragColor` after the real line, so the schedule stays a
terrain schedule -- was run on the card and against llvmpipe on the same
scene, with `--data-path` pointing at a tree whose only difference is that
one file.  Frame means, MEASURED 2026-09-19 at 1920x1200, one X server and
one process a probe:

| probe | card vs llvmpipe |
|---|---|
| `colDiffuse1`, `colDiffuse2`, `tDisplacement`, their mix, `specularTex` | matches |
| `normalTex`, `normal`, `lVector`, the half vector | matches |
| `pointDotNormalHalf`, `pointDiffuseWeight`, `pointSpecularWeight` | matches |
| `pointSpecularWeight * pointDiffuseWeight * 8` | matches |
| `uDiffuseColor * pointDiffuseWeight` | matches |
| `pointDiffuse` = `pointLightColor[0] * uDiffuseColor * pointDiffuseWeight` | matches |
| `totalDiffuse`, `ambientLightColor * uAmbientColor` | matches |
| **`pointLightColor[0] * uSpecularColor * pointSpecularWeight * pointDiffuseWeight * 8`** | **250, 248, 16 against 32, 30, 26** |
| **`pointSpecular * 8`** | **wrong, the mean 4.2x the reference** |
| **`totalDiffuse + totalSpecular`** | **wrong** |
| **`totalDiffuse + ambientLightColor * uAmbientColor + totalSpecular`** | **a flat 135, 136, 255 over the whole mesh** |

Every failing probe contains `pointSpecular` and every passing one does not.
Both of its scalars are right on their own and their product is right; what
is wrong is that product multiplied by the two uniform colour vectors, and
the way it is wrong names the shape: the light and material colours are
both `(1, 1, 1)`, so the correct value is the product itself, and the card
returns **red and green saturated with blue carrying the product**.  On a
machine whose registers are pairs, that is the scalar multiply reaching one
pair of the result and not the other.  The flat `135, 136, 255` of the last
row is the same thing seen through the sum: red and green are
`ambientLightColor * uAmbientColor` exactly and blue saturates.

**Program size is not the cause, and the reading that it was is retired.**
The probes that fail are also the longest -- 207 to 222 slots against 160
and fewer for every passing one -- and that correlation was taken for the
cause until it was measured.  `tools/m9-mesa-glsl.c` gained `fschain:N` and
`fsconst:N` for it: both rotate a pair of varyings N times, so a given N is
the same slot count either way, and only the constant count differs.
MEASURED on the M9128: `fschain:40` is **337 slots and renders every pixel
exactly**, and glmark2's own simplex-noise shader compiles to a **1701-slot**
single pass in the same scene and draws the height map correctly.  There is
no slot ceiling anywhere near the failing probes.

`fsconst` did find a ceiling, in the **constant count**, and it is far above
what terrain asks for: `fsconst:24` (209 slots, 56 constants) renders
exactly and **`fsconst:30` (257 slots, 68 constants) fails every one of
76800 pixels and stops the engine**.  A captured vendor pass carries 39
constants (`m9_fs_corpus.h`, entry 189 pass 2), so the limit lies between 56
and 68 and is not decoded further here.  The driver now bounds the count at
the highest measured-good 56 (`M9_RASTER_MAX_CONSTS`, checked in
`m9_raster_fs_init_textured`, which used to refuse only above the field's
127).  MEASURED on the card with the bound in place, both locks held and a
known-good fixture passing after: `fsconst:24` still renders every pixel
exactly, and `fsconst:30` now prints "a job could not be built; those draws
are skipped" and leaves the engine alive -- where before it failed all 76800
pixels *and* logged a stream timeout that cost a reboot.  Splitting on the
constant count, rather than refusing, is the remaining work and is listed in
the backlog.

The fault is **not** the integer-uniform lowering merged the same day: the
whole ladder was re-run on a tree rebased onto `cfaaf3a`, and
`terrain:bloom=false:tilt-shift=false` comes back with the same frame to
three figures -- mean 68.6, 43.6, 113.8 against llvmpipe's 95.0, 88.8, 31.7,
and the same 58.6 of 255 mean difference over an 8x8 tiling.

**What no fixture has reproduced yet.**  Five new fixture families were
built for this and all of them render exactly on the card: `chainvec:OP` and
`chainlit:OP` (a special function's result multiplied into a uniform or
literal vector, for pow, exp2, log2, rcp and rsq), `chainvec2:OP` (the same
with two uniform vectors, the shape `pointSpecular` has), `chainmany:N` (N
pow chains in one program), and `speclit` (terrain's specular term reduced
to a fixture: a normal from a texture through a tangent frame, four
normalizations, a pow with a uniform exponent, times two uniform vectors).
`filefs` was added with them: it renders a fragment shader named by
`M9_GLSL_FS` against a vertex shader named by `M9_GLSL_VS` and compares the
readback with a raw frame from `M9_GLSL_REF`, which is how a shader too big
to write a CPU reference for is A/B-ed against llvmpipe in seconds instead
of against a benchmark run in minutes.

Run through `filefs`, **terrain.frag itself is correct**: the stock shader
with the failing probe appended, with terrain's own uniform names and
values, differs from llvmpipe on 14 of 76800 pixels -- chain precision, not
the fault.  The driver compiles it to **two** raster passes there and to
**one pass of 205 slots, two texture units and four interpolated inputs** in
glmark2 (CAPTURED, `M9X_SPLIT_TRACE=1` in both).  So the trigger is not the
shader text alone: it is that text under the schedule glmark2's own vertex
shader and geometry produce, and the next step is to make the fixture take
that schedule.

### 8.4b The fixture takes glmark2's schedule, and the fault is the allocator

**The varying layout was the whole of the difference.**  The fixture's own
vertex shader writes `vTangent = vec3(col.r, col.g, 0.5)` and
`vNormal = vec3(0.25, col.b, col.r)`, so the linker's cross-stage constant
folding and varying compaction pack terrain's five varyings into **two**
slots and the fragment shader reads `VARYING_SLOT_VAR0` and `VAR1` only.
terrain's own vertex shader leaves every component varying, which fills
**four** slots.  A fixture vertex shader that writes the same five varyings
with no constant component -- `tools/m9-terrain-fixture.vert`, two normalized
vectors built from all four attributes, their cross product, `vUv` and a view
position -- compiles `p-ps3.frag`, the failing probe, to `1 pass, 178 slots,
33 consts, 2 units, 4 varyings, 88 0f30030f`, and the emitted program is
**byte-identical** to the one terrain's own vertex shader produces (OFFLINE,
`M9_FS_PROGRAM=1` dumps compared).

**Matching the schedule is not enough on its own, and that is worth saying.**
Whether the frame shows the clobber depends on what the clobbered register
happens to be carrying, which is a matter of the varying *values*.
`tools/m9-terrain-fixture-clobber.vert` has the same schedule and the values
that show it; MEASURED through `filefs` on the M9128, one reference a
shader, `M9X_RA_RESET=0 M9X_CHECK_RA=0` against the fix:

| fixture vertex shader | placement carried over | fixed |
|---|---:|---:|
| `m9-terrain-fixture-clobber.vert` | **24712** of 76800 | **470** |
| `m9-terrain-fixture.vert` | 2145 | 2839 |

so only the first is a regression test; the second is the clean schedule
match.  The residual of both is the specular highlight's boundary, which a
`pow` of exponent 30 makes one pixel wide -- all three channels differ
together and alpha never does, where the defect itself put red and green
wrong on 38393 pixels and blue on 222.  That 38400-pixel reading was taken
on the tree before the multisample-state rebase and is not comparable with
the numbers above; it is kept only because it is what named the channels.

**Index 88 is not the cause here, and that reading is retired for this
defect.**  `p-swdw8`, which renders exactly, and `p-ps3`, which does not,
compile to the same one-pass shape and carry the **same** index 88,
`0f30030f` (OFFLINE).  Nothing in 88 separates them.

**The cause is that the register allocator carried its placement across
compilation attempts.**  `compile_output()` makes up to two attempts and
`emit_root()` a depth-first retry between them; `reset_output()` clears the
lops and `reset_graph()` the nodes, but neither clears `c->values`, and
`allocate()` reset only `v->last`.  Its free scan matches every value with
`v->general && v->reg >= 0 && (v->last == s || (v->def == s && v->last <
0))`, so on the last attempt it matched the **previous** attempt's values --
their `reg` and `def` still set, their `last` reset to -1 -- and pushed
their registers into the free list while this attempt still held live values
in them.  Traced on `p-ps3.frag`: three `allocate()` runs, and in the third
the scan frees `v13 reg 0x0a`, `v36 reg 0x18`, `v38 reg 0x1f` and thirty
more that the run had never placed (OFFLINE, a temporary trace in the
allocator).

The damage is exactly the shape the frame showed.  81 pairs of live ranges
shared a register in that pass; the one that mattered is **`v311`, the xy
lane of `pointLightColor[0] * uSpecularColor`, in `0x1f` from slot 34 to
slot 171**, with sixteen later values handed the same register.  The zw
lane, `v310`, went to the scratch register `0x13` and was never disturbed --
which is why red and green came back saturated and blue carried the product.

`allocate()` now clears `reg`, `def`, `last`, `general` and `rg_read` on
every value before it places any, and checks afterwards that no register is
held by two values whose live ranges overlap; a pass that breaks that is
refused so the splitter cuts instead of rendering every pixel silently
wrong.  `M9X_RA_RESET=0` restores the placement that carried over and
`M9X_CHECK_RA=0` turns the check off: with the reset off the check refuses
`terrain.frag`, and with both off the compiler still emits the three-pass
schedule that shares registers (OFFLINE, both measured).

Honest allocation costs registers, and terrain's probe was two short of its
fifteen live values, so the splitter cut and the cut was refused for "special
function of a texture read".  An **interpolated input's register now joins
the general pool at its last read** rather than staying reserved for the
length of the program -- it is an ordinary register written once by its own
slot -- which returns six registers to a four-varying pass.  Blending keeps
`18..1f` to itself.  With that, `p-ps3` is one pass again.

**The check earned itself immediately.**  The interpolation-register reuse
below was pushing *every* interpolated input's register onto the general free
list, and four bound units put `base` at `0x10`, so an interpolated value is
placed in `0x10` or `0x11` -- and `0x11` is `REG_S0`, which the backward scan
hands out on its own.  `m9-fs-compile-test`'s `split shared-coordinate-stream`
case, four units sharing one transformed stream with four more interpolants,
then held `0x11` as a general temporary and as scratch at once (`v79 [70,74]`
against `v82 [73,75]`, `v109 [97,107]` against `v117 [105,107]`) and the check
refused the shader rather than let it render.  An interpolation register now
rejoins the pool only outside `10..13`, the band the fresh run itself skips;
the case is back to one pass and four fetches, and the reuse that matters --
`1a..1f` -- is unchanged, so terrain's probe still compiles to one pass.  That
shape is **supported**, not refused: it was this reuse and not a capability
limit.

MEASURED on the M9128 after the change, engine proved with a known-good
fixture before and after each and no stream timeout:

| | before | after |
|---|---|---|
| `p-ps3` through the clobber fixture | 24712 of 76800 differ | **470** (tol 8), all channels equal |
| `terrain.frag` through `filefs` | 76800 of 76800 differ | 62030 |
| `terrain:bloom=false:tilt-shift=false` frame mean | 68.6, 43.6, 113.8 | **92.6, 89.7, 79.9** (llvmpipe 95.0, 88.8, 31.7) |
| 8x8 tile difference | 58.6 of 255 | **21.9 of 255** |

Red and green now match llvmpipe; the picture has the reference's relief,
its grass and its dirt, and what is left is a blue excess on the lit pixels.

### 8.4c What is left: the third component of a narrowed record

MEASURED 2026-09-19 at 1920x1200, one X server and one glmark2 process a
probe, each against an llvmpipe frame of the **same** probe captured on the
build host, scored as the mean 8x8 tile difference:

| probe, replacing terrain.frag's last line | tile difference | note |
|---|---|---|
| `texmix`, the diffuse mix alone | 2.9 | correct |
| `td` = `totalDiffuse` | 7.3 | correct |
| `ps` = `pointSpecular * 8` | 9.8 | correct |
| `light` = `totalDiffuse + ambient + totalSpecular` | 9.7 | **correct** -- this was the flat 135, 136, 255 |
| `orig`, the stock shader | **21.9** | blue 79.8 against 31.7 |
| `gl_FragColor.xyz * vec3(0.4)` | 1.7 | a record times a literal |
| `vec3(0.4) * light` | 7.6 | a literal times a computed vector |
| `gl_FragColor.xyz * (ambientLightColor * uAmbientColor)` | 2.0 | a record times a uniform product |
| `gl_FragColor.xyz * totalDiffuse` | **9.6** | blue 38.5 against 22.6 |

So every term of terrain's colour is right on its own and the fault is in
the **product of the record the texture mix is carried in with a vector the
consuming pass computes**.  Narrowing that product to one component names
it:

| probe | tile difference |
|---|---|
| `vec3(gl_FragColor.x * totalDiffuse.x)` | 4.2 |
| `vec3(gl_FragColor.y * totalDiffuse.x)` | 4.1 |
| `vec3(gl_FragColor.z * totalDiffuse.x)` | **17.8** |
| `vec3(gl_FragColor.w * totalDiffuse.x)` | 8.6 |

and the wrong frame is not wrong in an arbitrary way: the card's `.z` frame
sits **3.7 to 4.4** from the `.y` frame -- the same distance a correct probe
sits from its own reference -- and **17.8** from its own.  **The third
component of the record comes back as the second.**  It is not a lane of the
output: `gl_FragColor.zyx * totalDiffuse` is wrong in red, where `.z` went.
And it needs the record narrowed: `vec3(gl_FragColor.z * totalDiffuse.x,
gl_FragColor.y * totalDiffuse.x, gl_FragColor.x * totalDiffuse.x)`, which
keeps all four components in the record, matches llvmpipe to within the
noise of the measure (mean |R-G| 20.44 against 21.35, |G-B| 8.18 against
8.20, in one frame so there is no animation phase to explain it away).

What the offline dumps say about that pair of passes, and what they do not:
the producing pass of the `.z` probe reads the zw halves of its three texel
quads with the identity swizzle and writes `MOV_OUT` from one value into
both `0x11` and `0x13`, so by the compiler's model the record holds
`(z, w, z, w)`; the consuming pass reads texel register 4 with the
replicate-first-scalar swizzle, which by the same model is `z`.  The `.x`
probe's producing pass differs only in that its value is a broadcast, so
every dword of its record is the same and no substitution could show.  The
model and the hardware disagree and this tree does not settle which half is
wrong.

**What would settle it** is a capture of a vendor two-pass program whose
producing pass writes a record with four *different* dwords and whose
consumer reads more than the first -- `m9_fs_corpus.h` has no such pair --
or a fixture that writes a record of four traceable values and reads each
back in turn, which is the cheaper of the two and is the next step.  Until
then the reading "the consumer reads the record's dword 2 where the producer
put it" is UNTESTED, and only the input/output behaviour above is MEASURED.

### 8.4d The blue cast: record flags freed unit 2's texel pair (MEASURED, 2026-10-09)

8.4c's reading, that the consumer gets dword 1 where dword 2 belongs, does
not hold on main e75ec3b. Probes that write each pixel's own position into
the record (`p-pos01`, `p-pos23` in
`captures/m9x-terrain-record-probes-20261009`) come back exact in all four
dwords. Probes that carry terrain's texture mix lose dwords 2 and 3, and the
alpha with them, on about 13000 of 35190 pixels. The fault is in the
producer, not in the record transport.

terrain's producer binds three units, and its index 88 was `0070036f`. The
record flags `0x0c << units` set bits 5 and 6. Bit 5 is pair `0a/0b`, unit
2's zw texel register, so the engine reused it. With only bit 5 cleared
(`M9X_R88_AND=0xffffffdf`), the record z/w products drop to 137-259 pixels
(rounding) and alpha to 0. The `p-pos` controls, which never read a texel
z/w, do not change. Mesa m9x f3769f1 applies the record flags before the
texel pairs are marked live, so a record producer never frees a texel pair a
bound unit occupies; 2-unit producers keep `0330033f`. The split fixtures
(tex5, coordconv, conv3/7/10/13/16, texhi:3:x/4:x) stay exact, the piglit FBO
set and core are unchanged, and glmark2 terrain loses its blue cast: frame
means 118/133/44 against llvmpipe's 125/125/42, where it was 118/133/231
(captured kernel-side with `m9-scanout-grab`).

No capture holds a record producer with more than one unit, so the flag rule
beyond one unit is still extrapolated. What is measured is that the record is
written correctly without the flag on a live texel pair.

**Open.** `p-tda` (totalDiffuse plus the uniform ambient product, consumed in
the third pass) reads totalDiffuse as zero, or as part of it, on 5395 of
35190 pixels in a per-pixel speckle. totalDiffuse alone (`p-tdv`) and the
ambient product alone are both exact. This does not show at frame scale in
glmark2 terrain.

### 8.5 The defect `coordconv:4` isolated: index 88's two fields overlap

`tools/m9-mesa-glsl.c` gains **`coordconv:N`**: N taps at whole-texel offsets
around a base coordinate the fragment stage computes, so every tap takes a
coordinate route.  It is glmark2's tilt-shift blur in fixture form, and it is
checked against a CPU reference pixel by pixel.  `coordconv:4` was the first
of the family to fail: **54092 of 76800 pixels** (MEASURED 2026-09-19).

**The cause is index 88, which carries two fields over one range of bits.**
Bits 15:4 flag the register pairs `08/09`..`1e/1f` the engine may reuse, and
a record-writing pass sets two more flags one place higher for every unit it
binds -- `0x0c << units`, which section 3 derives from the two captured
producers (zero units, `0110fffc`, and one, `0110dffb`).  **From two units up
those record flags land inside the pair field**, and they are OR-ed in after
`registers_used` has cleared the pairs the program is actually holding values
in.  `coordconv:4`'s producing pass binds two units and carried both taps'
blue through registers `08` and `0a`; its 88 was `0030fb3f`, bits 4 and 5
set, declaring both of those pairs free.  That is the same fault `texhi:4:0`
measures for an ordinary pass.

The failure names itself precisely, which is what settled it:

| | |
|---|---|
| channels wrong | **blue only** -- 54092 pixels, R wrong on 150 (rounding), G and A exact |
| sign | always short, by 3..11 of 255 |
| fitted error | exactly `w * (red - blue)` of the **one tap the two-unit producing pass weights at 0.5** |

Red and green being exact means every tap sampled the right texel, so the
coordinate routes were all correct and only a value in flight was lost.  Blue
is component 2, which lives in the high register of a pair; red is component
0, in the low one.

**The fix is in the allocator, not the encoder**: a producing pass keeps no
live value in the pairs its own 88 marks free (`record_reserved`), rather
than the encoder dropping a flag both captures carry.  Which of the two
readings is right -- the flags shift and the pairs are therefore unusable, or
the shift rule is wrong beyond one unit -- is **not settled by this tree**;
what settles it is a capture of a vendor producing pass that binds two or
more units, and none exists.

A naive "a flagged pair is clobbered" is not the whole story either: a
**four-unit** producer's 88 is `00f003cf`, bits 6 and 7 set, which flags the
pairs `0c/0d` and `0e/0f` -- where texel slot 3 lives -- and `conv7`,
`conv10` and `conv16` all render exactly (MEASURED).  Only the two-unit case
is measured to matter.  The reservation is kept because it is measured to
close the defect and to cost nothing in 228 fixtures, not because the field
is decoded.

MEASURED after the change, one process a case, no stream timeout, a
known-good fixture passing after each:

```text
coordchain     0 of 76800 pixels differ: PASS   4 passes, chained routes
coordconv:2    0 of 76800 pixels differ: PASS   3 passes, 2 record slots
coordconv:3    0 of 76800 pixels differ: PASS   4 passes, 3 record slots
coordconv:4    0 of 76800 pixels differ: PASS   6 passes, 5 record slots
coordconv:5    0 of 76800 pixels differ: PASS
coordconv:6    0 of 76800 pixels differ: PASS
conv7/10/13/16 0 of 76800 pixels differ: PASS
```

`coordconv:4`, `:5`, `:6` and `coordconv:4d` are now listed in the suite.

**Two readings this retires.**  The 2026-09-19 note offered "`coordconv:4` is
the first schedule in which a pass that writes a record also consumes a
coordinate route, and the first in which one pass mixes a value route with
coordinate routes" as the open lead.  Both are wrong: `coordchain`'s own
pass 1 does exactly that -- `record 1, routes -1 0, coord 0 -1`, a producing
pass with a coordinate route on unit 0 and a value route on unit 1 -- and
`coordchain` passes (MEASURED, `M9X_SPLIT_TRACE=2` names the driver's own
schedule).  The same note suggested this also explained `refract`'s black
frame; that was found independently to be the record ceiling (`98be636`) and
is fixed.

### 8.6 Reproduction

```sh
# the schedule a shader needs, with its record slots; M9_FS_SCALAR=1 hands
# the backend the scalarised NIR the driver gets, M9_FS_PROGRAM=1 prints
# each pass's slot words and M9_FS_CONSTS=1 its constants, which is how the
# tap a pass carries is named
M9_FS_SCALAR=1 M9_FS_SPLIT=1 M9_FS_PROGRAM=1 M9_FS_CONSTS=1 \
        objs-m9x-llvm/src/gallium/drivers/m9x/m9-fs-compile-test \
        --glsl terrain.vert terrain.frag

# the fixture on the target, inside the lock; M9X_SPLIT_TRACE=2 names the
# schedule the driver itself built, which is not always the offline one
flock -w 600 /root/tmp/m9-hw.lock env \
        LD_LIBRARY_PATH=$P/lib:$P/dri LIBGL_DRIVERS_PATH=$P/dri \
        MESA_LOADER_DRIVER_OVERRIDE=m9x M9X_SPLIT_TRACE=2 \
        M9_GLSL_RAW=/root/tmp/cc4.raw $P/src/m9-mesa-glsl coordconv:4
```

`M9_GLSL_RAW` keeps the readback, bottom row first as RGBA, so the error can
be fitted against the CPU reference off the target -- which is what named the
channel, the sign and the single tap.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
