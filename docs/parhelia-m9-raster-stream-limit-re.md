<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 raster streams: how many interpolated inputs one pass really carries

The clean-room driver states its interpolation limit once, in
`m9_raster_fs_layout_ok()`: six stream slots, three of them reserved for the
rasterizer position, the clip code and the clip position, and an eight-word
declaration.  Three interpolated inputs are left, which is what refuses
glmark2's `jellyfish`, `bump bump-render=height` and `ideas`.

This note establishes what the engine actually carries, decodes the vendor's
multi-pass contract for the shaders that need more, and records the hardware
run that settles it.

Labels: **MEASURED** is a result read back from the colour surface and
compared against a CPU reference for every pixel, through
`tools/m9-user-raster-streams.c` — sections 4 and 5 on an M9148
(`102b:0540`) on September 17, 2026, section 6 on an M9128 on September 19;
**CAPTURED** is read
from the vendor command streams in `captures/m9x-vendor-glsl-programs-20260915`;
**DERIVED** is inferred from those; **UNTESTED** is named where it is.

## Result

**Six interpolated inputs per raster pass, not three — and six is the real
ceiling.**  MEASURED: a single raster pass with six four-component inputs, nine
stream slots (`00..08`), five packed stride words (`10..14`) and a
fifteen-word declaration renders all three probe triangles with 100 % of
25518 interior pixels matching the CPU reference within one count, for each of
the six inputs in turn, with no input aliasing another.

Three was a limit of the driver's own constants and of the kernel validator's
index whitelist.  Nothing about it was a hardware property.

The ceiling above six is **the fragment unit's interpolation registers**, and
not the declaration-word count field of index `22` as this note first
concluded — section 6 measures both halves of that.  Six is also exactly where
the vendor's own compiler stops: `vs-varyings-max`, a shader with fourteen
varyings, is split into **three raster passes** carrying 2, 6 and 6 inputs
(CAPTURED).

## 1. What a vendor raster pass binds (CAPTURED)

Every raster pass names its streams as indexed slots `00..`, their strides
packed two per word from index `10..`, and its attribute layout as index `23`
words counted by index `22` bits 11:8.

| case | inputs | slots used | stride words | decl words (`22`) |
|---|---:|---|---|---:|
| driver's first-triangle stream | 1 | `00..03` | `10, 11` | 5 (`0x6403`) |
| `pr-tex-mul-color` | 1 + 1 texture | `00..04` | `10..12` | 6 (`0x2503`) |
| `vs-varyings-2` | 2 | `00..04` | `10..12` | 7 (`0x2603`) |
| `vs-varyings-4` | 4 | `00..06` | `10..13` | 11 (`0x2a03`) |
| `vs-varyings-max` pass 2 and 3 | 6 | `00..08` | `10..14` | 15 (`0x2e03`) |

`vs-varyings-4`'s raster pass, verbatim from the capture:

```text
00=113e8000 01=113db000 10=00100010
02=113ce000 03=113c1000 11=00100010
04=11402000 05=1140f000 12=00060010
06=113f5000 07=00000000 13=00000010
22=00002a03
23=00100d88 23=001006b6   attr 1  stream 0
23=00210d88 23=002106b6   attr 2  stream 1
23=00320d88 23=003206b6   attr 3  stream 2
23=00430d88 23=004306b6   attr 4  stream 3
23=00040688             attr 0  stream 4   rasterizer position
23=00c531b6             attr 12 stream 5   clip code, stride 6
23=00d60688             attr 13 stream 6   clip position
```

So the driver's `M9_RASTER_MAX_STREAMS = 6` and `M9_RASTER_MAX_DECL = 8` are
both under-counts of what the vendor emits, by four slots and seven words.

### One declaration word or two

An interpolated input costs two declaration words (`0d88` for `x,y` and
`06b6` for `z,w`) and the rasterizer position, the clip position, the clip
code and a sampler coordinate cost one each.  That per-word cost, not the
attribute numbering, is what the 16-word budget spends; whether a general
interpolant may also use the one-word `0688` form -- which would take this
ceiling from six to thirteen -- is untested and is experiment 3 of
`docs/parhelia-m9-varying-transport-re.md` section 8.

### The attribute numbers are a fixed functional space

CAPTURED across the corpus: attribute 0 is the rasterizer position, 5..8 are
the four texture coordinates (one declaration word each, form `0688`), 12 is
the clip code (`31b6`, a six-byte stream) and 13 is the clip position.  The
interpolated inputs take 1..4 and then **9, 10** — `vs-varyings-max`'s
six-input passes declare attributes `1 2 3 4 9 10`, skipping the texture
coordinate block.  MEASURED here for all six.

