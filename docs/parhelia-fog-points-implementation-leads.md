# Fog and hardware point size: what the evidence says, before any code

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Written while implementing the fixed-function features the hardware has and
the driver does not.  Logic ops landed the same day; these two did not, and
this is the state they were left in so the next attempt starts from evidence
rather than from the register map's plausible-looking rows.

## Fog

**The hardware has a fog unit.**  `0x1070` is a fog enable (0/1, CONFIRMED in
`parhelia-fragment-ops.md` section 2), the WARP output convention gives
`o10.w` as the fog factor (`parhelia-p690-warp-isa-re.md` section 3.2), and
the vendor uploads fog parameters as constant blocks - index `0x14e` carries
`{1/(end-start), -start/(end-start)}` for linear fog and `0x154` carries
`{start, end/(end-start), 1/(end-start), density*log2e}`, both CONFIRMED in
`parhelia-block-state-map.md`.

**Mesa does not ask for any of it.**  Measured with `PARHELIA_TRACE_FS=1` on a
`glEnable(GL_FOG)` draw: the state tracker lowers fog entirely into the
*fragment* shader, which arrives as `fmul, fadd, fsat, fneg, fadd, fmul, fmul,
fadd, vec4` - the linear factor followed by a mix toward the fog colour.  The
vertex shader writes position only.  So the hardware fog unit is never
reached, and a fogged draw renders unfogged: measured `ffffff` where the mix
wanted `ff0000`.

Every operation in that expression is already supported by the fragment
data-flow compiler (`fsat`, `flrp`, `ffma`, `fmul`, `fadd` - see
`parhelia_fs_compile.c`).  Two things still stop it:

1. the compiled combiner is only bound on the *textured* draw path, so an
   untextured fogged quad never uses it at all;
2. the combiner's inputs are the texture samples `t0..t3` and the vertex
   colours `v0`/`v1`, and the lowered expression needs the interpolated fog
   coordinate.  The hardware has a second colour (`o2`, colour 1, BIN-only in
   the ISA notes) which is the natural carrier.

So the shape of the work is: map the vertex fog output to `o2`, let the
fragment compiler read it as `v1`, and bind the compiled combiner for
untextured draws.  That is ordinary driver work with no unknown registers -
unlike the alternative of driving the hardware fog unit, which additionally
needs the fog *colour* register, and that one is **not identified**:
`parhelia-fragment-ops.md` section 11.3 warns that the ICD's bank-C indices
0x22-0x27 overlap registers MTXPARD treats as DMA control and trap
(`0x108c`, `0x1090`, `0x109c`), and the `fog` capture confirms the warning -
`0x1090` carries addresses there, exactly as it does in the baseline capture.
Do not write that range on a guess.

## Hardware point size

`captures/gl-matrix/points-secs.txt` writes `0x10fc = 0x42400000`, which is
**48.0f**, in a capture whose scene draws points; the baseline capture never
writes that register.  That is a good point-size candidate and it is the first
thing to confirm.

