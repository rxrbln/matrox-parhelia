Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# The maximum texture size, read out of the vendor binaries

This is a static audit of the retained vendor drivers.  No hardware was
touched, no X server was started and no driver source was changed.  It answers
one question: **what does Matrox's own software believe the largest sampler
image is, per chip family, and is there a register field that extends texel
addressing past 512 that our driver does not program?**

Tags: **PROVEN** means read from machine code at the cited address.
**INFERRED** means derived from proven facts but not executed.  Windows
addresses are image VAs (image base `0x10000`; `.text` VMA `0x10300` at file
offset `0x300`).  Linux ELF addresses are VMAs of the file as `objdump` prints
them.

## Evidence identity

| file | role | SHA-256 |
|---|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpsogl.dll` (`PSOGL`) | **P690 OpenGL ICD**, bound to `102b:2539` | `db0ab86fd25fe2cfb37bae5252d825aac8a2cc16d88191dedb165d4ba57dbcf0` |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` (`PSER`) | P690 display driver | `52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll` (`OGL`) | Parhelia OpenGL ICD | `41b2317309801a917506c29c2b5998713ddb9bdfa3d26c623ab56617d0c1d848` |
| `matroxdriver-x86_64-cvs-20090511/opengl6/libGL.so.1.3.0` (`LGL`) | Linux ICD, all cards in one binary | `4e760f5fcff26fcbc5e51edd597265495b758d6ee4bc524d1a45270ad8333b1c` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (`KRN`) | Linux kernel HAL | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |
| `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` (`DRV`) | Linux X driver | `8f5ca1d85f1c13fda731c8a0ba28db063ec0e697b7bc5dfff300c1500d564e4f` |

`mtxpser.inf` lines 37-50 bind `PSOGL`/`PSER` to `PCI\VEN_102B&DEV_2539` (and
`2537`/`2538`), and line 93 registers `mtxpsogl.dll` as the ICD for that
adapter.  So `PSOGL` is the **P690's own** OpenGL driver, not a Parhelia one
reused.

## 1. The number is 2048, and it is the same on both families  **PROVEN**

### 1.1 The ICD rejects anything larger, it does not clamp it

`LGL` validates `glTexImage2D` twice - once per entry point - and both read the
same way:

```
147503  test  r10d,r10d                      ; proxy target?
14750c  cmp   r13d,0x800                     ; width  > 2048
147516  seta  dl                             ; ... or
147519  shr   eax,0x1f                       ;     height negative
14751e  jne   147732                         ;  -> GL_INVALID_VALUE
147524  cmp   r12d,0x800                     ; height > 2048 -> same
147531  lea   eax,[r13-0x1] ; test r13d,eax  ; width  power of two?
14753e  lea   eax,[r12-0x1] ; test r12d,eax  ; height power of two?
147581  cmp   r14d,0xb                       ; level <= 11
1474ea  mov   DWORD PTR [rax+0x4],0x501      ; GL_INVALID_VALUE
```

The second copy is at `0x14a28d`/`0x14a29f`, reached through a shared tail that
first subtracts `2 * border` from the dimension under test
(`0x14a27f`-`0x14a28b`), so the 2048 there bounds the *interior* size; its
error store is at `0x14a2c9` and its level check at `0x14a2da`.

The Windows ICDs carry the identical sequence, at the identical VA, in both
families:

| | `PSOGL` (P690) | `OGL` (Parhelia) |
|---|---|---|
| size `> 0x800` -> error | `103d2250` | `103d2230` |
| power-of-two test | `103d2266` | `103d2246` |
| `GL_INVALID_VALUE` store | `103d2297` | `103d2277` |
| `level > 0xb` -> error | `103d22a3` | `103d2283` |

The one difference from the Linux ICD is that on Windows the power-of-two test
is skipped when a context byte is set (`cmp BYTE PTR [ecx+0x1a9],0x0 ; jne`),
i.e. non-power-of-two images are a switchable extension there.  **The 2048
bound is not conditional on anything in either binary.**

### 1.2 The advertised limit

`LGL`'s `glGetIntegerv` ladder compares the enum and answers with an immediate:

```
13b94c  cmp   r12d,0xd33          ; GL_MAX_TEXTURE_SIZE
13b953  je    13df0b
13df0b  mov   DWORD PTR [r13+0x0],0x800     ; = 2048
```

`GL_MAX_3D_TEXTURE_SIZE` (`0x8073`, `0x13bced`) and
`GL_MAX_CUBE_MAP_TEXTURE_SIZE` (`0x851c`, `0x13c546`) are the same 2048, and
`docs/parhelia-gl-limits.md` §3.2 already lists the whole table.

### 1.3 There is no per-device-id texture limit anywhere

* No PCI device id (`0x0527`, `0x0528`, `0x2537`, `0x2538`, `0x2539`) appears
  as an immediate in `LGL`'s disassembly at all: the Linux ICD is one binary
  for every card in the package (`docs/readme.txt` lists Parhelia, P750, P690,
  P650, APVe) and cannot vary a limit per chip.
* `DRV` does know the ids (`3dbb2`, `3eab6`, `45709`, … `cmp $0x2539`), and
  `MTXOpenGLFeature` (`DRV 0x8a7a0`) is the only OpenGL gate it has: it
  requires `SFAGetSWFeature(...) & 0x40`, a board word `[+0x3988] > 0x96`, and
  a subsystem id `!= 0xc1102b`.  It is a yes/no switch for the whole feature,
  never a size.
* The P690 display driver `PSER` and the Parhelia one `MTXPARD` are different
  builds (2010 vs 2009) with **the same histogram of `mov <mem>,imm32` stores
  for 2048, 512 and 4096** (63/158/44 in both), and the 2048 stores land on
  the same structure offsets (`[esi+0x504]`, `[esi+0x5d8]`, `[esi+0x27c]`, …).
  No 512 constant is bound to a texture dimension in either.

## 2. How the vendor programs a texture wider than 512  **PROVEN**

It just writes the number.  The kernel HAL's descriptor writer `mtx3424`
(`KRN .text+0x1eab0`) is the only place `0x11dc`/`0x11e0`/`0x11e4` are packed,
and its field widths are:

| value | code | field |
|---|---|---|
| width | `1eac3  and r14w,0x3fff` -> `1ecb2`/`1ecb9` | `0x11e0[13:0]`, **14 bits** |
| height | `1eab5  and r9w,0x3fff` -> `1ecbd`/`1eccd` | `0x11e4[13:0]`, **14 bits** |
| depth | `1eb16  and r9w,0xfff` -> `1ecc3`/`1ecc6` | `0x11e4[27:16]`, 12 bits |

and the surface sizer `mtx1444` (`.text+0x5b900`) reads them back with exactly
the same masks (`5b972`, `5b999`) before multiplying by the row granularity.
The pitch function `mtx3441` (`.text+0x5ba40`) masks the width with `0x3fff`
too (`5ba46`) and only saturates above **`0x1fff`**, where it substitutes 8192
texels (`5ba4b`-`5ba57`).

**There is no second width register, no high-address-bit field, no
coordinate-scale and no shifted width.**  The three transformations
`mtx3424` does apply are all understood and none of them is size related:

1. `1eb48  lea eax,[r15+0x1f] ; and ax,0x3fe0` - writes `align(width, 32)`
   instead of the raw width, but only when the surface record's flag bit 2 is
   set (`1eb69  test BYTE PTR [rax],0x4`).
2. `1eb92`/`1ebab`/`1ebc4` - width, height and depth are written **minus two**
   when the border flag (bit 4) is set; the hardware adds the border ring.
3. `1ecfa`-`1ed13` - for surface **type 3 (strided)** only, the width field is
   forced to the literal `0x20` and the height field receives `width >> 5`
   (precomputed at `1eb39`).  That is the flat/strided buffer encoding, not a
   large-2-D-texture mode: the 2-D, cube and volume class builders pass types
   0, 1 and 2 (`docs/parhelia-p690-volume-cube-targets.md`).

Our own emitter (`reference/p3d_state.c` `p3d_emit_state`) writes
`(width & 0x3fff) | texelcode<<16 | type<<19 | fmt<<24` and
`(height & 0x3fff) | depth<<16 | max_level<<28`, which is byte for byte what
`mtx3424` produces for a 2-D surface without a border.