The fragment register group is **not** the attribute number: it is the index of
the input among the declared four-component inputs, in declaration order.
`vs-varyings-max`'s six-input fragment program interpolates from
`0x40 + 8k` for `k = 0..5` while declaring attributes 1,2,3,4,9,10 (CAPTURED),
and the probe confirms the same dense numbering (MEASURED).  This is what
`m9_fs_compile.c` already assumes, and why `M9_FS_MAX_VARYINGS` is 6.

### Why the driver's `m9_raster_fs_init_textured()` would have collided

It numbers input `k` as attribute `k + 1`, so a fifth input would have claimed
attribute 5 — texture unit 0's coordinate.  The correct map is `k + 1` for
`k < 4` and `k + 5` after it.

## 2. The kernel validator was the hard stop

`m9_index_allowed()` accepted stream slots `0x00..0x05` and strides
`0x10..0x12` only, and `m9_index_reloc_kinds()` described the same range, so
a job naming slot `0x06` was refused with `-EPERM` before it reached the
engine.  MEASURED baseline, unmodified module: `n=1..3` draw, `n=4..6`
return `Operation not permitted`.

Widening both to `0x00..0x09` and `0x10..0x14` — the captured maximum — is the
whole kernel change.

## 3. The multi-pass contract, for the record (CAPTURED)

The driver does not need this (it runs transform and lighting on the CPU and
writes its varyings straight into stream views), but it is what the vendor uses
when one pass cannot hold a shader, and it was undecoded until now.

