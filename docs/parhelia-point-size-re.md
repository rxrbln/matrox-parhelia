# Hardware point size: the constant, the WARP port, and the primitive class

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Answers the question `parhelia-fog-points-implementation-leads.md` §"Hardware
point size" left open: which register carries `glPointSize`, in what encoding.

**The answer is that it is not an MMIO register at all.** The size travels as a
vertex **constant-file slot** and reaches the setup unit as a **WARP vertex
output port**. Three independent sources agree: the Linux kernel module, the
Windows Direct3D driver, and the `gl-matrix` captures.

Evidence tags: **PROVEN** = read out of a binary or measured in a capture;
**INFERRED** = best reading, consistent with everything, not directly shown;
**OPEN** = not determined.

---

## 1. Summary

| what | where | encoding |
|---|---|---|
| point size | constant/block slot **`c[0x172]`, component `.w`** | `0.5f * (float)(int)(size + 0.5f)` — **half** the size, in **window pixels**, with the size first snapped to a whole pixel count |
| point size min / max | `c[0x173].x` / `c[0x173].y` | floats; the size is clamped to this range *before* halving; max is itself clamped to `2048.0f` |
| point distance attenuation | `c[0x172].x/.y/.z` | GL `GL_POINT_DISTANCE_ATTENUATION` `(a, b, c)`, default `(1, 0, 0)` |
| half-pixel centring constants | `c[0x173].z` / `c[0x173].w` | fixed `+0.5f` / `-0.5f` |
| the extent the setup unit reads | **WARP vertex output port `o10`, component `.z`** | the vertex program must write it; without the write a point rasterises nothing |
| wide-point primitive class | `0x1130[25:24] = 3` and `0x1300` low nibble `0xc` | class 0 (`0x1130[25:24] = 0`, nibble `0xd`) is the one-pixel point |

There is **no enable bit** beyond the class selection. Everything else in a
wide-point draw is byte-identical to a unit-point draw (§4).

---

## 2. `c[0x172].w` is the point radius — three sources

### 2.1 Linux kernel module [PROVEN]

`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`, `mtx2497`
(`.text+0x867a0`), read through `scratchpad/mtxk_r.asm`. `rcx = [rbx+0x3c68]`
is the shadow of the `0x172`/`0x173` block pair, laid out
`[+0x00..+0x0c] = c[0x172].xyzw`, `[+0x10..+0x1c] = c[0x173].xyzw`:

```
 86910  xmm0 = [rsi+0x258]                 ; the GL/D3D point size
 8691a  xmm0 *= xmm1                       ; supersample/AA scale, 1.0 normally
 8692d  minss xmm1, [rcx+0x14]             ; clamp to c[0x173].y  (max)
 86932  maxss xmm0, [rcx+0x10]             ; clamp to c[0x173].x  (min)
 86939  if ((K[0x08] & 3) != 1) goto store the clamped size unchanged
 869b5  if (G[0x09] & 2)        goto  "     "
 869ba  if (K[0x04] & 0x40)     goto  "     "
 869c3  xmm0 = .LC4                        ; = 0x3f000000 = 0.5f
 869cb  xmm1 += 0.5f
 869d2  rdx  = (long)xmm1                  ; N = (int)(size + 0.5f)
 869df  xmm1 = (float)N
 869e4  [rbx+0xe2] = ([rbx+0xe2] & ~2) | (((N & 1) == 0) << 1)    ; parity, §5
 869fb  xmm2 = 0.5f * (float)N             ; <-- the value that is stored
 86970  [rcx+0x0c] = xmm2                  ; c[0x172].w
 86984  mtx2702(ctx+0x7e8,  0x172, 0)      ; queue the block upload
 86998  mtx2702(ctx+0x2218, 0x172, 0)
```

`.LC4` is `.rodata.cst4 + 0x190` in the object = `0x3f000000` = `0.5f`,
resolved through the `R_X86_64_PC32` at `.text+0x869c7`. `[rcx+0x0c]` is the
**only** write to `c[0x172].w` anywhere in the module (exhaustive scan of
`[...+0x3c68]` users: `0x8691e`, `0x86a04`, `0x87e64`, `0x87ea7`, `0x8baad`,
`0x8bb8b`, plus the two allocations at `0x8e465`/`0x8ef15`; `mtx3654`
@`0x8bb2d` writes only `.xyz`).