## 3. Is there an unimplemented texture-addressing mode?  **No candidate found**

The per-unit register window is closed: `mtx1381` (`KRN .text+0x3f680`) emits
exactly eleven registers per unit in three groups, and every one of them is
accounted for in `docs/parhelia-texture-sampling.md` §1.1.  The remaining
unknowns in that window are

* `0x11cc`/`0x11d0` - colour key and mask,
* `0x11d4`/`0x11d8` - bump matrix, zero on every vendor path,
* `0x11e8` - texture-cache partition, a table indexed by the number of active
  stages,

none of which carries an address bit.  In the unit selector
`0x1200 + 4*unit`, `[11:8]` is the constant `1` on every vendor path
(`or ...,0x40100`, `docs/parhelia-fragment-isa.md` §11.3) and `[18:16]` is the
coordinate mode with only three values in use - 4 for two coordinates, 5 for
projective, 6 for three (`docs/parhelia-p690-texture-pipes-re.md` §222).

The storage-order field `0x11e0[18:16]` selects between six layouts (§5.3 of
the sampling doc) and changes the *pitch rule*, not the addressable extent.
`mtx3441` is explicit about it: tiled codes 2/3/4/5 use
`bpp * next_pow2(max(w, 32)) / 8` and linear codes 0/1 use
`bpp * align(w, 32) / 8`.  (Note for the record: the P690 measurement in
`docs/parhelia-p690-mip-chain-half-tiles.md` proves `align(w, 32)` for the
tiled order this chip uses, so the SUN90 address unit differs from this
Parhelia-era formula for non-power-of-two widths.  The two rules agree for
every power-of-two width, so this cannot be what wraps a 1024-wide image.)

## 4. Does the vendor work around a size limit?  **It does not, for textures**

No path in `LGL`, `PSOGL`, `OGL`, `KRN`, `PSER` or `MTXPARD` splits, tiles,
downsamples or re-pitches a *sampler image* because of its size.  Above 2048
the ICD returns `GL_INVALID_VALUE` and that is the end of it (§1.1).

Two genuine chunking routines exist and are worth naming so they are not
mistaken for one:

* `MTXPARD 0x47270` / `PSER 0x487d0` - identical code.  A destination rectangle
  is clamped to the surface, and if it is wider than 2048 **or** taller than 64
  the chunk size becomes `align(w, 32) x align(h, 32)`, which is then halved
  repeatedly while `w*h > 0x20000` (131072) or either axis exceeds 2048,
  counting the divisions in two fields.  The `2048 x 64 = 0x20000` shape says
  this sizes a **staging/scratch working set**, not a texture.  **INFERRED**:
  the enclosing operation was not identified.
* `PSER 0x158efb` - a three-tier emitter selected by the surface's own
  `0x3fff`-masked width and height: `> 0xfe0` (4064) takes `0x1563f0`,
  `> 0x800` (2048) takes `0x157640`, and otherwise the inline path runs.  Two
  further fields are tiered at `0x1000` (4096).  So 2048 is a real internal
  boundary in the P690 display driver, but for a coordinate encoding rather
  than for addressability.  **INFERRED** which DDI entry point this is.

## 5. What this means for the measured 512-texel wrap

**Settled the same day, in favour of candidate 2 below: the wrap was ours.**
`--mip-address-probe` programmed a fixed `screen_w = screen_h = 512` clip
window while its colour target followed the image under test, so the part of
a 1:1 quad past 512 pixels was clipped and never drawn; the check then counted
those undrawn destination pixels as texels the sampler had fetched wrong.  With
the clip window following the target, 528-, 640-, 1024- and 2048-wide chains
are byte exact and the GL harness passes 44 of 44 checks at
`GL_MAX_TEXTURE_SIZE 2048`.  The cap is back at 2048 (Mesa `907e6fb`), and
this document's reading of the vendor binaries - 2048 on both families, 14-bit
fields, no missing register - is what the hardware does.  The rest of the
section is kept as written, because its reasoning is what pointed at our own
code rather than at the silicon.

