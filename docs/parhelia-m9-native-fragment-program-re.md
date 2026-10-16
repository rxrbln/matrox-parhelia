<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9148 native fragment program: location, entry and first custom programs

This note answers where the M9 raster pass takes its per-pixel program from,
shows on hardware that a patched native fragment program executes, and sizes
the work for a NIR-to-native fragment backend (the prerequisite for glamor on
the clean-room m9x Gallium driver).  Labels: **MEASURED** is an M9148
(`102b:0540`) result through `DRM_IOCTL_PARHELIA_M9_SUBMIT` on September 15,
2026 with the unchanged `m9x_drm.ko`; **CAPTURED** is read from saved vendor
captures; **STATIC** is read from the pinned Linux `m9x_drv.so`
(SHA-256 `3878475450ffad5c...0570273`).

## Result

The fragment program is **not** in the index-`6c` linked record, the index-`69`
fixed-fragment database or the `68/60..67` inline uploader.  It is the first
part of the native program resource bound at index `c2`, followed by the
vertex program(s).  Index `c3` carries the entry slot of each pass: a
preprocessing pass enters at its vertex program and a raster draw enters at its
fragment program.  For one fragment program that is `c3` = fragment slot count
and `c3=0`, respectively.  A driver-built
fragment program therefore needs no new record format: it is a slot prefix of
the `c2` image plus the correct `c3` entries and the raster constant span.

## 1. Evidence for the location

### A/B/A GLSL capture (CAPTURED)

`captures/m9x-linux-glsl-ab-20260914` (decoded with
`tools/m9-command-decode.py --execute`).  Everything except allocation
addresses and write-back serials that differs between draw 15 (A) and 19 (B):

| change | pass | `c2` | `c3` | `8c` | `8d..8e` span | `69/6a/6b` |
|---|---|---:|---:|---:|---:|---|
| FS-A (varying color) | pre | `11f81000` | `4` | `00501500` | `0xa8` | fixed |
| FS-A | raster | `11f81000` | `0` | `00500200` | `0x10` | fixed |
| FS-B (color * uniform) | pre | `11f83000` | **`9`** | `00501500` | `0xa8` | fixed |
| FS-B | raster | `11f83000` | `0` | **`00500600`** | **`0x30`** | fixed |
| VS-B (x += 0.125 w) | pre | `11f83000` | `4` | **`00501700`** | **`0xb8`** | fixed |
| VS-B | raster | `11f83000` | `0` | `00500200` | `0x10` | fixed |

Changing only the fragment shader moves the vertex entry from slot 4 to slot 9
(five more fragment slots for the multiply) and grows only the raster constant
span.  Changing only the vertex shader keeps the entry at 4 and grows only the
preprocessing constant span.  Both switches bind a new `c2` allocation; `6c`
also changes, but the 0x80-byte semantic image selected there contains no
code (`parhelia-m9-pool5-raw-copy-closure.md`).

### Vendor `c2` dumps (CAPTURED)

`captures/m9x-cold-init-20260915-v1/gt-*/vram/c2-*.bin`, disassembled with
`tools/m9-native-disasm.py`.  Flat and Gouraud gltri (40 slots) and glxgears
(99 slots) share slots 0..3 byte for byte and both bind preprocessing `c3=4`:

```text
0 5e62ad9e ee003c18   op0=1e cls=4 op2=10 a=42 b=79 c=45 d=2d
1 1e622d9d ae003c18   op0=1d cls=4 op2=10 a=40 b=79 c=44 d=2d
2 ffff801c a01fc00f
3 ffff801c a01fc00f
```

This is the fixed-function fragment program.  The gltri vertex program (slots
4..39) and the gears lighting program (slots 4..98) both end with the same
`ffff801c a01fc00f` slot.

### Special-shader fixture (CAPTURED)

In `captures/m9x-linux-gl2-special-20260914` the first preprocessing `c3` gives
the fragment slot count of each case.  Later preprocessing passes step
further into the same image.  The window raster draw binds `c3=0`, except in
tex2d-bias and tex2d-proj.  Their raster range holds two draws: one into an
intermediate `ee190004` target with `c3=0` and one into the window target
(`cc210080`, sampler `7c=00009312`) with `c3=4`.  Several fragment programs
can therefore share one image, each draw selecting its own entry:

| case | pre-pass `c3` values | raster `8c` (8-byte constant entries) |
|---|---|---|
| baseline | 4, b, 10 | `00500200` (2) |
| tex2d | 4, b, 10 | `00500200` (2) |
| dfdx / dfdy / tex2d-proj | 8, f, 14 | `00500200` / `00500000` |
| mat4-vec4 | b, 12, 17 | `00500a00` (10) |
| discard-cond | f, 16, 1b | `00500400` (4) |
| if-uniform | 10, 17, 1c | `00500600` (6) |
| loop-fixed / loop-uniform | 2c, 33, 38 | `01500a00` (10, plus `8c[24]`) |