`rbx+0xdc` is the WARP code-generation key `K`, so `[rbx+0xe4] = K[0x08]` and
`[rbx+0xe2] = K[0x06]`.

### 2.2 Windows Direct3D driver [PROVEN]

`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`, the same computation on a
structurally identical shadow (`ecx = [esi+0x3aa8]`, same `+0x0c`/`+0x10`/`+0x14`
layout), via `scratchpad/pard.asm`:

```
 11e6f9  fcom [ecx+0x14]  ; clamp to max = c[0x173].y
 11e708  fcom [ecx+0x10]  ; clamp to min = c[0x173].x
 11e717  if ((state[0xe4] & 3) == 1 && !edi && !(state[0xe0] & 0x40)) {
 11e732     st0 += ds:0x19c9d4                 ; 0x3f000000 = 0.5f
 11e738     call 0x198d80                      ; ftol  -> N
 11e743     fild N   (+ ds:0x19c400 = 2^32 if negative)
 11e75f     parity = ((N & 1) == 0)
 11e761     state[0xe0] = (state[0xe0] & ~0x20000) | (parity << 17)   ; = byte +0xe2 bit 1
 11e759     st0 = 0.5f * (float)N
          }
 11e7de  fstp [ecx+0x0c]                       ; c[0x172].w
 11e7e3  push 0x172 ; upload block 0x172       (twice, two contexts)
```

The companion routine at `0x11aab0` writes the other three components and the
clamps: `[ecx+0x00/04/08] = D3DRS_POINTSCALE_A/B/C` (`push 0x172` at
`0x11ab16`/`0x11ab48`), then `[ecx+0x10] = min`, `[ecx+0x14] = max` hard-limited
to `ds:0x19d454 = 0x45000000 = 2048.0f` (`push 0x173` at `0x11abb2`).
`2048.0` is the same number the GL stack reports as `GL_ALIASED_POINT_SIZE_RANGE`
upper bound and as block slot `0xa6` (`docs/parhelia-gl-limits.md`).

### 2.3 Hardware capture [PROVEN]

`captures/gl-matrix/psize-secs.txt` is `GLTEST_PRIM=points GLTEST_PSIZE=8`
(`tools/capture-gl-matrix.sh:59`). Extracting every `41080013` block load —
header `count=4, first=0x172` — from each capture:

| capture | `c[0x172]` | `c[0x173]` |
|---|---|---|
| `base` (triangle) | `(1, 0, 0, 0.5)` ×27 | `(0, 2048, 0.5, -0.5)` |
| `points` (size 1) | `(1, 0, 0, 0.5)` ×9 | `(0, 2048, 0.5, -0.5)` |
| `psize` (size 8) | `(1, 0, 0, 0.5)` ×14 **then `(1, 0, 0, 4.0)` ×6** | `(0, 2048, 0.5, -0.5)` |
| `lwidth` (line width 8) | `(1, 0, 0, 0.5)` ×20 **then `(1, 0, 0, 4.0)` ×7** | unchanged |

`glPointSize(8)` → `4.0f` = `0.5 * 8`. Exactly §2.1's formula.
`captures/gl-matrix/attribution.txt:311` reports the same delta independently
(`c[0x172]: (1,0,0,0.5) -> (1,0,0,4)`).