The measurement in `docs/parhelia-p690-texture-corruption-investigation.md` §1
- bit 9 of both the x and the y texel coordinate dropped, for tiled and for
linear storage alike - is **not explainable by anything in the vendor
binaries**, and it contradicts them:

* Matrox's own P690 ICD accepts and draws 1024x1024 and 2048x2048 images and
  advertises `GL_MAX_TEXTURE_SIZE` 2048 (§1).
* The descriptor field really is 14 bits wide in the vendor's own packer, and
  the vendor's sizer and pitch function both read 14 bits back (§2).
* There is no register we are failing to write that could extend addressing,
  because the per-unit window is fully enumerated (§3).

So exactly one of the following must hold, and the binaries cannot decide
between them:

1. **The SUN90 address unit really is narrower than the Parhelia one.**  The
   descriptor packer and the sizer above are the *Parhelia-era* Linux HAL.  If
   `0x11e0[13:9]` is not implemented on `0x2539`, the vendor's 2048 cap would
   simply be wrong on this chip - possible, but it would mean Matrox shipped a
   P690 ICD that draws every large texture wrong, which is implausible.
2. **The 512 wrap is on our side**, either in the sampler state we emit or -
   more likely, because it reproduces in linear storage too - in the *upload*,
   i.e. in the code that decides where texel (x, y) is written.  A GL-level
   probe cannot tell "the sampler fetched the wrong texel" from "the texel was
   stored in the wrong place", and that is precisely the distinction the
   earlier nine-bit-coding false alarm already cost a round on.

Note that our `max_texture_2d_size = 512` for `0x2539` therefore rests on a
measurement whose two candidate causes have not been separated.

## 6. Experiments that would settle it

In rough order of cost, all on the P690, none requiring a GL client:

1. **Read the descriptor back.**  Write `0x11e0` with width 640, 1024 and 2048
   (and `0x11e4` likewise) and read the register.  If the high bits come back
   clear, `[13:9]` is not implemented on SUN90 and question 5.1 is answered in
   one command.  If they read back intact, the field exists and the wrap is
   downstream.
2. **Separate upload from sampling.**  With `parhelia-drm-test`, write a coded
   1024x1024 image through `ph_mip_texel_offset_cpp()`, then (a) read the VRAM
   bytes back through the host aperture and decode them with the same function,
   and (b) sample the same image 1:1 into a render target.  (a) failing is an
   upload bug; (a) passing and (b) failing is the address unit.  Do it for
   linear and for tiled storage.
3. **Ask the hardware what size it thinks it has.**  Store the same 1024-wide
   image twice and sample it with two descriptors that differ only in the width
   field, 1024 and 512.  If the two sampled results are identical, the unit is
   using 512 regardless of what we wrote.
4. **Find the exact breakpoint.**  Sweep the width over 496, 512, 528, 544,
   640, 768, 1024, 1536, 2048 at a fixed height of 64 and the height likewise.
   A break exactly at 512 in both axes points at a coordinate field; a break at
   the first width whose `next_pow2` differs from `align(w, 32)` points at the
   pitch rule of §3; a break where `width * height * 4` crosses 2 MiB points at
   an offset-width limit and would show in y alone.
5. **Sweep the two fields whose meaning is only inferred.**  `0x11e8` between
   the kernel reset default `0x00100000` and the vendor table value
   `0x00500000`, and `0x1200 + 4*unit` `[11:8]` over 0..15 with a 1024-wide
   image bound.  Cheap, and it closes §3 by experiment rather than by
   enumeration.
6. **Re-check the harness once more.**  `tools/parhelia-gl-q3formats-test.c`
   now codes x and y in ten bits each (`code_of`, `CODE_BITS 24`), which covers
   1024 exactly, but the file comment above `CODE_BITS` still says "x:9 y:9"
   and `test_base_level` still decodes its diagnostics with nine-bit shifts
   (`(g >> 9) & 0x1ff`, around line 915).  Those are stale and will misreport a
   coordinate above 512 in the `base` group's messages even when the comparison
   itself is right.  The 1024x1024 case also needs a window at least
   1024x1024 (`win_w`/`win_h` are capped at 1024, so a 1280x1024 screen is the
   minimum), or the readback covers rows that were never drawn.