### Emitters (STATIC)

* `m9x29970` (`0x4ae470`) emits header `458fc3c2` with two relocations of the
  same resource pair at state `+0x148`: packet slot 2 (`c2`) usage `0x13`, slot 3
  (`c3`) usage `0x14`.  `c3` is therefore a relocation-callback value derived
  from the program resource, consistent with the pass-dependent entry above.
  Its alternate branch emits literal `c2=4, c3=0` and uploads `+0x18c` two-dword
  slots from `+0x190` through window selector
  `8f = 0x80000000 | ((2k >> 4) & 0x3f) << 6` and ports
  `0x90 + (2k & 0xf)`, `0x90 + ((2k + 1) & 0xf)`.  The invariant `8f/90..9f`
  context-start image in earlier captures is therefore this inline form of the
  `c2` program group, analogous to `69=4` for the database.
* `m9x30355` (`0x4ae850`) emits `8c = state+0x158`, `8d` = relocation of
  `+0x160` with usage `0x17`, `8e` = relocation of `+0x170` with usage `0x18`.
  When `8c & 0x7f007f00` is zero it takes an inline constant path (`e6/e3`, then
  `8f` with selector byte `0xc0`, count `+0x19c`).  Across all 77 relocated
  `8c/8d/8e` groups in the special and A/B/A fixtures,
  `8e - 8d == 8 * 8c[14:8]` without exception (CAPTURED), so constants are
  8-byte (component-pair) entries.

## 2. Hardware verification (MEASURED)

`tools/m9-user-fragment-probe.c` rebuilds the verified first-triangle
two-pass stream (`m9-user-triangle-data.h`), patches the vendor 40-slot `c2`
image, overrides `c3` per pass, `88`, `8c` and a raster constant span at system
`+0x1000`, and draws smooth triangles.  It reads the tiled color BO linearly
with `DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS` and matches each output byte of every
pixel at least 1 px inside a triangle against the interpolated inputs
`r,g,b,1-r,1-g,1-b,ff,00` (±4).  A channel is reported only when at least 98 % of
the interior pixels match.  The first runs used two triangles (24912 interior
pixels, corners `ff0000/00ff00/0000ff` and `3366cc/cc3366/66cc33`).

| variant | patch | result |
|---|---|---|
| base | vendor image, pre `c3=4`, raster `c3=0` | `A=ff R=r G=g B=b` |
| base-append | copy slots 0..3 to 40..43, raster `c3=0` | identical to base |
| ras-entry-40 | same image, raster `c3=0x28` | identical to base |
| ras-entry-4 | raster `c3=4` (vertex program as fragment) | `R=00 B=00`, A/G `ff` on 75 % |
| e40-swap | copy to 40, swap `c` fields at 40/41, raster `c3=0x28` | same pixels as src-swap |
| e40-swap-ctl | same image, raster `c3=0` | identical to base |
| **op-swap** | slot 0 `op0 1e->1d`, slot 1 `1d->1e` | **`A=g R=b G=ff B=r`** (exact permutation) |
| nop1 | slot 1 = `ffff801c a01fc00f` | `A=ff R=00 G=ff B=b` |
| nop0 / nop01 | slot 0 (or 0 and 1) = that slot | `R=00 B=00`, A/G `ff` on 75 % |
| d-2c, e-1 | `d=2c` / `e=1` in slots 0,1 | identical to base |
| const-trace | raster constants `.25,.5,.75,.125` | identical to base |
| a1-41 | slot 1 `a 40->41` | identical to base |
| src/a/b mutations | `c 44<->45`, `a 40<->42`, `b=7f/78`, `a=42/44/7f` | per-triangle flat or saturated values, not decoded |
| W1[11:10]=0 in slot 0 or 1 | lowerer constant-source bits | output changes, independent of `e` and constants |

Conclusions:

1. **Raster `c3` is the fragment entry slot.**  A modified program placed after
   the vertex program runs only when the raster pass enters there; the
   original slots 0..3 still run with `c3=0`.
2. **A patched fragment program executes per pixel.**  Swapping `op0` of the
   two output slots permutes channels exactly: output `(R,G,B,A) = (b, 1, r, g)`.
   So the cls-4 form with `op0=1d` writes output pair xy (R,G) and `op0=1e`
   writes pair zw (B,A).  The output register maps x,y,z,w to pixel R,G,B,A.
   The permuted draw covered 25825 pixels instead of 26000, so edge coverage is
   not fully identical.