**A pass writes at most four vector targets plus the clip code.**  Output
register pair `(0x11 + 4j, 0x13 + 4j)` goes to the `j`-th bound target,
`j = 0..3` being `b0, b2, b4, b6`; register `0x21` goes to `b8`.
`vs-varyings-4`'s second preprocessing pass writes `r11/r13, r15/r17, r19/r1b,
r1d/r1f` with `b0,b2,b4,b6` bound.

**Intermediate results are one packed record.**  The target descriptor
`b1/b3/b5/b7` names a form in bits 20:19 (1 = a two-dword field, 3 = a
four-dword one), the field's **dword offset** in bits 15:8 and the record's
**dword stride** in bits 7:0.  `vs-varyings-4` packs five four-dword fields
into twenty dwords (`ee190014 ee190414 ee190814 ee190c14 ee191014`),
`vs-varyings-2` three into twelve (`ee19000c ee19040c ee19080c`) and
`vs-varyings-max` fifteen into sixty (`...3c`), so those read as slots — but
the fields are not slots.  `cap-attribs_14` packs `ee19020a ee19060a
ee09000a`, a two-dword field at dword 0 and two four-dword fields at dwords 2
and 6 of a ten-dword record, which no slot rule produces.  See
`docs/parhelia-m9-varying-transport-re.md` section 3.

**A later pass binds up to four of those slots as sources and sees them as
low-numbered registers.**  Binding `k` is index `0x74 + k` with `0x78` the
arena base and `0x79` the descriptor of slot *s*; the `0x74 + k` word is
`0x0c | (4s + 1) << 8 | (4s + 2) << 16 | (4s + 3) << 24`.  The program then
reads binding `k` as the register pair `(0x00 + 4k, 0x02 + 4k)`.
`vs-varyings-4`'s third pass binds slots 1..4 and its nine slots are exactly

```text
r11 = mov r00   r13 = mov r02   r15 = mov r04   r17 = mov r06
r19 = mov r08   r1b = mov r0a   r1d = mov r0c   r1f = mov r0e   stop
```

a pure gather of four inputs into four targets.  A later pass carries no
source record declaration: its index `22` is `0x...000e` and its only index
`23` word is the `00004b08` terminator.

**The raster half splits the same way.**  `vs-varyings-max` issues three raster
draws: the first two write `b0` into a two-slot arena (`ee190408`, then
`ee190008`) and the third writes the colour surface, each later pass binding
the previous one's slot through `0x74`/`0x78`/`0x79` exactly as above.  The
fragment programs are three separate entries at `c3` = 0x00, 0x09 and 0x22.

## 4. The hardware measurement

`tools/m9-user-raster-streams.c` rebuilds the verified two-pass stream packet
by packet, replaces the raster pass's stream, stride and declaration packets
with `N` interpolated inputs, and binds each input to its own CPU-written view
in a system-memory buffer — the address class the driver already uses for
varyings the draw module produces.  The fragment program is the two-slot
interpolation the driver's own backend emits, returning input `k`:

```text
slot 0   a = 0x42 + 8k   b = 0x79   c = 0x45 + 8k   d = 0x2d   op0 = 0x1e
slot 1   a = 0x40 + 8k   b = 0x79   c = 0x44 + 8k   d = 0x2d   op0 = 0x1d
slot 2,3 stop
```

Each view carries a value that identifies both the view and the vertex, so a
mis-wired slot reads as a different input rather than as noise, and the probe
counts those aliases separately.  Every pixel at least one pixel inside a
triangle is compared against the barycentric mix of that triangle's three
corner values.

```text
staged module /root/tmp/re-m9x_drm.ko, 2026-09-17
n=1 read=0 PASS inside 25518 match 25518 (100.0%) worst 1 written 26838 outside 0
...
n=6 read=5 PASS inside 25518 match 25518 (100.0%) worst 1 written 26718 outside 0
raster-streams: 21 PASS 0 FAIL
```

All 21 (`n`, `k`) combinations for `n = 1..6` pass, no run reported an alias,
and no run wrote a pixel outside a predicted triangle.

Two things the probe had to get right, and both cost a debugging round:

* **The four-slot marker.**  `finish_slots()` pads a fragment program to at
  least four slots and sets bit 30 of the `W1` of slot `n - 4`.  Without it the
  program returns a constant `(0, 1, 0, 1)` instead of the interpolation —
  it does not fault, it silently produces the wrong colour.
* **The placeholder texture.**  Index `88` bit 20 is the unary texture-unit
  count and the driver sets it even with no texture; setting it without binding
  the opaque-black 1x1 unit-0 texture **stops the engine** (`-ETIMEDOUT`, then
  `-EIO` on every later submit, recovered by a reboot).

### Gates

No Mesa file was changed, and `m9-fs-compile-test` is unchanged at 182 corpus
shaders, 152 supported, required exact 23/23.  For the kernel change,
`/root/parhelia-cleanroom/raster/m9-user-raster-state` was run once with the
shipping module and once with the widened one: 799 lines of output, 46 PASS
and 12 FAIL both times, byte-identical (the twelve are the suite's
pre-existing mixed-link exploration cases).  The widening only admits index
values that were refused before; it changes nothing that already ran.

## 5. What the driver has to change

Nothing in the hardware; six constants, one loop bound, one attribute map and
one kernel whitelist.

1. `m9_submit.c`: `m9_index_allowed()` and `m9_index_reloc_kinds()` from
   `0x00..0x05` / `0x10..0x12` to `0x00..0x09` / `0x10..0x14`.  Done.
2. `m9_draw_params.h`: `M9_RASTER_MAX_STREAMS` 6 -> 10,
   `M9_RASTER_MAX_DECL` 8 -> 16, `M9_TRANSIENT_VIEWS` 7 -> 11 (four views for
   the preprocessing pass plus the seven CPU views the stream limit allows).
3. `m9_draw_params.c`: `m9_raster_fs_layout_ok()` keeps its shape — the two
   inequalities are already the right ones, only their bounds were wrong — and
   gains `varying_count <= M9_FS_MAX_VARYINGS`.  Input `k` is declared as
   attribute `k < 4 ? k + 1 : k + 5`.
4. `m9_draw.c`: the stream loop `for (pair = 0; pair < 3 && ...)` becomes
   `pair < (fs->stream_count + 1) / 2`; it already emits the two-value packet
   for an odd stream count, which the probe confirms the engine accepts.
5. `m9_fs_compile.c`: `varying_slot()`'s `varying_count + texture_count >= 3`
   becomes the same predicate the raster layout uses, so the compiler and the
   emitter agree in one place.
6. `m9_context.c`: the transient-view gate follows `M9_TRANSIENT_VIEWS`, and
   the local `views[]`/`flat[]` arrays are already `M9_FS_MAX_VARYINGS` wide.

What stays refused: seven or more interpolated inputs in one pass, which
section 6 MEASURES as an engine limit rather than an encoding one, and which
the vendor answers with the multi-pass split of section 3.  Since the driver
emits the one-word declaration the budget reads `varying + texture + 3 <= 16`,
which no six-input layout reaches, so only the ten stream slots and the six
interpolation registers bind.

## 6. One declaration word an input, and what six really is (MEASURED 2026-09-19)

Run on the dual-DisplayPort M9128 at 192.168.1.111 with
`tools/m9-user-raster-streams.c`, every interior pixel against the same CPU
reference, one process a step under the hardware lock.

### A non-zero first selector reads a pair out of another attribute's stream
(MEASURED 2026-09-19)

The same twelve bits answer the other question they raise.  A texture
coordinate attribute declared over **the stream slot an interpolated varying
already uses**, with selectors `c | (c + 1) << 3 | 6 << 6 | 6 << 9`, reads
that varying's components `c` and `c + 1` as its `s` and `t` and leaves `r`
and `q` unwritten.  MEASURED through the m9x driver on the M9128
(`M9X_COORD_SHARE=1`): `texture`, `texpair` -- the sampled pair at components
1 and 2 of an interpolated varying, which is what glmark2's texture scene
links -- `vary2tex`, `vary2blend`, `rtt300` and `texaff` are each **0 of
76800 pixels different** against their CPU reference, exactly as they are
with a coordinate stream of its own, with no stream timeout either way.

That is what lets a preprocessing pass carry a textured draw on **one**
output target: the coordinate rides the varying's stream instead of taking
the clip position's target.  It applies only where the coordinate needs no
scale or bias, because a shared stream carries the varying's own values.

### The single-word form carries a general interpolant

The low twelve bits of a declaration word are four three-bit component
selectors, so the captured pair `0x0d88` + `0x06b6` reads `x,y` and then
`-,-,z,w`, and the single word `0x0688` attributes 0, 5, 6 and 13 use reads
`x,y,z,w` outright.  No captured pass ever declares a general interpolant that
way.  The hardware accepts it:

| declaration form | inputs swept | index 22 at six inputs | result |
|---|---|---|---|
| `0d88` + `06b6` a pair | 1..6 | `0x2e03`, fifteen words | 21 of 21 exact |
| `0688`, one word | 1..6 | `0x2803`, nine words | 21 of 21 exact |

`M9RS_STREAM` dumps of both forms confirm the difference reached the engine:
the one-word pass declares `00100688 00210688 00320688 00430688 00940688
00a50688` for attributes 1, 2, 3, 4, 9 and 10 against streams 0..5, then the
fixed trio, where the pair form spends two words on each.
The whole `m9-mesa-glsl` fixture suite is **211 of 211** with the driver
emitting it.

So six inputs cost nine declaration words rather than fifteen and the
sixteen-word budget stops binding: a pass can carry six interpolants beside
all four texture units.

### A seventh input has no register

With seven inputs declared the seventh never reads its stream, whatever
attribute number the four-bit field is given — 11, 14, 15 and 5 were each
tried and each read nothing that matches any bound view.  The first six stay
exact.  Reordering the attributes says why: declaring attributes
`1,2,3,4,9,11,10` against streams 0..6 made the program's read of input 5
return **stream 6**, and `1,2,3,4,10,11,9` made it return stream 4 — the
interpolation registers `0x40 + 8 * i` are handed out in **ascending attribute
order** among the declared inputs, and there are exactly six of them.  The
seventh register, `0x70`, reads garbage.

The failure is quiet: the pass rasterizes, writes every pixel it should, and
returns no error; only the values are wrong.  No stream timeout, no `EBUSY`.

### Index 88 bits 31:24 are not eight interpolant flags

`m9_fs_raster_88()` sets one bit from 24 up an interpolated input.  With a
seventh input and bit 30 set, **input 4** also stopped reading its stream;
with all eight bits set every input did.  Forcing the six-input mask
`0x3f10dff0` on a seven-input pass left the first six exact and only the
seventh wrong, which is how the register-file limit above was separated from
this one.  The field holds six inputs and no more.

## 7. The fixed trio is really a pair (MEASURED 2026-09-19)

Every captured raster pass declares three attributes beside its interpolants
-- 0 the rasterizer position, 12 the clip code at stride 6, 13 the clip
position -- and 556 of 556 carry all three.  Two of them are optional as far
as the pixels go: dropping **either** attribute 12 or attribute 13 from the
declaration and its stream renders the same image, MEASURED on the M9128 with
the `varying` fixture through the m9x driver, the known-good fixture passing
before and after.

That matters because a preprocessing pass has four output targets and its
appended tail spends three of them.  Dropping attribute 13 frees output
target 1 -- the clip position's -- for a second varying, which is how the
driver now carries two of them in one draw
(`docs/parhelia-m9-vertex-output-target-re.md`).  Attribute 12 would free
nothing: the clip code rides index `b8`, not one of the four targets, and a
pass that enables output target 3 instead stalls the engine in every shape
tried.

The measurement covers geometry inside the clip volume only; what the clip
position buys for geometry that needs clipping is **OPEN**, and the
`vary3big` fixture is the case that asks it.

## Reproduction

```sh
# host: the captured passes, their programs and their constant spans
python3 tools/m9-vertex-program-extract.py \
    captures/m9x-vendor-glsl-programs-20260915/vs-varyings-4
python3 tools/m9-glsl-program-extract.py \
    captures/m9x-vendor-glsl-programs-20260915/vs-varyings-max

# target, next to m9-user-triangle-data.h, staged module, inside the lock
gcc -O2 -Wall -I/root/parhelia-cleanroom/include -o m9-user-raster-streams \
    m9-user-raster-streams.c -lm
flock -w 1800 /root/tmp/m9-hw.lock ./m9-user-raster-streams 1 6
```

The probe takes `first` and `last` input counts, and reads
`M9RS_DUMP`, `M9RS_STREAM`, `M9RS_VIEW0_TRANSIENT`, `M9RS_NO_TEXTURE`,
`M9RS_CLS`, `M9RS_R88`, `M9RS_ONEWORD` and `M9RS_ATTRS` for bisecting against
the captured configuration.