Note the `lwidth` row: **`glLineWidth(8)` writes the same slot**, so `c[0x172].w`
is a shared "half primitive extent" and not point-exclusive. That matches
`docs/parhelia-fragment-ops.md:221` ("index `0x36`, 2 floats, line width, point
size"). Which of the two a draw uses is decided by the primitive class.

---

## 3. The extent reaches the setup unit as WARP output `o10.z` [PROVEN]

`mtx2179` (`.text+0xa2400`) appends a point-size microcode fragment to the
generated fixed-function vertex program when `K[0x04] & 4` — registry index
`0xc4` (`mtx727`, 15 bundles) if `K[0x48] == 3`, else `0xc5` (`mtx793`, 13
bundles) — `docs/parhelia-warp-codegen.md` §1.2 step 6.

Disassembled with `tools/parhelia-warpreg.py -i 0xc5` piped into
`tools/parhelia-warpdis2.py` (the unpatched template; `mtx2179` patches source
register and constant indices at emission time):

```
  0: sub   T1, r7, r7
  1: mov   r9, c24.yyyy
  2: mul   r6.x, T1.yyyy, T1.zzzz
  3: mul   r6.y, T1.xxxx, T1.zzzz
  4: mul   T1,   T1.xxxx, T1.yyyy
  5: sop8b T1,   T1.000w
  6: mul   T3, r6.xy00, T1.ww00
  7: mul   T1, r6.xy00, T1.ww00
  8: max   T1, T1.xxxx, T1.yyyy
  9: mul   T1, T1, c24.xxxx
 10: add   T1, r9, T1
 11: add   r7, r7, T1.00z0
 12: mov   o10.z, T1.zzzz          <-- 041802a4 80440200 b05a2124
```

Fragment `0xc4` is the same with two extra head bundles and `dp3`s in place of
the `mul`s at 2–4. **Both end with the identical bundle
`041802a4 80440200 b05a2124` = `mov o10.z, T1.zzzz`.** Decoded with the field
map of `docs/parhelia-warp-isa-complete.md`, that bundle is: primary opcode
`0x60`, secondary op `1`, port-A destination **file 2 (output), index 10, write
mask `0x4` (`.z` only)**, `IDXSEL = 0`, `f1i = 0`, no reserved bits — i.e. it
names output register 10 component z and nothing else.

Cross-check on hardware: `docs/parhelia-engine-context-bringup.md` §14.1
measured that a point draws **nothing** when the bound program does not write
`o10.z`, at any size and on any boot — the one positive result in that section
(six points, six pixels, class 3) was withdrawn in §14.1a as unreproducible, but
the *negative* half held on every boot.

The affine tail (`o10.z = c.x * max(...) + c.y`) is the projection of the radius
into screen space for the transforming path; the exact operand mapping after
`mtx2179`'s patches is **[OPEN]** and does not matter for a driver that feeds
window-space vertices, because it can write `o10.z` directly.

---

## 4. What else a wide point changes: nothing but the class [PROVEN]

Decoding the draw spans of `points-secs.txt` and `psize-secs.txt`
(`tools/decode-gl-secs.py`, full decode, the section that ends in the
one-vertex `DRAW`), the two are identical except for three values:

| register / slot | `points` (size 1) | `psize` (size 8) |
|---|---|---|
| `0x1130` | `0x0000200c` | **`0x0300200c`** — bits [25:24] = class 3 |
| `0x1300` (DWGCTL) | `0x00a60ecd` — nibble `0xd` | **`0x00a60ecc`** — nibble `0xc` |
| `c[0x172].w` | `0.5f` | **`4.0f`** |

Identical in both: `0x10b0 = 0x00060020` (the points primitive word),
`0x12fc = 0x0bb80024`, `0x10f0`, `0x1060 = 0x000280cc`, the `0x10b8`
descriptors `0x20c00000`/`0xa1c50020`, `0x1110/0x1114/0x1118`, the guard band
`0x1120..0x112c`, and the bound WARP entry — the last `0x1100` before the draw
is `0x08557583` in `base`, `points` **and** `psize`. The draw packet is
`DRAW 97080001`, **5 dwords per vertex, one vertex**, in both.

Two corrections fall out of that:

* `docs/parhelia-gl-state-encoding.md` §4.9 predicted the sized-point draw
  would become an 8-dword pre-transformed vertex (`0x970e0001`) with a
  regenerated WARP program. **It does not**: same 5-dword layout, same program
  entry. The vendor's one fixed-function program writes `o10.z`
  unconditionally and triangles ignore it.
* `reference/p3d_state.c:648` writes `0x12fc = 0x04000024` for `P3D_POINTS` and
  `0x0bb80024` otherwise. In the `gl-matrix` captures **both** the unit-point
  and the wide-point draw use `0x0bb80024`; the `0x04000024` reading comes from
  the separate `points-vs-tri` capture (`bringup` §20.1). Worth re-checking
  before relying on either — it is not the point size and did not change the
  measured result in §20.2.

---

## 5. The parity bit `K[0x06]` bit 1 [PROVEN that it is set; INFERRED what for]

Both vendor drivers set one extra state bit from the *parity* of the rounded
size: Linux `mtx2497` @`0x869e4`–`0x869f5` (byte `HWSTATE+0xe2` bit 1),
Windows `MTXPARD` @`0x11e761`–`0x11e76d` (bit 17 of the dword at `state+0xe0`,
the same bit). `HWSTATE+0xdc` is the WARP code-generation key `K`, so this is
`K[0x06]` bit 1 and it is part of the program cache key: the generated vertex
program differs between an odd and an even point size.

It is not a gate in any emission rule listed in
`docs/parhelia-warp-codegen.md` §1.2, so the most likely use is the half-pixel
centring — GL centres an odd aliased point on a pixel centre and an even one on
a pixel corner, and `c[0x173].z/.w` hold exactly `+0.5` / `-0.5` for that.
[INFERRED]. The Gallium driver already implements that rule on the CPU in
`ph_point_centre()` (`parhelia_context.c:4617`), which is the behaviour to
reproduce if the hardware turns out not to do it.

---

## 6. `0x10fc` is not the point size — the lead is closed [PROVEN]

`parhelia-fog-points-implementation-leads.md` flagged `0x10fc = 0x42400000`
(48.0f) in the points capture as the prime candidate, with the caveat that the
same register also appeared to carry `-8160.0f`, `-0.5f`, `0` and `1`.

Every one of those is a **decoder desync**. `tools/decode-gl-secs.py` frames a
mis-aligned block-load payload as a register list. `points-secs.txt` around the
offending write decodes as:

```
   1000 = 4104000b        <- a block-load header, read as a value
   1200 = 00000160        <- "count=2 first=0x160", the viewport block
   10fc = 00000001
   ...
   1000 = 41080013        <- the 0x172 block-load header
   1010 = 00000172        <- "first = 0x172"
   1000 = 3f800000  1000 = 00000000  1000 = 3f000000     <- c[0x172] = (1,0,0,0.5)
   1000 = 45000000  1000 = bf000000                      <- c[0x173] = (0, 2048, 0.5, -0.5)
   1000 = 45ff0000  1000 = 45ff0000  10fc = c5ff0000     <- c[0x174] guard band ±8160
   1000 = 43000000                                       <- c[0x175] = 128.0
```

The `0x42400000` instances are likewise inside a vertex buffer: the very first
section of `points-secs.txt` reads `42400000 43400000 3f000000 3f800000`, i.e.
the client's own vertex `(48.0, 192.0, 0.5, 1.0)`. **No `0x10fc` write occurs
anywhere in the points or psize draw span**, and `base-secs.txt` contains zero
`0x10fc` decodes only because its 2D traffic happens not to desync the parser.

This also explains why probing `0x10fc`, `0x1020`, `0x1024`, `0x102c`,
`0x1074`, `0x1098`, `0x10c0`, `0x10c8` on hardware kept coverage at one pixel:
none of them is the size, and no MMIO register is. The existing
`POINT_SIZE_PROBE` sweep in `parhelia_drm_test.c:4249` cannot succeed by
construction, because it binds `warp_passthrough`, which never writes `o10.z`,
so the extent never leaves the vertex engine.

---

## 7. How to program it

For this driver, which submits **window-space** vertices on the native point
path and uploads its own WARP program (`drm_parhelia_warp_program`), the
vendor's constant plumbing is optional — the constant file only exists to get a
number into `o10.z`.

1. **Quantise like the vendor.** `N = (int)(clamp(point_size, min, max) + 0.5f)`,
   `radius = 0.5f * (float)N`. `min`/`max` from `pipe_rasterizer_state` /
   `GL_POINT_SIZE_MIN/MAX`, `max` itself capped at `2048.0f`.
2. **Get `radius` into `o10.z`.** Upload a point program = the existing
   `warp_passthrough` bundles plus one bundle `mov o10.z, <radius>` before the
   `end`/`wra` tail. Two ways to source it:
   * **vendor-faithful:** put `radius` in `c[0x172].w` with the existing
     constant loader (`ctx->warp_constant_loader`, block load
     `41080013 | 4 | 0x172`, also writing `c[0x173] = (min, max, 0.5, -0.5)`)
     and emit `mov o10.z, c[0x172].wwww`. Only the constant changes per size,
     so the program is uploaded once.
   * **simplest:** re-upload a three-bundle-longer program per distinct size
     with `radius` as a literal, or append a sixth per-vertex dword and emit
     `mov o10.z, v<n>.x` — the latter would also give `GL_PROGRAM_POINT_SIZE`
     for free. `docs/parhelia-gl-limits.md`:94 shows the vendor has a
     per-vertex point-size attribute (key bit 28), so the port accepts a
     per-vertex value in principle; the one bringup run that tried a vertex
     register saw nothing, but that was the same single boot whose positive
     result was withdrawn, so it is not evidence.
   The kernel validator accepts all of this: output file index ≤ 11 covers
   `o10` (`parhelia_warp.c warp_destination_ok`), primary opcode `0x60` and
   secondary op `1` are both on the allow lists, and the vendor's own bundle
   `041802a4 80440200 b05a2124` has `IDXSEL = 0`, `f1i = 0` and no reserved
   bits set, so it passes as-is.
3. **Select the wide-point class.** `p3d_prim_tab[P3D_POINTS].cls` is hard-coded
   `0` in `reference/p3d_state.c:22`. For a point with `N > 1` it must be **3**,
   which makes `p3d_emit_state()` produce `0x1130 = 0x0300200c` and
   `p3d_dwg_nibble[3] = 0xc` produce `0x1300 = 0x00a60ecc` with no other
   change. Keep class 0 for `N == 1`: that is what the vendor does and what
   already works.
4. **Leave `0x12fc` alone** (`0x0bb80024`) for the wide point — see §4.
5. **Keep the CPU centring** (`ph_point_centre`) until §5's parity question is
   settled on hardware; if the hardware already applies `c[0x173].z/.w` the
   centres will be off by half a pixel and the fix is to drop the CPU bias.
6. Then `ph_stage_point` only needs to expand smooth and textured points; plain
   aliased wide points stop going through `ph_expand_point`.

---

## 8. The experiment that confirms it

Extend `parhelia-drm-test --point-probe` so that, instead of sweeping an MMIO
register, it:

* uploads a point program = `warp_passthrough` + `mov o10.z, <literal radius>`
  (`DRM_IOCTL_PARHELIA_WARP_PROGRAM`) and binds its entry,
* sets the wide-point class (`0x1130[25:24] = 3`, `0x1300` nibble `0xc`),
* draws one point at a known window position,
* counts covered pixels and prints the bounding box (the probe already does).

Sweep `radius` = 0.5, 1.0, 2.0, 4.0, 8.0. The prediction is **1, 4, 16, 64 and
256 pixels**, square, i.e. an `N × N` square for `N = 2·radius`. Two controls
worth running in the same boot: the same program with class 0 (expect one pixel
regardless of `radius`, per bringup §14.1), and the same class 3 with the
`o10.z` bundle removed (expect nothing).

A second, more vendor-faithful variant loads `c[0x172] = (1, 0, 0, radius)` and
`c[0x173] = (0, 2048, 0.5, -0.5)` through the constant loader and uses
`mov o10.z, c[0x172].wwww`; if the literal variant works and this one does not,
the difference is in the constant-loader sequence, not in the point path.

Nothing in this document was run on hardware by this analysis; §2.3 and §4 are
measurements from captures already in the tree, §2.1, §2.2, §3 and §5 are read
out of the vendor binaries.

---

## 9. Still open

* The exact operand mapping inside fragments `0xc4`/`0xc5` after `mtx2179`'s
  patches — i.e. whether the hardware expects `o10.z` in window pixels or in a
  scaled unit. The window-pixel reading is INFERRED from `c[0x172].w` being a
  pixel count on a path whose positions are already in window pixels, from the
  guard band `c[0x174] = ±8160` being documented in window pixels, and from the
  single observed run where `o10.z = 0.5` produced one pixel. The probe in §8
  settles it in one sweep.
* What `K[0x06]` bit 1 (size parity) actually changes in the generated program
  (§5).
* What distinguishes fragment `0xc4` from `0xc5` in GL terms — the selector is
  `K[0x48] == 3`, and `K[0x48]` is marked [OPEN] in
  `docs/parhelia-warp-codegen.md`:468. `0xc4` is the variant that reads three
  constants where `0xc5` reads one, which fits distance attenuation being
  active, but that is not shown.
* Whether `c[0x172].w` serves wide **lines** as well as points on the silicon
  (§2.3 shows the vendor writes it for `glLineWidth(8)`, but that capture's
  lines are CPU-expanded, so the write may be dead there).

---

## 10. Hardware measurement, 2026-09-13: every prediction in §2–§8 is refuted

The probe §8 asked for was run on the P690 (device 0x2539, SUN90) through
`parhelia-drm-test --point-probe` with `POINT_SIZE_WARP=1`. None of it
reproduces. Recorded here because the predictions above read as settled and
they are not.

**Method.** Untransformed screen-space vertices (x, y, r, g, b), the kernel's
passthrough WARP program with one extra secondary `mov <output>, c0.x` bundle
inserted before `end`, one point at (128,128), the target cleared to 0x204060
and every pixel that differs counted.

**Positive control, so a null result cannot be blamed on the harness.** The
same move aimed at the colour output reproduces the constant exactly:
`c0.x = 0.25` gives red 0x40, `0.50` gives 0x80, `1.00` gives 0xff. The
constant load and the secondary move both execute.

| what was predicted | what the hardware did |
|---|---|
| `o10.z` carries the point extent | radius 4.0 written to **every** output o1..o11, **every** component x/y/z/w: still exactly one pixel. No output changes coverage. |
| `c[0x172].w = 0.5*round(clamp(size))` sets the size | loaded with `c[0x173] = (1, 64, 0.5, -0.5)` at sizes 1, 2, 4, 8, 16: still one pixel, at every primitive class. |
| wide points are primitive class 3 (`0x1130[25:24] = 3`) | draws **nothing**: zero pixels, at every size and with every output written. |
| DWGCTL low nibble `0xc` selects wide points | draws nothing, alone or together with class 3. |
| the raster image might need to match | `0x12fc = 0x0bb80024` instead of the point value `0x04000024`, with and without class 3: no change either way. |

An unrelated fact the sweep turned up, worth knowing before writing a probe
that passes values through a vertex: **a vertex colour component above 1.0
discards the primitive outright**, triangles as well as points. The first
version of this sweep carried the radius in the red channel and read that as
"large radii are rejected". They were not; the vertex was. Sizes must reach
the engine as constants, which is also how the vendor delivers them.

**Polygon offset behaves the same way.** `c[0x178] = {factor, units, 0.5,
786432.0}` (`parhelia-block-state-map.md` §3.1) applied to the second of two
coplanar triangles under a LESS depth test never moves the surface: units from
-1.5e-5 to -6.2e-2 and factors -1/-4, flat and z-sloped, all leave the first
triangle owning every pixel. The depth-test controls in the same run pass — a
green triangle at z=0.4 wins every pixel and one at z=0.6 loses every pixel —
so the test, not the bias, is what works.

**What this points at.** Both features live in the `0x160..0x17f` fixed-function
constant range, and both are inert when the bound program is a passthrough.
`parhelia-block-state-map.md` §7 already flags "whether 0x178 d0/d1 is read by
the WARP program ... or as a point-size pair" as unresolved. The measurements
here are consistent with the whole range being read by the vendor's
fixed-function *vertex programs* rather than by setup hardware, which would
mean neither feature is available to a driver that compiles its own programs
without also generating the code that consumes these constants — and would
make "hardware point size" a misnomer. That is the open question; §9's list
stands, with §2–§8 demoted from CONFIRMED to refuted-on-P690.

Whether the original Parhelia (0x0527/0x0528, HSLPAR) differs is untested: no
0x0528 board was available. Per-family behaviour must not be assumed from this.

---

## 11. What the measurements in §10 were missing

A second pass over the vendor binaries, prompted by §10, found two errors in
§2–§8 rather than a hardware limitation. Wide points *are* rasterised by the
hardware; §10's probe asked for them the wrong way.

**Wide points are not expanded on the CPU.** The ICD's two point clippers,
`Point_to_Point` (`libGL .text+0xebfd0`) and `Point_to_Sizedpoint`
(`.text+0xebf10`), are byte-identical apart from one constant and **both
reserve exactly one output vertex per input point**. They differ only in the
primitive type stamped through `f3530`: `xor esi,esi` at `0xec001` against
`mov esi,0x9` at `0xebf5a`. Only two of the nine `f3530` call sites pass 9 —
that one and `Tri_to_Sizedpoint` at `0xecf2d`, i.e. `glPolygonMode(GL_POINT)`,
which takes the same hardware path. CONFIRMED.

**The extent is a bounding box in `o9`, not a scalar in `o10.z`.** The sized-
point program writes `o9 = (x - r, y - r, x + r, y + r)`. §10 swept a scalar
into every component of every output and found nothing because no single
component ever carries the extent. This supersedes §2.1's `o10.z` claim.

**The WARP class and the raster class are different.** `0x1130` and DWGCTL
carry class 3, but the WARP mode latch `0x10f0` and the bind packet stay at the
primitive's own class. §10 raised all of them together, which is why class 3
drew nothing there: the draw was dropped at the bind, not at the rasteriser.
The class chain itself (`mtx499[·] = 4` → `mtx878[4] = 3` → `0x1130[26:24] = 3`)
is unchanged.

**The wide/narrow condition is a plain inequality.** `libGL .text+0xc7a8d`
compares `glPointSize` against `1.0f` with no rounding and no `GL_POINT_SMOOTH`
term:

```
movss   xmm0,[rax+0x4]        ; GL state+0xad4 = glPointSize
ucomiss xmm0,[rip+...]        ; 0x367a0c = 1.0f
setne al ; setp dl ; or eax,edx
lea     eax,[rax+rax*8]       ; 0 -> 0, 1 -> 9
```

`ph_stage_point()` used `roundf(size) > 1.0f`, so `glPointSize(1.2)` took the
unit-point path where the vendor takes the wide one; it now matches. Wide and
antialiased **lines** do use a real threshold: `.text+0xea8ee` compares the
line width against `1.5f`, or `1.0f` under `GL_LINE_SMOOTH`. CONFIRMED.

**Correction to an earlier inference.** The sized point was described as
"primitive-table entry 1". The ICD's internal primitive id is **9**; the two
are separate enumerations joined by the kernel's translation tables
(`mtx2468`, `mtx1011`, `mtx3178`). The ICD mapping `GL mode -> internal prim`
is `{0,1,3,2,4,5,6,7,8,6}` at `.rodata+0x369880`, so `GL_POLYGON(9)` folds onto
`TRIANGLE_FAN(6)` and leaves slot 9 free for sized points. Two incidental
fixes in that table: `GL_LINE_LOOP(2) -> 3` and `GL_LINE_STRIP(3) -> 2` are the
reverse of what earlier docs state, and internal `0xd` is a "reject this draw"
sentinel guarded at `.text+0xd40f3`.

Polygon offset (§10's second half) is not answered by this: it remains untested
against a program that reads `c[0x178]`.

---

## 12. Hardware wide points confirmed on the P690, 2026-09-13

§11's reading is correct and measured. With the WARP mode latch and the bind
packet left at the primitive's own class, `0x1130[25:24] = 3`, and a program
that writes `o9 = (x - r, y - r, x + r, y + r)` from constant `c0.x`:

| radius | pixels covered | box | origin |
|---|---|---|---|
| 0.5 | 1 | 1x1 | 127,127 |
| 1.0 | 4 | 2x2 | 127,127 |
| 2.0 | 16 | 4x4 | 126,126 |
| 4.0 | 64 | 8x8 | 124,124 |
| 8.0 | 256 | 16x16 | 120,120 |

Exactly `(2r)^2`, centred on the vertex at (128,128). The point program is the
kernel passthrough plus two adds, `ADD o9.xy, v0.xyxy, -c0.xxxx` and
`ADD o9.zw, v0.xyxy, +c0.xxxx`.

Three things the sweep settles beyond the mechanism itself:

* **`0x1130` class 3 is the selector, not the DWGCTL nibble.** Nibble `0xc`
  on its own draws nothing; class 3 alone and class 3 with nibble `0xc` give
  identical coverage. `p3d_dwg_nibble[3] = 0xc` is harmless but is not what
  turns a point wide.
* **The raster image does not matter here.** `0x12fc = 0x0bb80024` (the value
  the captures carry) and `0x04000024` (the point value this driver writes)
  produce identical boxes at every radius. The open item about which value a
  point should carry is therefore not a correctness question for wide points.
* **Raising the WARP class with the raster class is what §10 got wrong.** The
  draw was dropped at the bind, not at the rasteriser, which is why class 3
  appeared to draw nothing there.

The `c[0x172].w` rows printed after this sweep in the same run are not
evidence: the block file still holds the previous iteration's `c0`, and the
passthrough program does not write `o9`, so those draws report leftover state.
The constant-file account of point size (§2.2) remains refuted; the size
reaches the setup unit only as the `o9` box a program writes.