3. **`ffff801c a01fc00f` stops the fragment program.**  Replacing slot 1 leaves
   R,G unwritten while slot 0 still writes B,A.  With nothing written the pixel
   keeps a state-dependent value (`00ff00` with alpha `ff` on 75 % of pixels
   here), so an explicit write to every channel is required.
4. The vendor fixed-function fragment slots do not read the raster constant
   span.  Their `a/b/c` selectors are operands of operation `op2=0x10` (not the
   lowerer MOV form `op2=0x14`); single-field changes produce values that are
   not plain input copies, so these slots cannot be edited field by field into
   arbitrary sources.

### Second session: constant output and source selectors (MEASURED)

After the reboot (module sha `de704649...`, `c3` now range-checked by the
validator), the probe drew three smooth triangles (8506 interior pixels each;
corners `ff0000/00ff00/0000ff`, `3366cc/cc9933/99cc66`, constant `4080c0`).
Slot 0 (vendor B,A interpolation) was kept; slot 1 was replaced by a
lowerer-style MOV carrying the cls-4 output routing:

```text
W0 = 0xffc00000 | c << 15 | 0x2d << 8 | 4 << 5 | op0     op0 1d -> R,G ; 1e -> B,A
W1 = 0xa2803f1f                                           source in c (W1[11:10]=11)
W1 = 0xa280331f | (2*i + pair) << 14                      source constant pair e (W1[11:10]=00)
```

| slot 1 | R,G on all three triangles | reading |
|---|---|---|
| constant `e=0` (`a280331f`) | `3f-40`, `7f-80` | constant entry 0 = (0.25, 0.5) |
| constant `e=1` (`a280731f`) | `bf-c0`, `1f-20` | entry 1 = (0.75, 0.125) |
| constant `e=2` (`a280b31f`) | `5f-60`, `9f-a0` | entry 2 = (0.375, 0.625) |
| constant `e=3` (`a280f31f`) | `df-e0`, `ff` | entry 3 = (0.875, 1.0) |
| `c=0x44` | t0 `00,00`; t1 `99,cc`; t2 `40,80` | provoking (last) vertex color x,y, not interpolated |
| `c=0x45` | t0 `ff,ff`; t1 `66,ff`; t2 `c0,ff` | provoking vertex color z,w |
| `c=0x40`, `c=0x42` | flat per-triangle values, not colors | unidentified per-triangle setup data |
| cls 0 instead of 4 (`c=0x44`) | same as cls 4 | cls does not change this output write |

Constant output color is therefore verified: raster constants are 8-byte
entries in the `8d..8e` span; `e` selects the entry (`2*i + pair`, i.e. pair
index); the output MOV form above writes them to R,G or B,A.  The vendor
`op2=0x10` slot (`a=40/42 b=79 c=44/45`) is the per-pixel interpolation
operation: it combines the provoking-vertex attribute (`c`) with per-triangle
setup data (`a`) and, presumably, the pixel position (`b=0x79`).  Its exact
formula is unmeasured.  Replacing slot 0 with a constant MOV to B,A while
keeping vendor slot 1 changed R,G and A unpredictably, so slot 1 depends on
state that slot 0 leaves behind.

**Lockup trigger (MEASURED):** a program at slot 40 (raster `c3=0x28`) of
`ffff801c a280331f`, `ffff811c a280731f` (lowerer constant MOV, cls 0,
destinations `d=0x00/0x01`), then the output MOV from `c=0x00/0x01` and the stop
slot, timed out (`M9 kernel stream timeout ... command=0x4a90dbe/0x4a90dc8`) and
left the engine unusable.  Class-0 destinations `0x00/0x01` are not safe fragment
temporaries.  The temporary selector space, MUL/ADD of two sources and
interpolation into a temporary remain unverified.  The prepared variants are in
*Reproduction*; each must first be proven with a safe destination.

## 3. What a NIR-to-native fragment backend needs

**Image and binding.**  One `c2` BO holds `[fragment slots...][vertex slots...]`.
Each raster draw uses the slot index of its fragment program and each
preprocessing pass the slot index of its vertex program.  The vendor payload is `0x1080` bytes (528 slots) and the
compiler limits a stage to 256 slots.  The current validator accepts literal
`c3` values (MEASURED with 0, 4 and 0x28).

**Encoding coverage.**  The lowerer corpus (`parhelia-m9-native-lower-runtime.md`,
195-case matrix) provides exact slot sequences for MOV, ADD, SUB, MAD, MUL,
RCP, RSQ, DP3, DP4, MIN, MAX, EXP, LOG, TEXLD, CMP, DDX, DDY, SETP and FLOOR,
plus masks, swizzles, modifiers, predicates and conditional forms.  Those
sequences use synthetic descriptors and class-0 results.  Still missing for the
fragment stage:

* the physical selectors of fragment inputs (varyings bound by the raster
  `23` declaration list) and of temporaries.  The vendor slots read `0x40/0x42`,
  `0x44/0x45` and `0x79`, whose semantics are not decoded;
* the output write form: class 4 with `op0` 1d/1e (MEASURED above); the vendor
  compiler endpoint `fffc7e1d a2803f0f` does not occur in the dumped programs;
* temporaries: class-0 destinations `0x00/0x01` lock up the engine, so the
  fragment temporary selector range must be found (the vendor interpolation
  slot routes its result with `d=0x2d`, so selectors near that are candidates);
* the interpolation formula of `op2=0x10` for arbitrary varyings (texcoords).
  The raster constant path is verified: 8-byte entries in `8d..8e`, `e = pair
  index` with `W1[11:10]=00`;
* TEXLD resource metadata and sampler binding (`parhelia-m9-texture-state.md`,
  five-stream textured raster template), kill (discard) and derivatives.

**Glamor subset.**  Glamor shaders need texture sampling (1 to 2 samplers),
MUL, ADD, MIX (`MAD`/`LRP`), DP, CMP-style selects and discard.  All are in the
lowerer corpus except TEXLD metadata and KIL.  A first backend can emit
per-component-pair slots from the corpus rules, write outputs with the cls-4
form and end with `ffff801c a01fc00f`.

**Kernel.**  No change is needed to bind driver-built programs.  Recommended:
validate that `c3` lies inside the `c2` BO, treat `c2` as read-only for the
job, and add an engine reset that clears the requester.  A module reload does
not recover a locked-up requester (observed above), and a textured raster `88`
without a texture binding is one known trigger, so the validator should also
reject it.

## 4. Next steps, in order

1. After reboot: run the MOV-form batch; establish constant output and a pure
   varying copy.  Then sweep the source selector over `0x00..0x7f` in a MOV slot
   with traceable varyings (float4 color, texcoord declaration) to map
   varying/temporary selectors.
2. Vendor capture (highest value, no hardware risk): rerun
   `tools/m9x-glsl-ab.c` and `m9x-glsl-special-matrix.c` under vendor X with
   `tools/m9-vendor-vram-dump.py` on the bound `c2` address and on the
   `8d..8e` spans.  This yields exact native fragment programs for constant
   color, uniform multiply, texture2D, discard and derivatives, with their
   constant layout.
3. Replay a vendor tex2d fragment program with the textured raster template to
   pin sampler selectors, then implement the NIR backend against these images
   as differential fixtures.

## Reproduction

```sh
# target, inside flock -w 1200 /tmp/m9-hw.lock
gcc -O2 -Wall -I/root/parhelia-cleanroom/include -o m9-user-fragment-probe \
    m9-user-fragment-probe.c -lm
./m9-user-fragment-probe base ras-entry-40,copy0-3@40,rasc3=28 \
    ras-entry-4,rasc3=4 op-swap,op0@0=1d,op0@1=1e \
    nop1,slot1=ffff801c:a01fc00f
K=const=0.25,0.5,0.75,0.125,0.375,0.625,0.875,1.0
./m9-user-fragment-probe mov-rg-c44,slot1=ffe22d9d:a2803f1f \
    mov-rg-k1,slot1=ffffad9d:a280731f,$K mov-ba-k1,slot0=ffffad9e:a280731f,$K
```

Second-session constant outputs (verified) and the unverified MUL/ADD forms:

```sh
K=const=0.25,0.5,0.75,0.125,0.375,0.625,0.875,1.0
./m9-user-fragment-probe mov-rg-k0,slot1=ffffad9d:a280331f,$K \
    mov-rg-k3,slot1=ffffad9d:a280f31f,$K mov-rg-c44,slot1=ffe22d9d:a2803f1f
# locks up the engine, do not run: k-out with class-0 d=0x00/0x01 temporaries
# MUL corpus form (a,b sources): W0=(a&7)<<29|b<<22|7f<<15|d<<8|1c W1=ac200100|a>>3
# ADD corpus form (a,c sources): W0=(a&7)<<29|7f<<22|c<<15|d<<8|1c W1=ac800d00|a>>3
```

Variant syntax: `name,arg,...` with `slotN=w0:w1`, `FIELD@SLOT=hex` (`a b c d e
op0 op2`), `w0and@SLOT=`, `w1or@SLOT=`, `copyS-E@D`, `prec3=`, `rasc3=`, `r88=`,
`r8c=`, and a final `const=f,f,...` (it sets `8c[14:8]` to the pair count).