The caution is that this driver already writes `0x10fc` with `0.2f`
(`P3D_R_TEX_UNK10FC`, one of "the three registers only a same-boot diff could
reveal" in `p3d_state.c`), and the same capture also writes `-8160.0f` and
`-0.5f` there.  A register carrying 0.2, 48.0, -0.5 and -8160.0 in one
capture is either windowed by another selector or is not one register's worth
of meaning.  Resolve that before wiring `pipe_rasterizer_state::point_size`
to it: the cheap experiment is a `parhelia-drm-test` probe that draws one
point at several sizes and reads the covered pixels back, with everything else fixed.

Today the driver expands every point larger than a pixel into two triangles in
Gallium's draw module, which is correct and costs CPU.

---

## Progress, 2026-09-13

**Point size is done** — see `parhelia-point-size-re.md` sections 10-12. The
account in this document's point-size section is superseded: `0x10fc` was a
decoder desync, the extent is a bounding box in `o9`, and the selector is
`0x1130` class 3 with the WARP class left at the primitive's own.

**Logic ops and line stipple are now actually verified.** They were reported
as tested before, but the target's copy of `parhelia-gl-q3formats-test` dates
from 2026-09-12 12:22 and contains no fog, logic-op or stipple case at all -
`strings` on it finds none of them. Rebuilt from the tree on the target, all
eight logic ops pass (COPY, CLEAR, SET, NOOP, INVERT, XOR, AND, OR) and the
line stipple case passes. **Rebuild the GL tools on the target before trusting
a result about a recently added test.**

**Fog: the fragment side now compiles, the carrier does not work yet.**

The diagnosis in this document was right - Mesa lowers fog into the fragment
shader and the combiner never saw it - and two real compiler limits were in
the way. Both are fixed and both are regression-free (the RGB 64x64 subset and
`parhelia-gl-prim-test` are unchanged):

* **A replicated source could not be replicated again.** A scalar in a
  register, consumed twice in an rgb context, was rejected with "rgb operand
  already replicated". A replicated source already holds one channel in all
  four, so selecting a channel of it again is the identity; it is now accepted.
* **`a*c + b*(1-c)` is the combiner's LRP**, which is exactly `P3D_COMB_LRP`,
  but Mesa lowers `flrp` into two multiplies and an add, so the op was never
  reached. `ph_fs_try_lrp()` matches the lowered form and its commutations.
  Without it the fog shader needed six combiner slots against a budget of
  five; with it, it compiles.

What remains is the carrier. The fog coordinate has to reach the combiner's
`v1`, and nothing supplied it: the vertex stream had no second colour and the
pass-through WARP program does not write `o2`. Both are now written -
attribute kind 6 at three dwords (`0x21c50020` + `0xa1c60050` untextured,
`0x21c50030` + `0xa1c60060` with depth), `ph_colour1_program()` uploading
pass-through plus `mov o2, v6`, `ph_fs_program::uses_color1` telling the draw
path when it is needed, and `p3d_state::vertex_colour1` declaring it - but a
draw that uses them leaves **the following draws rendering the previous
colour**: with the carrier on, the whole logic-op group degrades to `ff00ff`
and four `GL_TEXTURE_BASE_LEVEL` cases fail. The fogged draw itself still
renders unfogged white, so the coordinate is not arriving either.

It is therefore **behind `PARHELIA_FOG_COLOUR1=1` and off by default**, so the
tree stays at its clean baseline. The next attempt starts here: find why a
colour1 draw corrupts the state of the draws after it - the stream-shape flush
guard and `ctx->colour1` never being cleared after a submit are the first two
things to check - and confirm with `PARHELIA_TRACE_FS=1` that the compiled
combiner is actually bound on that draw rather than the fixed-function TEXENV
path.

Driving the hardware fog unit instead still needs the fog colour register,
which is still unidentified; nothing here changes that.

### The combiner's v1 is not fed by any WARP output (measured 2026-09-13)

`parhelia-drm-test --colour1-probe` removes the GL path entirely: a triangle
with colour 0 red and a second colour attribute green, a one-instruction
combiner `MOV r0, <src>`, and the kernel pass-through program plus one
secondary `mov o<N>, v<M>`.

| what | result |
|---|---|
| combiner reads v0 | `ff0000`, correct - the program runs and the 8-dword stream still delivers colour 0 |
| combiner reads v1 | `ff5600` |
| v1, second attribute absent | `ff5600` |
| v1, move writes o2 from v6 | `ff5600` |
| v1, move writes o2 from v5 (known-good red) | `ff5600` |
| v1, move destination swept over **every** output o1..o11 | `ff5600` at all eleven |
| v1, with `0x1074` (separate specular colour) 0 and 1 | `ff5600` both |

So v1 reads an undefined constant and **no vertex output reaches it** in this
configuration, while v0 through the same combiner program is correct. Either
an enable we have not found gates the second interpolator, or v1 is not fed
from the vertex side at all on this part. `0x1074` alone does nothing.

That is why the GL-side carrier is gated off: the plumbing is written and the
vertex attribute is fetched (colour 0 still arrives on the wider stream), but
the value cannot reach the fragment stage yet. The descriptor dword for kind 6
is still inferred, not measured - though colour 0 continuing to arrive on the
same stream argues the stream shape is at least accepted.

### The fog unit is the route, and its registers are now identified (2026-09-13)

A second RE pass answered the carrier question, and the answer redirects the
implementation away from `v1` entirely.

**No vendor code path ever emits a combiner instruction that reads `v1`.**
`parhelia-fragment-isa.md` section 12.2.2 already says the fixed-function
generator never emits `v1`, and the recovered program blobs agree. The vendor
gets a second colour to the framebuffer *after* the combiner (colour sum, a
post-combiner add whose register is still unidentified), and it implements
`GL_FOG` with a hardware fog unit. So the `ff5600` constant this probe
measured is what an interpolator with an idle bus looks like, and there is no
positive evidence anywhere that the P690's `v1` works at all.

**The fog unit, decoded byte-exact from the vendor's own `GL_FOG` capture:**

| register | meaning |
|---|---|
| `0x12d4` | `[0]` enable, `[2:1]` mode, `[13:6]` blue, `[22:15]` green, `[31:24]` red, each `(int)(c * 255 + 0.5)` |
| `0x12d8` | fog start, `value * 16777215` |
| `0x12dc` | fog end, `value * 16777215`; `0x00ffffff` is the off state |
| `0x12e0` | a 64-dword fog table port |

The capture that confirms it is `GLTEST_FOG=1` with
`glFogfv(GL_FOG_COLOR, {0.2, 0.3, 0.4, 1.0})`, which writes
`0x12d4 = 0x33269981`: R `0x33` = 51 = 0.2*255, G `0x4d` = 77 = 0.3*255,
B `0x66` = 102 = 0.4*255, enable 1, mode 0 = `GL_LINEAR`. It also sets
`0x1110` bit 5 and uploads constant block `0x154`, and it leaves the
descriptors and **the whole combiner program unchanged** - no combiner slot is
consumed. This confirms `parhelia-pixel-pipeline-complete.md` section 3.6 on
hardware, which until now had only a fog-*off* capture behind it.

The fog factor arrives as WARP output **`o2.w`** through interpolant 5, and
`0x1114` nibble 5 already selects `o2` in the `0x00287210` this driver writes,
so only `0x1110` bit 5 was missing.

**Implemented, and it does not fog yet.** `p3d_state` now carries `fog`,
`fog_mode`, `fog_colour` and `fog_start`/`fog_end`; `p3d_emit_state()` writes
the three registers and ORs `0x20` into `0x1110`. `parhelia-drm-test
--fog-probe` drives it directly, taking the factor from constant `c0.x` so no
vertex attribute is involved, over factors 1.0 down to 0.0 and over fog ends
0.0/0.25/0.5/0.99: **every fragment stays `ffffff`**. Emitting the registers
costs nothing - 53 selftests, the GL subset and `parhelia-gl-prim-test` are
all unchanged - so it is left in place.

Two candidates for why, for the next attempt, in the order I would try them:

1. **The `0x12e0` fog table.** A 64-entry LUT that has never been uploaded
   would plausibly leave the unit at factor 1 for every fragment, which is
   exactly the measurement. The capture should show whether the vendor writes
   it before the first fogged draw.
2. **Whether `o2.w` reaches interpolant 5 at all.** This is the same shape as
   the `v1` result: `o9` reaches the *setup* unit and works (hardware point
   size), but nothing this driver writes to `o2` has yet been observed to
   reach the *fragment* stage. A probe that varies `o2.w` while watching a
   register-level side effect, rather than the final colour, would separate
   "the unit is off" from "the factor never arrives".

### Everything in the fog recipe was tried, and the unit stays silent

Each of these was measured on the P690 with `parhelia-drm-test --fog-probe`,
and in every one the triangle stays `ffffff`:

| tried | result |
|---|---|
| `0x12d4` enable + `GL_LINEAR` mode + red fog colour | no change |
| fog factor in `o2.w` from constant `c0.x`, swept 1.0 down to 0.0 | no change |
| depth-sourced fog: a z-sloped triangle with a real depth surface, near and far sampled separately | near and far identical |
| fog end swept 0.0 / 0.25 / 0.5 / 0.99 (1.0 packs to the `0x00ffffff` off value, so it was never the whole story) | no change |
| the `0x12e0` table port, 64 dwords, all-zero / all-ones / a ramp | no change at any fill |
| `0x1110` bit 5 set in all of the above | no change |

So the documented recipe is complete and the unit does not respond, on
pass-through *and* on transforming draws. Two readings remain, and no probe
here can separate them:

* **The registers are not reaching the unit from our state emission.** They
  are written in the ordinary register list; if the fog block latches from a
  different packet or bank, the writes land nowhere.
* **SUN90 has no fog unit.** Every fog capture in the tree is from the 0x0528
  board. The RE pass that identified these registers found no chip-class
  branch anywhere near them, but it also could not exclude a silicon-level
  absence on the P690 - and the P690 is a cut-down part (two texture pipes
  instead of four). The `v1` result has exactly the same shape: a fragment-
  stage input that never carries anything, while `o9` reaches the *setup* unit
  and drives hardware point size correctly.

**A transforming draw wedges the engine, and the fog registers are not why.**
The one structural idea left - that the `0x160..0x17f` constants are read by
the clip/raster block, which may only be engaged for transforming draws, which
would explain why point-size constants, polygon offset *and* fog are all inert
on pass-through draws - was tested by running the fog probe with the transform
program, an identity-shaped MVP and a viewport. The draw does not retire:
`submit timeout at CALL 37/37 ... wedged`.

Running the identical draw with `FOG_DISABLE=1`, i.e. the fog registers off,
**wedges in exactly the same way**, so the defect was this probe's transform
state and not the fog registers. Its transform path pairs `transform = 1` with
a depth surface, which the working `--transform` probe does not have.

**The idea is now tested on the working path, and it is refuted.**
`--transform` with `TRANSFORM_FOG=1` tilts its triangle in z from 0.05 to 0.95,
paints it white and turns the fog unit on with a red fog colour and end 0.5.
The draw **retires normally** - no wedge - and the result is
`near (60,40) ffffff, far (40,180) ffffff`: no fog, and no difference between
the near and far ends of the slope. So engaging the clip/raster block does not
make the fog constants live, and the "these constants only work for
transforming draws" theory does not rescue fog, point size or polygon offset.

Each attempt costs a reboot and a module reload; see the target-recovery
notes (`reboot` over ssh does nothing on this target, sysrq does, and
`/lib/modules` holds no `parhelia.ko`).

The 0x0528 board would settle the silicon question in one run; it was not
available.

## Multiple WARP lights: emitted, and the accumulation is not a sum

`p3d_warp_build()` now takes `shape->lights` and emits one dp3/max/mad group
per light, each adding `max(0, N.L) * colour` to a running sum, with light i
taking its direction from `light_slot + 2i` and its colour from
`material_slot + 2i`. `shape->lights` of 0 or 1 emits the single-light program
**byte for byte** - the builder self-test that compares against the kernel's
`warp_lighting` image still passes, as do all 53 self-tests - so the shipping
path is untouched and this is opt-in.

Structurally it works: `parhelia-drm-test --lighting-probe` shows the
instruction count stepping 16 / 19 / 22 for one, two and three lights, and
with one channel per light the colours come back `ff0000`, `ffff00`, `ffffff`
- each light reaches the fragment it should.

**The magnitudes are wrong, reproducibly: every light after the first
contributes 5/3 of its colour.** Each light gets its own pair of slots, an
identical +Z direction and an identical colour, and the normal is (0,0,1), so
every `N.L` is exactly 1 and n lights must read n times the colour:

| colour | 1 light | 2 lights | 3 lights |
|---|---|---|---|
| 0.125 | `0x20` (32) | `0x55` (85) | `0x8a` (138) |
| 0.25 | `0x40` (64) | `0xaa` (170) | `0xea` (234) |

The first light is exact (0.125 x 255 = 31.9, 0.25 x 255 = 63.8). Every later
light adds 53 at 0.125 and 106 at 0.25, and both are **5/3** of the expected
term: 0.125 x 5/3 x 255 = 53.1, 0.25 x 5/3 x 255 = 106.25. The 0.25 three-light
figure reads 234 where 64 + 106 + 106 = 276 would saturate at 255, so something
clamps or rounds differently near the top of the range as well; the 5/3 factor
on the 0.125 row is exact.

An earlier table here reported a factor of 0.414 from a run that filled every
constant slot with the same data. That run was **wrong**: it forced the +Z
direction only into the first light's slot, so the later lights dotted the
normal against a colour. The numbers above replace it.

Ruled out by measurement, each its own run:

* **Not the accumulator register.** Accumulating in `r4` instead of reading
  `T2` back gives identical numbers.
* **Not the MAD.** Splitting the accumulating MAD into a separate MUL and ADD
  (instruction count 20 / 24, so the split really is emitted) gives identical
  numbers.
* **Not a channel effect.** The same value in all three channels comes back
  the same in all three (`403f40`, and `807f7f` at 0.5), so red is not special;
  the measurements above put every light in the red channel so the terms add
  in one place.
* **Not constant-slot addressing.** With every slot from c5 to c15 filled with
  the same data, every choice of `light_slot` gives the same exact answer -
  including c12..c15, which a sparse layout appeared to read as zero. Writing
  into c8 breaks the draw exactly as expected, which is independent proof the
  block lands where intended.

So the constants arrive, the instructions are emitted, and the first light is
exact, but the diffuse terms do not add. That is a WARP behaviour this project
has not seen before and it wants the vendor binaries, not more probes: the
question for the next RE pass is **how the vendor's own multi-light program
accumulates** - whether it uses a different destination file for the running
sum, a different opcode, or a per-light scale it programs deliberately.

Until that is answered `shape->lights` should stay at 1 in anything that
ships.

### Answered: the vendor uses a hardware light accumulator, not a register sum

A second RE pass settled why the MAD chain cannot be made to work - **the
vendor never chains MADs, and has no register running sum at all**. Each light
ends in opcode `0x81`, which encodes no destination; the per-light term is
committed into an implicit accumulator pair, and a read-out instruction pulls
it back at the end. The census over the 575-fragment vendor library is
unambiguous: `0x81` appears 194 times and `0x82` 28 times, and **neither ever
writes either port**, while the read-out opcodes `0x84`/`0x86` (24 each) do.
The same start/accumulate split exists in the clip-plane family (`0x7a` starts,
`0x79` accumulates), so it is the house style of this ISA.

The per-light body, from fragment `0x170` (`mtx1773`, two directional lights):

```
dp3   r4.z, r0, c[L+5]        ; N.H, H = normalise(L + eye)
dp3   r4.y, r0, c[L+1]        ; N.L
mul   r5,   c[L+2], v5        ; light diffuse  x vertex colour
mul   r6,   c[L+3], v5.xyz1   ; light specular x vertex colour
mul   T0,   c[L+4], v5        ; light ambient  x vertex colour
op81  <no destination>  r4.0y00, r5, T0
op81  <no destination>  r4.0yz0, r5.0000, r6
```

`src0` is a vector of scalar factors `(spot, N.L, N.H, attenuation)`; a
directional light zeroes the first and last through the swizzle. Light *i*
takes seven constants at `0xE0 + 7i`: position, direction, diffuse, specular
(`.w` = shininess), ambient, halfway vector, attenuation coefficients. The
read-out is `mul o1, r10, c[0x14E].zzzz` and `mul o2.xyz, r11, c[0x14E].wwww`,
which also resolves what `0x14E` is: a host-computed **over-bright
normalisation**, a power of two chosen from the exponent of the largest light
colour, applied to the light colours and undone here. The maximum is 12
lights, there is no CPU fallback below eight, and **no chip-class branch exists
anywhere in the lighting stack** - HSLPAR and SUN90 run the identical library.

Two consequences worth recording:

* The 5/3 factor has **no vendor explanation**: there is no per-light
  coefficient, no instruction-encoding scale field (the vendor's own field
  patcher enumerates every bit of the bundle and there is no analogue of the
  combiner's `0x126c[19:17]`), and the one global scale is a power of two our
  driver never programs. Our MAD chain is a construct the vendor library never
  uses once, so there is nothing to be wrong *against*.
* A refinement to the measurements above: the increments are 32/53/53 at
  c = 0.125 but 64/**106**/**64** at c = 0.25, so the third term at 0.25 is
  1.0x, not 5/3x. The accumulation stops growing near 234/255 rather than
  saturating at 255, which a uniform 5/3 would not do.

**Tried, and it does not work yet - but the first measurement of it was
invalid.** The kernel validator now accepts `0x81`, `0x82`, `0x84` and `0x86`
(they cannot branch and encode no destination, so the file-3 destination check
is unchanged), and `p3d_warp_build()` gained `shape->accumulator` emitting
`0x82` for the first light, `0x81` for the rest and a read-out.

The original run reported a **constant `0x555455`** regardless of light count,
colour or read-out opcode, and concluded the accumulator was not live at all.
That run swept several configurations inside one boot, so all but the first
executed stale code. Re-measured one configuration per fresh WARP store, with
two lights of 0.25 in one channel:

| lights | result |
|---|---|
| 1 | `000000` |
| 2 | `560000` |

So the accumulator **does** respond - it is not a dead constant - but the
arrangement is still wrong: one light contributes nothing, two contribute
0.337 where 0.5 is wanted. The likely cause is the operand shape. The vendor
passes `src0 = r4.(spot, N.L, N.H, atten)` with the colours **pre-multiplied
into registers** (`r5` diffuse, `T0` ambient, `r6` specular) and issues *two*
`0x81`s per light, one for diffuse+ambient and one for specular; this builder
issues one, with a zero specular and the ambient added separately.

None of this is on the critical path: the ordinary MAD chain accumulates
exactly (1x, 2x, 3x, verified above), so the accumulator is only interesting
if the hardware's two-sided face selection turns out to depend on it.

A masked `dp3` was also tried, since the vendor only ever writes one component
where we write all four: identical numbers, so that is not it either.

So the next attempt should reproduce the vendor's light-block layout and the
inline first light rather than bolt an accumulator onto our program shape.
`shape->lights` stays at 1 in anything that ships; the single-light program is
still byte-identical to the kernel image and all 53 self-tests pass.

## Texture coordinate generation, and a defect it uncovered

**Texgen is implemented and its output is correct.** `p3d_warp_shape` gained
`texgen_mask` and `texgen_slot`: a coordinate set whose bit is set takes s and
t from two plane equations against the object-space position -
`GL_OBJECT_LINEAR`, and eye-linear is the same program with the planes
transformed host-side - instead of copying the fetched attribute. Set i reads
its planes from `texgen_slot + 2i`. The builder self-test checks that a
generated set costs two more instructions than a copied one and that both are
`dp4`s, and all 53 self-tests pass.

One thing had to be learnt on hardware: **the generator must state r and q**.
The fetched path moves all four components, so writing only s and t leaves the
set's r and q holding whatever the register had, and the sampler reads all
four - the first version sampled the wrong texel entirely. The generator now
moves `(0, 0, 0, 1)` into the whole set before the two `dp4`s.

Verified with `--transform-textured-probe TEXGEN=1`: planes chosen to
reproduce the constant coordinate the vertices carry sample **the same texel**
as the fetched path - the magenta hue is exact.

### The defect: an uploaded program's vertex colour is 0.4157 of the kernel's

The texgen run samples the right texel but the colour comes back `6a006a`
where the kernel-program control gives `ff00ff`. Under `GL_MODULATE` the
result is texel x vertex colour, so the vertex colour is arriving as
**106/255 = 0.4157** instead of 1.0.

`TEXGEN=2` is the control that pins it: it uploads the **same program without
generation** - byte for byte the shape the kernel carries - and gives the
identical `6a006a`. So this is nothing to do with texgen. The variable is that
the program was uploaded through `WARP_PROGRAM` rather than being one of the
kernel's built-in entries.

Not the constant group: `const_mask` of 1, 5 and 7 all give `6a006a`, so the
viewport pair and the clip/raster copy at `0x160` are not involved.

0x6a keeps appearing in this file - it is the value the second and third
lights contribute in the multi-light probe, and `ff5600` and `0x555455` in the
`v1` and accumulator probes are the same family. Every one of those probes
used an uploaded program. That makes "an uploaded program does not behave
identically to the kernel's own" a **single suspect behind several open
results**, and the cheapest one to chase next, because the control is now
trivial: build the shape the kernel already carries, upload it, and diff the
image against the kernel entry. The builder reproduces six kernel programs
dword for dword, so the instruction images are known to be identical - which
means the difference is in how an uploaded program is bound or executed, not
in what it says.

## The stale WARP instruction cache invalidated several results above

**The engine serves the code an entry address ran first.** A program uploaded
through `WARP_PROGRAM` to an address that has already executed something runs
the *old* instructions, silently. `parhelia_drm_test.c` already carried a
warning about this ("the engine keeps serving the code an entry ran first"),
and it is now the measured explanation for a family of anomalies in this file.

The isolation is exact. `--transform-textured-probe TEXGEN=3` uploads a
program that is **byte-identical to the kernel's** transform-textured image -
a new self-test proves the builder reproduces it dword for dword - and binds
it with **the same state, the same matrix and the same constant groups** as
the control. The only difference is the entry address:

| | entry | sampled |
|---|---|---|
| kernel program | `0x11f17f03` | `ff00ff` correct |
| uploaded copy, same bytes | `0x11f20203` | `6a006a` |
| uploaded copy, **fresh store** | `0x11f95783` | `ff00ff` correct |

One module reload between the runs is the whole difference.

### What this corrects

* **Multiple lights work.** Re-measured with one module reload per light
  count: at colour 0.125 the readings are `0x20` / `0x40` / `0x60` and at 0.25
  they are `0x40` / `0x80` / `0xc0` - exactly 1x, 2x and 3x, with no ambient.
  The MAD chain accumulates correctly. **The 5/3 factor was never real**: the
  probe uploaded a new program per light count inside one boot, so the second
  and third configurations ran the first one's code. Every conclusion drawn
  from it above - including "our MAD chain is a construct the vendor never
  uses, so there is nothing to be wrong against" - was reasoning about an
  artefact.
* **The combiner's `v1` is alive.** On a fresh store the second colour
  attribute reads back `00ff00`, the green it was given, with colour 0 still
  correct at `ff0000`. The earlier constant `ff5600` at every output slot, and
  the conclusion that "no vendor code path emits `v1`, so it is probably dead
  on SUN90", came from a sweep of eleven destinations inside one boot where
  ten ran stale code.
* **The light accumulator result is unsafe.** The `0x555455` constant came
  from the same kind of sweep and has to be re-measured a configuration at a
  time before anything is concluded about opcodes `0x81`/`0x82`/`0x84`.

### How to measure an uploaded program from now on

**One configuration per fresh WARP store.** `--lighting-probe` takes
`LIGHT_ONLY=n` for exactly this reason. Reload the module between runs:

```sh
rmmod parhelia; modprobe i2c-algo-bit; modprobe drm_dma_helper
insmod /root/tmp/parhelia-stage/.../parhelia.ko
```

A sweep that uploads several programs in one boot measures the first one
repeatedly, and the wrong answer looks entirely plausible - a clean
proportional factor, a constant colour - which is what made this take so long
to catch.

**The GL-side fog carrier still fails, and it is now a narrow defect.** On a
fresh store it renders `ffffff` where the mix wants `ff0000`. What is known to
be right, from `PARHELIA_TRACE_FOG=1` on the failing draw:

```
fog carrier want=1 fs=... valid=1 uses_v1=1 fogc=1 textured=0 entry=0x47d8ad03
```

so the fragment program compiled, it reads `v1`, the draw module exports a fog
coordinate, the forwarding program is uploaded and bound (it is set after the
untextured default at line 3851, not overwritten), the kind-6 attribute is
declared and three dwords per vertex are staged. The compiled combiner is
bound for untextured draws. Uploading the program mid-stage was ruled out as
the cause: flushing staged vertices first changes nothing.

The result being the **unmodified vertex colour** points at the fog factor
evaluating to 1 rather than at a missing carrier - which makes the fragment
constant buffer the first place to look, since the lowered expression takes
`1/(end-start)` and `-start/(end-start)` from a uniform and nothing yet
confirms those reach `c0`/`c1` on the untextured path. The DRM-level
equivalent works: `--colour1-probe` on a fresh store reads the second colour
back as `00ff00` through a one-instruction `MOV v1` combiner, so the
attribute, the `mov o2, v6` program and the `v1` input are all sound.

`PARHELIA_FOG_COLOUR1` therefore stays off, but the remaining question is a
constant-upload one inside our own driver, not a hardware unknown. The fog
*unit* results earlier in this file used single uploads or none at all and are
not affected by the stale-cache correction.

## User clip planes need the clipper program, which the driver does not bind

`c[0x16c]` loaded with a plane that should remove half a triangle
(`(-1, 0, 0, 0)`, and again with `w = 0.5`) changes nothing: the transform
probe covers 18336 pixels with and without it.

That is expected once the rest of the state is read properly. The planes are
consumed by the **kind-1 clipper program**, not by fixed setup:
`p3d_state` already carries `clipper_entry` ("v3.x of the kind-1 load") and
`base_reg_entry`, and `p3d_emit_state()` emits the vendor's kind-1 packet only
when `base_reg_entry` is set - while `parhelia_batch.c` rejects any state that
sets it (`input->base_reg_entry ||` in the validator). So no draw this driver
builds has ever bound a clipper.

The work for `GL_CLIP_PLANE` is therefore: allow `base_reg_entry` in the
validator, bind the kind-1 loader and the clipper entry, load the planes at
`0x16c + slot` **transformed by `inv(view * proj)` transposed and compacted**
(`parhelia-block-state-map.md` section 3.6 gives the exact host-side
arithmetic, CONFIRMED), and only then expect geometry to disappear. Loading
the constants alone proves nothing, which is what this measurement shows.

**And the programs themselves are missing, which is the real blocker.** The
kind-1 base-register program is the vendor's `mtx251` and the clipper is a
kind-6 entry: `parhelia-constant-file-blocker.md` records passing 0 for it
because it is "a kind-6 clipper entry we do not have". Neither image exists in
this tree - the kernel carries six vendor programs and neither of these is one
of them - so clip planes need those images recovered from the vendor binaries
before any driver change is worth making. An earlier note here calling the
work "fully specified" was wrong: the constant layout is specified, the
microcode is not.

The same caution applies to the fog and point-size constants measured inert
earlier in this file: a constant in `0x160..0x17f` is only live if something
reads it, and for several of these ranges the reader is a program the driver
has never bound.

## Positional lights with distance attenuation

`p3d_warp_shape` gained `attenuation` and `atten_slot`. A light then reads a
**position** rather than a direction, and its coefficients
`(k0, k1, k2)` from `atten_slot + i`; the term becomes
`max(0, N.L) / (k0 + k1*d + k2*d*d)`. Per light the generator emits the vector
to the light, `dp3` for `d*d`, `RSQ` for `1/d`, the normalised direction, the
polynomial as two `MAD`s, `SMUL` for its reciprocal (the perspective-divide
opcode doubles as a reciprocal), and one multiply into the clamped `N.L`.

**Verified on hardware** with `--lighting-probe LIGHT_ATTEN=<k2>
LIGHT_DIST=<d>`, one module reload per distance so no measurement runs stale
code.

The control first, `k2 = 0`, which leaves `N.L` alone: brightness *rises* with
distance, `0xaf` / `0xe2` / `0xf1` at d = 1, 2, 3, because the light moves
more nearly overhead. Solving `N.L = d / sqrt(c + d*d)` for the sampled
point's lateral offset gives c = 1.125, 1.094 and 1.08 - the same point from
three independent measurements, which is what says the dot product and the
normalisation are right.

Then `k2 = 1`, so the expected value is `N.L / (1 + c + d*d)` with that c:

| d | predicted | measured |
|---|---|---|
| 1 | 0.221 -> `0x38` | `0x38` |
| 2 | 0.145 -> `0x25` | `0x25` |
| 3 | 0.085 -> `0x16` | `0x16` |

Exact at all three. The spot-angle half of `GL_SPOT_CUTOFF` is not done: the
vendor's `src0.x` factor is the spot term and the light block carries the spot
direction and exponent, but nothing here generates them yet.

## Spot cutoff

`shape->spot` and `spot_slot` add `GL_SPOT_CUTOFF` on top of the positional
light: slot `spot_slot + i` carries the cone axis in xyz and the cosine of the
cutoff in w, and the gate's scale rides in the attenuation slot's unused w.
The cone test is `dot(-L, axis)` against that cosine, turned into a hard 0/1
without a compare instruction the same way the shader compiler lowers one:
subtract, clamp at zero, scale enormously, then `s / (s + 1)` through `SMUL`.

**Verified on hardware**, one module reload per configuration. At distance 2
the sampled point is 1.05 units off the cone axis - a figure measured
independently from the `k2 = 0` attenuation control - so it sits at
`atan(1.05 / 2)` = 27.7 degrees, i.e. `cos = 0.885`. The measured boundary:

| cos cutoff | result |
|---|---|
| 0.00 (90 degrees) | `0xe2`, the same as no spot at all |
| 0.80 | `0xe2` lit |
| 0.87 | `0xe2` lit |
| 0.90 | `000000` dark |
| 0.95 | `000000` dark |
| 0.99 | `000000` dark |

The edge falls between 0.87 and 0.90, bracketing the predicted 0.885, and a
cone wide enough to contain the sample reproduces the unspotted brightness
exactly. The spot *exponent* is not implemented: it needs a power, and this
ISA has no LOG/EXP, so it wants repeated squaring for an integer exponent.

## User clip planes: the program side is done, the hardware does not act on it

A further RE pass corrected two things in the section above, and both
corrections matter:

* **Both programs were already in the tree.** `mtx251` (kind-1 base registers)
  and `mtx3605` (kind-6 clip-and-interpolate) ship as firmware in
  `parhelia_firmware.h` and are bound by `parhelia_setup_commands()` and
  `parhelia_publish_commands()`. The "an entry we do not have" note in
  `parhelia-constant-file-blocker.md`, and this file's claim that the microcode
  was missing, are both wrong.
* **Clipping is not done by a separate program.** It runs *in the per-vertex
  program* as a chain of dp4s against the plane equations - `0x7a` starts it,
  `0x79` accumulates each further plane - and `0x6d` publishes the accumulated
  clip code to **`o10.x`**, which is the only thing in the whole vendor driver
  that writes that output. `mtx3605` is the edge interpolator the hardware runs
  afterwards to emit a new vertex, not the plane test.

The vendor reads the planes as `c[b1+12..17]` with `b1 = 0x160`, i.e. exactly
block `0x16c..0x171`, confirming `mtx2522`'s destination from the consumer
side. Because **nothing but the program reads those six slots**, the planes can
equally be put in the absolutely addressable window and dp4'd against there,
which avoids relative addressing entirely - and that is what this driver does,
since the validator rejects `IDXSEL`.

**Implemented:** `warp_primary_allowed()` now accepts `0x79` and `0x7a`
(`0x6d` already was); `p3d_warp_shape` gained `clip_planes` and `clip_slot`;
`p3d_warp_build()` emits the chain while `r2` still holds the clip-space
position and before the perspective divide reuses `r5`, then publishes `o10.x`.
The instruction count moves 16 -> 18 for one plane, so the chain really is
emitted and the kernel accepts it.

**Not working:** a plane keeping `x <= 0` in clip space against a triangle
spanning -0.75..0.75 leaves all 18336 pixels lit. The clip code is computed and
published and the hardware ignores it. `0x1110` extra bits 0x24 and 0x27 (the
vendor's own low byte) change nothing.

What is left to try, in order: the setup-stage entries
`0x10c8/0x10cc/0x10d0/0x10d4/0x10d8` and the kind-6/kind-12/stage-6 latches
that let the hardware spawn clipped vertices - the RE says
`parhelia_setup_commands()` and `parhelia_publish_commands()` already write
both, so the question is whether an ordinary draw from this driver actually
runs with them live; then the `0x1130` pipeline word, since clipping may need a
class or an enable this driver has never set.

## Two-sided lighting, and the pattern it shares with clip planes

**Implemented in the builder.** `shape->two_sided` and `back_material_slot`
run the whole light walk twice - the second pass negates the normal, reads the
back material, and writes its result to **`o7`** instead of `o1`, which is how
the vendor does it (front `o1`/`o2`, back `o7`/`o8`, materials at blocks
`0x134` and `0x156`, one program and one draw). With `two_sided` clear the
single-light program is still byte-identical to the kernel image and all 53
self-tests pass.

**The hardware does not select it.** A triangle wound the other way still
takes the front material: `ff0000` both ways where the back material is blue.
`0x1110` extra bits `0x20`, `0x40`, `0x80` and `0x100` change nothing.

**This is the same symptom as user clip planes**, and that is the useful
observation. In both cases the per-vertex program computes the right thing and
writes it to the output the vendor uses - `o10.x` for the clip code, `o7` for
the back colour - and the hardware ignores it. Two features, two different
outputs, one shared failure mode, so the likely cause is common to both: a
publication or enable step that says which outputs the setup unit should
consume, which every draw this driver builds omits.

The RE points at the setup-stage entries `0x10c8/0x10cc/0x10d0/0x10d4/0x10d8`
and the kind-6 / kind-12 / stage-6 latches that `parhelia_setup_commands()` and
`parhelia_publish_commands()` write at initialisation. The question to settle
first is therefore **not** another bit in `0x1110`: it is whether an ordinary
draw from this driver runs with those latches live at all, and what a vendor
capture writes between the program bind and the draw packet that we do not.
Settling that one question plausibly unblocks clip planes and two-sided
lighting together.

## Vertex blending (skinning)

`shape->bones` and `bone_slot` blend the position through two matrices before
the model-view-projection transform: four `dp4`s per bone against absolute
constants, then `w * bone0 + (1 - w) * bone1` as a multiply and a MAD into the
latched position T0. The weight is the vertex colour's x, and the second
weight is the implicit `1 - w`, which is the rule the vendor's own
`LASTBETA`-style leading `dp4` implements.

Everything is **absolutely addressed**, so no index register and no
base-relative addressing: the opcodes are `dp4`, `mul` and `mad`, all already
whitelisted. A true indexed matrix palette is a different matter - it needs
`mova` and `IDXSEL`, which the validator rejects because it cannot bound `a0`
statically, and that is a real design question rather than a whitelist edit.

**Verified on hardware.** Bone 0 is the identity and bone 1 the same matrix
translated +0.5 in x, so the weight should slide the triangle by exactly
`0.5 * 128` = 64 pixels at full effect on a 256-wide viewport:

| weight | x extent | shift |
|---|---|---|
| 1.00 | 32..222 | 0 px |
| 0.50 | 64..254 | 32 px |
| 0.00 | 96..255 | 64 px (clipped at the edge, 17840 lit) |

Exactly linear in the weight and exactly the predicted magnitude at both ends.

### The publication question is not a register bit in 0x1110 or 0x1130

Both candidate control words have now been swept against a clip-plane draw
that should remove half a triangle, and none of them changes anything:

| register | values tried | result |
|---|---|---|
| `0x1110` | `+0x20`, `+0x24`, `+0x27`, `+0x40`, `+0x80`, `+0x100` | 18336 lit, unclipped |
| `0x1130` | `+0x1000`, `+0x2000`, `+0x100000` | 18336 lit, unclipped |

`0x1130 + 0x1000` is worth naming: the vendor's own control words carry
`0x300c` where this driver writes `0x200c`, so bit 12 is set in vendor paths
where we clear it, and our register map calls that bit flat shading. Setting
it changes neither the clipping nor anything else visible.

So the remaining difference between a vendor draw that clips and ours is not a
bit in either word. The next step is a **capture diff**, not another sweep:
take a vendor `glEnable(GL_CLIP_PLANE0)` draw and this driver's draw of the
same geometry, and compare every register and packet between the program bind
and the draw. The same comparison covers two-sided lighting, since it fails
the same way - the program writes `o7` and the setup unit does not take it.

## What the vendor actually does for clipping, and why ours cannot work yet

A further RE pass answered the publication question, and the answer is that
**two register fields and a different stage program** are involved - the
sweeps above were looking in the right place for one of them and the wrong
place for the other.

| what | vendor | ours |
|---|---|---|
| `0x111c[21:16]` clip-plane count, low-justified | `(1 << n) - 1`, from a literal 64-entry popcount table (`mtx2119`) and `mtx1535` | `0x00000fdb`, top half **zero** |
| `0x1130[6:5]` | `2` when any user clip plane is on | `0` always |
| stage-6 program at `0x10d8` | `mtx1789` (334 instr) for 1-4 planes, `mtx1343` (361) for 5-6 | `mtx3733` (239) + `mtx1591` (41) |

The last row is the blocker, and the reason our geometry is never clipped: the
program that does the clipping is not on the chip, which is why no register
sweep could ever have worked.

**But the "239 + 41 = 280" identity is a coincidence, not a derivation, and it
matters.** Checked byte for byte against the shipped `stage-c.bin`:

* the extraction method is sound - `mtx251.bin` is found in the object at
  exactly `st_value + 0xdce00`, confirming the offset rule;
* `stage-c.bin` is **not contiguous anywhere** in `mtx_parhelia.o`;
* **none** of `mtx3733`, `mtx1591`, `mtx1789` or `mtx1343` appears inside it,
  in either order;
* yet every one of its 280 instructions occurs somewhere in the object.

And the firmware manifest names its source symbol as `stage-c`, which is **not
an ELF symbol** in the vendor object. So `stage-c.bin` was taken from a **live
WARP store dump at instruction 1376**, not assembled from symbol bodies - the
header comment in `parhelia_firmware.h` claiming "complete ELF symbol bodies,
not truncated live slot dumps" is wrong for this one file.

That changes the shape of the remaining work. A clip-capable stage image
cannot be produced by extracting `mtx1789`/`mtx1343` and appending a tail,
because the resident image is a *generated* program (`mtx2884` assembles
fragments), and ours was captured after that generation, not before.

**An attempt to attribute it instruction by instruction failed, and the
failure is worth recording so nobody repeats it.** Searching the object for
each of `stage-c.bin`'s 280 instructions and naming the symbol that contains
the first hit produces a confident-looking table - `mtx847` 104 instructions,
`mtx1038` 74, and six more, 280 of 280 "attributed". It is meaningless.
**Not one instruction in `stage-c.bin` is unique in the object**: every one
occurs at least twice, 100 of them more than fifty times, and one occurs
**1899 times**. The method finds coincidences, and collapsing the result into
runs gives 188 runs for 280 instructions - noise, not structure.

What *is* established stands on direct comparison rather than search:

* the extraction offset is right (`mtx251.bin` is found at `st_value + 0xdce00`);
* `stage-c.bin` is not contiguous anywhere in the object;
* against `mtx3733 + mtx1591` specifically, only **3 of 280** instructions
  match positionally and only 73 appear anywhere in that pair, so the
  `239 + 41 = 280` identity is a coincidence;
* the manifest's source symbol `stage-c` does not exist in the ELF.

So the resident setup program is a generated image whose composition is **not
known**, and recovering the clip-capable variant needs either a store dump
from the vendor driver with a clip plane enabled, or a reimplementation of
`mtx2884` validated against something better than instruction search - the
whole-image CRC of a regenerated zero-clip build against the shipped
`stage-c.bin` would be the honest test.

So clipping is reachable, but the work is kernel-side: build the 375- and
402-instruction stage-6 images the same way `stage-c.bin` was obtained, bind by
plane count, and set both register fields. Setting the two fields against
today's 280-instruction image is **not** a safe experiment - the body has no
handler for the state the bits announce - so it was not run.

Also worth recording: the stage-6 program depends on the cull mode as well
(`75 * cullMode` in the selection index), so `stage-c.bin` is correct only for
the cull setting it was captured under.

## Two-sided lighting: the documented enable does not engage it on the P690

The same pass placed the two-sided control at **`0x1130[8:7]`** - 0 off, 1
front face CCW, 2 front face CW - derived from the vendor kernel and, in its
author's words, never run on hardware. It is now run, and it does not work.

`0x1130` ORed with `0x80` and with `0x100`, each against both windings, and
each combined with `0x1110` `+0x20` and `+0x100`: the back-facing triangle
takes the **front** material every time.

The winding control rules out the obvious objection. With culling on, the two
vertex orders behave exactly as opposite faces:

| | front winding | back winding |
|---|---|---|
| `cull = 1` | culled (background) | drawn |
| `cull = 2` | drawn | culled (background) |

`0x1144` - which the vendor emits in the same packet as `0x111c` and this
driver hardcoded to zero, making it the best remaining candidate for "which
vertex outputs does setup consume" - was then tried at `0x80`, `0x180` and
`0xff` alongside `0x1130 |= 0x80`. No change either.

So the hardware does distinguish the two faces, the program does write `o7`,
and the setup unit still does not take it. Every register this driver writes
around a draw has now been swept against it. Either the attribution of
`0x1130[8:7]` is wrong, or - more likely, given what clipping turned out to
need - **the resident stage program decides this too**, and ours is a build
with no back-colour path. That is the hypothesis to test next, and it is the
same shape as the clip-plane answer: not a bit, a program.

## Polygon offset: applied by the vertex program, and it works

The earlier measurement in this file - `c[0x178]` loaded with
`{factor, units, 0.5, 786432}` and no effect on a coplanar pair - was right
about the symptom and wrong about the cause. `c[0x178]` is read by the
**vertex program**, exactly as `c[0x172].w` is for point size; loading a
constant that nothing reads does nothing.

`p3d_warp_shape` gained `polygon_offset` and `offset_slot`: the generator adds
a bias vector to z, in the pass-through branch on the way to `o0` and in the
transform branch after the viewport multiply. The constant holds
`(0, 0, units, 0)` in window-depth units.

**Verified on hardware** with `--polygon-offset-probe`, which draws a red
triangle then a coplanar green one under a LESS depth test - only a bias can
separate them:

| units | result |
|---|---|
| 0 | red keeps every pixel |
| -1/65535 | green takes every pixel |
| -16/65535 | green |
| -1/256 | green |
| -1/16 | green |
| +1/16 | red - a positive bias must never win, and does not |

The controls in the same run pass: green at z 0.4 takes everything, green at
z 0.6 takes nothing, so the depth test and the draw order are sound. One depth
unit is enough to flip it, so the granularity is fine enough for decals.

**The slope-scaled term is not implemented.** `GL_POLYGON_OFFSET` is
`factor * m + units * r`, and `m` is the primitive's maximum depth slope, which
a vertex program cannot see - the `factor` rows in the probe correctly move
nothing. Quake 3's decals use the units term, so this is the useful half; a
complete implementation needs either a setup-side slope-scaled bias (not
located) or the slope computed host-side per primitive.

## Specular lighting

`shape->specular`, `specular_slot` and `shininess_log2` add a specular term per
light: the halfway vector at `specular_slot + 2i`, the specular colour at
`+ 2i + 1`, and `max(0, N.H)` raised to `2^shininess_log2` by repeated
squaring - this ISA has no POW, so the exponent is a power of two. The term
accumulates in its own register and folds into the colour with the ambient.

Two encoding rules had to be learnt to get it right, and both are general:

* **A full-mask `dp3` does not broadcast its scalar.** The vendor only ever
  writes one component with `dp3`, and reading the other components of a
  full-mask result gives something that is not the dot product. Every earlier
  lighting measurement in this file used `N.L = 1`, where the difference is
  invisible; the first specular attempt, with `N.H` varying, produced an
  `x^(1/4)`-shaped curve that was this and nothing else.
* **`src1` encodes through the shared swizzle field, which carries no
  replicate.** Writing `dp3` to one component and reading it as `.yyyy` from
  `src1` silently reads the single component instead, which shows up as one
  coloured channel. The broadcast has to go through `src0` of its own
  instruction first.

**Verified on hardware**, one module reload per configuration. With the
specular colour at 0.3 and one squaring, so the term is `colour * N.H^2`:

| N.H | measured | `0.3 * 5/3 * N.H^2` |
|---|---|---|
| 1.0 | `0x80` = 0.500 | 0.500 |
| 0.7071 | `0x40` = 0.250 | 0.250 |
| 0.5 | `0x20` = 0.125 | 0.125 |

**The exponent is exact** - the term halves precisely with the square of
`N.H` - and the shape of the highlight is right.

**The magnitude carries a measured 5/3 factor**, unexplained. Holding
`N.H = 1` and sweeping the colour: 0.25 reads 0.420, 0.3 reads 0.502, 0.5
reads 0.835 - all `x * 5/3` to within a bit - then it compresses, 0.6 reading
0.867 rather than saturating. Ruled out by measurement, each its own run: the
scratch registers (`r12`/`r13` against `r14`/`r15`, in case they collided with
the `r10`/`r11` light accumulators), the destination file (writing the T latch
instead kills the draw outright, so T3 is in use), the constant slot (`c5` and
`c22` behave identically), and masked versus full-mask `dp3`. The same 5/3
appears elsewhere in this file's history and has never been explained.

Until it is, a driver wanting GL-correct magnitudes should pre-scale the
specular colour by 3/5 host-side; the builder takes the constant as given and
does not apply a factor of its own.
