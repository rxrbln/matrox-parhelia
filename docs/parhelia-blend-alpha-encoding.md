# Parhelia-512 blend and alpha-test register encoding

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Complete bitfield and value encoding for the two Parhelia fragment back-end
registers an OpenGL 1.x renderer needs beyond the single measured value:

* `0x12e8` **BLENDCTL** — blend enable, four blend factors, blend equation, dither
* `0x12e4` **ALPHATEST** — alpha test enable, compare function, reference value
* `0x12ec` / `0x12f0` **BLENDCONST01 / BLENDCONST23** — the constant blend colour

Everything here was read out of vendor **binaries** and cross-checked against the
existing hardware captures. No vendor C source was consulted.

Tags on every claim:

* **[KRN offset]** — `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`,
  `.text` offsets (unstripped, symbols `mtxNNNN`). This is the Linux kernel
  module that composes the register words our driver replaces, so it is the
  primary source.
* **[ICD offset]** — `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0`
  (x86-64; VMA = file offset). The GL entry points and the GL-enum translators.
* **[PARD offset]** — `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`
  (i386 PE, VMA; file = VMA − 0x10000). The Windows D3D driver, used only as an
  independent second witness.
* **[HW]** — the measured values quoted in the task and in
  `docs/parhelia-gl-state-attribution.md`.
* **[INF]** — inference; the reasoning is always given.

Prior art this builds on, confirms and in two places corrects:
`docs/parhelia-pixel-pipeline-complete.md` §3.2/§3.3 (PPC, derived from the
Windows binary), `docs/parhelia-slot-translation.md` (ST), and
`docs/parhelia-fragment-ops.md` (FO, the ICD side). The value of this document
over those is that the **Linux kernel** composition path is now read directly,
end to end, and every factor code is confirmed by two independent binaries.

---

## 0. Summary — what to implement

```c
/* engine register = 0x1000 + index*4; opcode-0 packets carry the index byte */
#define PARHELIA_ALPHATEST      0x12e4  /* index 0xb9 */
#define PARHELIA_BLENDCTL       0x12e8  /* index 0xba */
#define PARHELIA_BLENDCONST01   0x12ec  /* index 0xbb */
#define PARHELIA_BLENDCONST23   0x12f0  /* index 0xbc */

/* hardware blend-factor enum, 4 bits, same encoding in all four factor fields */
enum parhelia_blend_factor {
	PBF_ZERO		= 0x0,
	PBF_ONE			= 0x1,
	PBF_CONST_COLOR		= 0x2,
	PBF_INV_CONST_COLOR	= 0x3,
	PBF_SRC_COLOR		= 0x4,
	PBF_INV_SRC_COLOR	= 0x5,
	PBF_SRC_ALPHA		= 0x6,
	PBF_INV_SRC_ALPHA	= 0x7,
	PBF_DST_COLOR		= 0x8,
	PBF_INV_DST_COLOR	= 0x9,
	PBF_DST_ALPHA		= 0xa,
	PBF_INV_DST_ALPHA	= 0xb,
	PBF_SRC_ALPHA_SAT	= 0xc,
	/* 0xd not emitted by any vendor path */
	PBF_CONST_ALPHA		= 0xe,
	PBF_INV_CONST_ALPHA	= 0xf,
};

/* hardware compare-function enum, 3 bits: a predicate mask,
 * bit0 = EQUAL, bit1 = LESS, bit2 = GREATER.
 * Shared by alpha test (0x12e4), depth and stencil (0x12c8). */
enum parhelia_cmp_func {
	PCF_NEVER = 0, PCF_EQUAL = 1, PCF_LESS = 2, PCF_LEQUAL = 3,
	PCF_GREATER = 4, PCF_GEQUAL = 5, PCF_NOTEQUAL = 6, PCF_ALWAYS = 7,
};

/* hardware blend-equation enum, 3 bits */
enum parhelia_blend_eq {
	PBE_ADD = 0, PBE_SUBTRACT = 1, PBE_REVERSE_SUBTRACT = 2,
	PBE_MIN = 5, PBE_MAX = 6,
};

static inline uint32_t
parhelia_blendctl(int enable, unsigned srcRGB, unsigned dstRGB,
		  unsigned srcA, unsigned dstA, unsigned eq,
		  int dither_supported, int dither_on)
{
	uint32_t v = (!!enable)
		   | ((srcRGB & 0xf) << 1)  | ((dstRGB & 0xf) << 5)
		   | ((srcA   & 0xf) << 9)  | ((dstA   & 0xf) << 13)
		   | ((eq     & 0x7) << 17) | ((eq     & 0x7) << 20);
	if (dither_supported)			/* see §1.3 */
		v |= (1u << 23) | (dither_on ? 0 : (1u << 24));
	return v;
}

static inline uint32_t
parhelia_alphatest(int enable, unsigned func, unsigned ref8)
{
	return (!!enable) | ((func & 0x7) << 1) | ((ref8 & 0xff) << 8);
}
```

Verification against the two measured values [HW]:

| state | computed | measured |
|---|---|---|
| blend off, ONE/ZERO both pipes, ADD, dither on | `parhelia_blendctl(0,1,0,1,0,0,1,1)` = `0x00800202` | `0x00800202` |
| `GL_SRC_ALPHA`/`GL_ONE_MINUS_SRC_ALPHA` | `parhelia_blendctl(1,6,7,6,7,0,1,1)` = `0x0080eced` | `0x0080eced` |
| alpha test off, ALWAYS, ref 0 | `parhelia_alphatest(0,7,0)` = `0x0000000e` | `0x0000000e` |
| `glAlphaFunc(GL_GREATER, 0.5f)` | `parhelia_alphatest(1,4,0x80)` = `0x00008009` | `0x00008009` |

All four reproduce bit for bit.

---

## 1. `0x12e8` BLENDCTL — PROVEN

### 1.1 Bitfields

```
bit   0      blend enable            (see §1.2 for the two force-off conditions)
bits  4:1    source      RGB   factor
bits  8:5    destination RGB   factor
bits 12:9    source      ALPHA factor
bits 16:13   destination ALPHA factor
bits 19:17   blend equation (alpha pipe)
bits 22:20   blend equation (RGB pipe)   -- always written with the same value
bit  23      dither unit active   \  see §1.3
bit  24      dither disable       /
bits 31:25   0
```

**Evidence.** `mtx2480` (@`0x129e0`) is the Linux kernel's blend / alpha / mask /
scissor state validator. `%rbp` holds the fragment-back-end shadow object whose
dword at `+0x10` is the `0x12e8` image (PPC §3.1 gives the flush header
`0xbbbcb9ba` = `{0x12e8, 0x12e4, 0x12f0, 0x12ec}`; the Windows object offsets
`+0x0c/+0x18/+0x14/+0x10` are the same fields, shifted by 4 because the Linux
dirty pointer is 8 bytes). The four factors are packed in one straight-line
block [KRN 0x12be7–0x12c41]:

```
12be7:  movzbl 0x54(%rsp),%edx ; movzbl 0x11(%rbp),%eax
        and $0xf,%edx ; and $0xffffffe1,%eax ; add %edx,%edx ; or %edx,%eax
        mov %al,0x11(%rbp)          ; srcAlpha -> bits [12:9]
12c00:  movzbl 0x58(%rsp),%edx ; movzbl 0x10(%rbp),%eax
        and $0xf,%edx ; add %edx,%edx ; and $0xffffffe1,%eax ; or ...
        mov %al,0x10(%rbp)          ; srcRGB   -> bits [4:1]
12c16:  mov 0x4c(%rsp),%edx ; mov 0x10(%rbp),%eax
        and $0xf,%edx ; shl $0xd,%edx ; and $0xfffe1fff,%eax ; or ...
        mov %eax,0x10(%rbp)         ; dstAlpha -> bits [16:13]
12c2d:  mov 0x50(%rsp),%edx ; movzwl 0x10(%rbp),%eax
        and $0xf,%edx ; shl $0x5,%edx ; and $0xfe1f,%ax ; or ...
        mov %ax,0x10(%rbp)          ; dstRGB   -> bits [8:5]
```

The equation is written twice from the same translator result [KRN 0x12c45–0x12c7f]:

```
12c45:  mov 0x37(%r12),%esi ; call mtx3082
        movzbl 0x12(%rbp),%ebx ; and $0x7,%eax ; shl $0x4,%eax
        and $0xffffff8f,%ebx ; or %eax,%ebx ; mov %bl,0x12(%rbp)  ; bits [22:20]
        and $0xfffffff1,%ebx
        mov 0x37(%r12),%esi ; call mtx3082
        and $0x7,%eax ; add %eax,%eax ; or %eax,%ebx
        mov %bl,0x12(%rbp)                                        ; bits [19:17]
```

Two calls, one input — **the hardware has no separate RGB/alpha blend equation**,
so `GL_EXT_blend_equation_separate` cannot be exposed. (It *does* have separate
RGB/alpha *factors*, so `GL_EXT_blend_func_separate` can.) This independently
reproduces ST's reading of the same fields.

### 1.2 The enable bit is not simply the GL enable

`mtx2480` clears bit 0 in two cases beyond `glDisable(GL_BLEND)`:

1. **Identity suppression** [KRN 0x12b6c–0x12b80, 0x138e2–0x13916]:
   ```
   12b6c:  cmpb $0x0,0x36(%r12)   ; blend enabled at all?
           je   12b85             ;  no -> off
           cmpl $0x2,0x3b(%r12)   ; srcAlpha == D3DBLEND_ONE?
           je   138e2
           mov  $0x1,%edx         ; -> on
   138e2:  cmpl $0x2,0x3f(%r12)   ; srcRGB == ONE ?
           cmpl $0x1,0x43(%r12)   ; dstAlpha == ZERO ?
           cmpl $0x1,0x47(%r12)   ; dstRGB == ZERO ?
           mov  0x37(%r12),%eax ; dec %eax ; cmp $0x1,%eax ; jbe 12b85
   ```
   i.e. `src = ONE, dst = ZERO` on both pipes with equation `ADD` or `SUBTRACT`
   is a no-op, and the driver turns the blender off rather than program it.
   Purely an optimisation; a driver may skip it. Matches [PARD 0x9c5de].

2. **Raster-op gate** [KRN 0x12b8e–0x12b9c]:
   ```
   12b92:  movzbl 0x18(%r15),%eax ; and $0xfffffff0,%eax ; cmp $0xc0,%al ; je ...
   ```
   `%r15+0x18` is the engine shadow's **DWGCTL** image and the ROP occupies its
   bits `[7:4]` — established independently in `docs/parhelia-2d-accel-complete.md`
   §4 (`DWGCTL = (dstSlot<<10) | (srcSlot<<8) | (BOP<<4) | opcode`, shadow `+0x18`),
   and the logic-op translator `mtx1588` writes its result into the same nibble
   [KRN 0x12b1b–0x12b46]. **Blending is only honoured when `0x1300` DWGCTL has
   `BOP == 0xC` (GXcopy).** Matches [PARD 0x9c617].

   Cross-check against the capture: the 3D baseline `0x1300 = 0x00a60ec4`
   (`docs/parhelia-gl-state-encoding.md` §4.6) has bits `[7:4] = 0xc`, so the
   gate is open — consistent with the measured `0x0080eced` having bit 0 set [HW].

### 1.3 Dither bits `[24:23]` are format-gated

[KRN 0x12a67–0x12aa0, 0x13920, 0x137a0]:

```
12a67:  mov 0x28(%r13),%rax ; mov (%rax),%edx        ; render-target format id
        lea -0xf(%rdx),%eax ; cmp $0x1,%eax ; seta %cl
        cmp $0x1e,%edx      ; setne %al
        test %cl,%al ; je 137a0                      ; fmt in {0x0f,0x10,0x1e}
12a84:  cmpb $0x0,0x1c(%r12) ; je 13920              ; dither disabled?
12a90:  orb $0x80,0x12(%rbp) ; andb $0xfe,0x13(%rbp) ; bit23=1, bit24=0
13920:  orb $0x80,0x12(%rbp) ; orb $0x1,0x13(%rbp)   ; bit23=1, bit24=1
137a0:  andb $0x7f,0x12(%rbp) -> 12a94: andb $0xfe,..; bit23=0, bit24=0
```

| bits [24:23] | meaning |
|---|---|
| `01` | dithering **enabled** (the measured `0x00800202` / `0x0080eced` case) |
| `11` | dithering **disabled** |
| `00` | render-target format does not dither (internal format ids `0x0f`, `0x10`, `0x1e` — the 10-bit-per-component "GigaColor" pair and one 16-bit format) |

So bit 23 reads as "dither unit engaged" and bit 24 as "dither off"; the pair is
never `10`. PPC listed the no-dither set as `{0x0f, 0x10, 0x1e, 0x2f}`; the Linux
kernel tests only `{0x0f, 0x10, 0x1e}` — **correction to PPC §3.2**, `0x2f` is not
in the Linux gate.

A driver that renders to an ordinary 8-bit-per-component target and wants
dithering writes bit 23 and clears bit 24, exactly as the captures show.

### 1.4 Factor codes — the complete table

The hardware nibble enum is listed in §0. It is confirmed by **two independent
binaries** that were never compiled from the same source tree:

* Linux: `mtx642` @`0x10110`, jump table `.rodata+0x5e0`, 18 entries, index =
  the D3D-numbered factor enum, out-params `(*rdx = RGB code, *rcx = alpha code)`
  [KRN 0x10110–0x10250].
* Windows: the translator @`0x9a5f0`, jump table `0x9a7d4`, 17 entries, index =
  `D3DBLEND − 1`, same two out-params [PARD 0x9a5f0–0x9a7d1].

Both produce byte-identical results. Table (intermediate enum = D3DBLEND
numbering, which is what the Linux ICD hands the kernel):

| D3D enum | name | RGB-pipe code | alpha-pipe code | KRN | PARD |
|---|---|---|---|---|---|
| 1 | ZERO | `0x0` | `0x0` | 0x10233 | 0x9a616 |
| 2 | ONE | `0x1` | `0x1` | 0x10160 | 0x9a7bd |
| 3 | SRCCOLOR | `0x4` | `0x6` | 0x1022b | 0x9a62d |
| 4 | INVSRCCOLOR | `0x5` | `0x7` | 0x10223 | 0x9a644 |
| 5 | SRCALPHA | `0x6` | `0x6` | 0x10210 | 0x9a65b |
| 6 | INVSRCALPHA | `0x7` | `0x7` | 0x101fd | 0x9a672 |
| 7 | DESTALPHA | `0xa` | `0xa`, or `0x1` if the RT has no alpha (§1.5) | 0x101ee | 0x9a6b7 |
| 8 | INVDESTALPHA | `0xb` | `0xb`, or `0x0` if the RT has no alpha | 0x101da | 0x9a70a |
| 9 | DESTCOLOR | `0x8` | `0xa` | 0x101ca | 0x9a689 |
| 10 | INVDESTCOLOR | `0x9` | `0xb` | 0x101ba | 0x9a6a0 |
| 11 | SRCALPHASAT | `0xc` | `0x1` | 0x101b2 | 0x9a74a |
| 12 | BOTHSRCALPHA | handled by the caller, not by `mtx642`: src `0x6` / dst `0x7` on the affected pipe | | 0x13afc (RGB), 0x13ae7 (alpha) | see PPC §3.2 |
| 13 | BOTHINVSRCALPHA | src `0x7` / dst `0x6` | | 0x13b1b (RGB), 0x13a96 (alpha) | see PPC §3.2 |
| 14 | BLENDFACTOR | `0x2` | `0xe` | 0x101a9 | 0x9a761 |
| 15 | INVBLENDFACTOR | `0x3` | `0xf` | 0x101a1 | 0x9a778 |
| 16 | (constant alpha) | `0xe` | `0xe` | 0x10191 | 0x9a78f |
| 17 | (inv constant alpha) | `0xf` | `0xf` | 0x10181 | 0x9a7a6 |
| — | out of range | `0x1` | `0x1` | 0x10160 | 0x9a7bd |

The alpha-pipe column is a self-consistency proof of the enum reading: wherever
the RGB pipe gets a *colour* factor the alpha pipe gets the corresponding
*alpha* factor (`SRC_COLOR 4 → SRC_ALPHA 6`, `DST_COLOR 8 → DST_ALPHA 0xa`,
`CONST_COLOR 2 → CONST_ALPHA 0xe`), and `SRC_ALPHA_SAT` becomes `ONE` on the
alpha pipe — which is exactly what both the GL and the D3D specification
require of the alpha channel.

### 1.5 Destination-format constraint on `DST_ALPHA`

The **only** way the render-target format restricts the factor set:

```
10110:  mov 0x8(%rdi),%rax ; mov 0x230(%rax),%rax ; mov 0x198(%rax),%rax
        mov (%rax),%edi    ; mov $0x1,%esi ; call mtx3921
        movzbl 0x12(%rax),%edx           ; the per-format "no destination alpha" byte
...
101ee:  test %dl,%dl ; je 10246
        movl $0xa,(%rbx) ; -> alpha pipe = 0x1     ; DEST ALPHA,   no dst alpha
10246:  movl $0xa,(%rbx) ; -> alpha pipe = 0xa     ; DEST ALPHA,   dst alpha present
101da:  test %dl,%dl ; je 1023b
        movl $0xb,(%rbx) ; -> alpha pipe = 0x0     ; INV DEST ALPHA, no dst alpha
1023b:  movl $0xb,(%rbx) ; -> alpha pipe = 0xb     ; INV DEST ALPHA, dst alpha present
```

`mtx3921` @`0x80630` is a linear search over the runtime format table `mtx2688`
(400 records × `0x24` bytes, built by `mtx1646` @`0x7c050` through `mtx1179`
@`0x7bf60`), keyed on `(format id, view index)`; the blend path always asks for
view index 1. Byte `+0x12` of that record is the flag. Parsing every
`mtx1179` call in `mtx1646` shows exactly four view-1 records with `+0x12 = 1` —
format ids `0x07`, `0x09`, `0x0c`, `0x0e` — each of which shares its hardware
surface-format code with a sibling id that has `+0x12 = 0`
(`0x06/0x07 → 0x24`, `0x08/0x09 → 0x26`, `0x0b,0x0d / 0x0c,0x0e → 0x32`). That is
the classic "A-variant / X-variant" pairing, so `+0x12 = 1` means *this render
target carries no destination alpha* [INF, from the pairing] — and the
substitution it triggers (`DST_ALPHA → 1.0`, `ONE_MINUS_DST_ALPHA → 0.0`) is
exactly the value a missing destination alpha must read as, which settles the
sense of the flag [PROVEN by construction].

The Windows driver does the same thing and adds one more sub-case
[PARD 0x9a6b7–0x9a747]:

| RT has dst alpha | chip flag `[dev+0x1fc]` | DESTALPHA → (rgb, alpha) | INVDESTALPHA → (rgb, alpha) |
|---|---|---|---|
| yes | — | `(0xa, 0xa)` | `(0xb, 0xb)` |
| no | set | `(0xa, 0x1)` | `(0xb, 0x0)` |
| no | clear | `(0x1, 0x1)` | `(0x0, 0x0)` |

The Linux kernel only ever emits the middle row, i.e. it assumes the
"flag set" behaviour: on the **RGB** pipe the codes `0xa`/`0xb` are left in place
and the hardware itself supplies 1.0 for the missing destination alpha. A
from-scratch driver on a no-alpha target is safest emitting the third row
(`ONE`/`ZERO` in both pipes), which is correct under either hardware behaviour.

Note the asymmetry: `DESTCOLOR`/`INVDESTCOLOR` are **not** corrected — their
alpha-pipe codes stay `0xa`/`0xb` even on an alpha-less target [KRN 0x101ca,
0x101ba].

Nothing else in `mtx2480` couples the blend state to a surface format.

### 1.6 Equation codes

`mtx3082` @`0x100d0`, jump table `.rodata+0x5b0`, index = the D3D blend-op enum:

| D3DBLENDOP | hardware code |
|---|---|
| 1 ADD | `0` |
| 2 SUBTRACT | `1` |
| 3 REVSUBTRACT | `2` |
| 4 MIN | `5` |
| 5 MAX | `6` |
| 0 / out of range | `0` |

Codes `3` and `4` are not emitted by any vendor path. Matches [PARD 0x9a8f0,
table 0x9a928].

**`MIN` and `MAX` force all four factors to `ONE`** [KRN 0x12bb6–0x12be6]:

```
12bb6:  mov 0x37(%r12),%eax ; sub $0x4,%eax ; cmp $0x1,%eax ; ja 139e8
12bc7:  movl $0x1,0x58(%rsp) ; ... 0x54 ; 0x50 ; 0x4c        ; all four = ONE
```

i.e. the four factor fields are not consulted for `MIN`/`MAX`; the driver must
still write `ONE` into all of them. `139e8` is the normal path that runs the
four `mtx642` translations.

---

## 2. `0x12e4` ALPHATEST — PROVEN

### 2.1 Bitfields

```
bit   0      alpha test enable
bits  3:1    compare function (same 3-bit predicate enum as depth/stencil)
bits  7:4    0
bits 15:8    reference value, 8 bits unsigned, 0x00 = 0.0, 0xff = 1.0
bits 31:16   0  (the Linux kernel writes zero; MTXPARD writes 0xff in [31:24])
```

**Evidence** [KRN 0x12aa6–0x12af4, disable path 0x13790]:

```
12aaa:  cmpb $0x0,0x2d(%r12) ; je 13790          ; alpha test enable flag
12ab6:  movzbl 0x1c(%rbp),%ebx ; or $0x1,%ebx    ; bit 0 = 1
13790:  movzbl 0x1c(%rbp),%ebx ; and $0xfffffffe,%ebx   ; bit 0 = 0
12abd:  mov %bl,0x1c(%rbp)
12ac0:  mov 0x32(%r12),%esi ; call mtx1344       ; GL/D3D func -> hw code
12acd:  mov %ebx,%edx ; and $0x7,%eax ; add %eax,%eax
        and $0xfffffff1,%edx ; or %eax,%edx
        mov %dl,0x1c(%rbp)                       ; func -> bits [3:1]
12adc:  mov 0x2e(%r12),%eax
        movb $0x0,0x1e(%rbp) ; movb $0x0,0x1f(%rbp)   ; bits [31:16] = 0
        mov %al,0x1d(%rbp)                       ; ref  -> bits [15:8]
```

Both branches fall through to the same func/ref programming, so the function and
reference fields are always valid even while the test is disabled — which is why
the disabled default is `0x0000000e` (`func = ALWAYS`) and not `0`.

### 2.2 Compare-function codes

`mtx1344` @`0xff10`, jump table `.rodata+0x4e8`, 9 entries, index = the D3D
compare enum (which the ICD produces as `GLenum − 0x1ff`, §3.2). The codes form
a **3-bit predicate mask: bit0 = EQUAL, bit1 = LESS, bit2 = GREATER**:

| GL enum | D3DCMP | hardware code | body |
|---|---|---|---|
| `GL_NEVER` 0x200 | 1 | `0` | 0xff26 |
| `GL_LESS` 0x201 | 2 | `2` | 0xff30 |
| `GL_EQUAL` 0x202 | 3 | `1` | 0xff36 |
| `GL_LEQUAL` 0x203 | 4 | `3` | 0xff41 |
| `GL_GREATER` 0x204 | 5 | `4` | 0xff47 |
| `GL_NOTEQUAL` 0x205 | 6 | `6` | 0xff51 |
| `GL_GEQUAL` 0x206 | 7 | `5` | 0xff57 |
| `GL_ALWAYS` 0x207 | 8 | `7` | 0xff61 |
| — | 0 / out of range | `2` (LESS) | 0xff20 |

The same function serves depth and stencil, so `0x12c8`'s depth func `[3:1]` and
stencil func `[7:5]` use this identical enum. This confirms ST's and PPC's
reading from a third code path, and explains the already-decoded `0x12c8`
transition `…e4 → …eb` [HW]: `0xe4` low nibble `4` = `bit0` clear (test off),
`func = 2` = `LESS`; `0xeb` = `1 | (5<<1)` = enabled, `func = 5` = `GEQUAL`.

### 2.3 Reference value

8 bits, unsigned, in bits `[15:8]`, computed by the ICD as
`(int)(ref * 255 + 0.5)` — round-half-up, not truncation — so
`glAlphaFunc(GL_GREATER, 0.5f)` gives `0x80`. Already established in FO §6
(`libGL` @0x114bd3, ICD state slot 0x18) and unchanged here; the new fact is
that it lands in `0x12e4[15:8]` rather than a register of its own.

> **Correction to FO §3.** FO's slot table lists "`0x1060` | 0x18 | alpha
> reference" and "`0x1064` | 0x19 | alpha compare func" under the
> `reg = 0x1000 + slot*4` rule that `parhelia-3d-integrated.md` §8 refuted.
> There are no such registers: `0x18` and `0x19` are ICD **state slots**, and
> the kernel folds both of them, plus slot `0x0f` (enable), into the single
> register `0x12e4`.

---

## 3. GL → hardware, end to end

The Linux vendor stack converts in two hops: the ICD maps the GL enum to a
D3D-numbered intermediate and ships it to the kernel as a state record, and the
kernel maps that to the register nibble. A from-scratch Mesa driver collapses
both hops into one table; the composed result is what follows.

### 3.1 Blend factors

`glBlendFunc(sfactor, dfactor)` [ICD 0x124750] validates `sfactor` with the
**source** translator @`0x11d4b0` and `dfactor` with the **destination**
translator @`0x11d5e0`, raising `GL_INVALID_ENUM` (0x500) if either rejects
[ICD 0x124798], and stores `(GL enum, D3D code)` pairs at ICD state block
`ctx+0x1028 +0x0c/+0x10` (srcRGB), `+0x14/+0x18` (srcA), `+0x1c/+0x20` (dstRGB),
`+0x24/+0x28` (dstA) — `glBlendFunc` writes the RGB and alpha pairs with the
same value, `glBlendFuncSeparateEXT` @`0x124820` writes them separately.

The two translators accept exactly the GL 1.1 + `EXT_blend_color` legal sets;
`GL_SRC_COLOR`/`GL_ONE_MINUS_SRC_COLOR` are rejected as a source and
`GL_DST_COLOR`/`GL_ONE_MINUS_DST_COLOR`/`GL_SRC_ALPHA_SATURATE` as a
destination, matching the specification.

**Composed table — this is the table to implement.** All nine ioquake3 source
factors and all eight destination factors are covered:

| GL factor | valid as | D3D | RGB-field code | alpha-field code |
|---|---|---|---|---|
| `GL_ZERO` (0) | src, dst | 1 | `0x0` | `0x0` |
| `GL_ONE` (1) | src, dst | 2 | `0x1` | `0x1` |
| `GL_SRC_COLOR` (0x300) | dst | 3 | `0x4` | `0x6` |
| `GL_ONE_MINUS_SRC_COLOR` (0x301) | dst | 4 | `0x5` | `0x7` |
| `GL_SRC_ALPHA` (0x302) | src, dst | 5 | `0x6` | `0x6` |
| `GL_ONE_MINUS_SRC_ALPHA` (0x303) | src, dst | 6 | `0x7` | `0x7` |
| `GL_DST_ALPHA` (0x304) | src, dst | 7 | `0xa` | `0xa` (`0x1` if no dst alpha) |
| `GL_ONE_MINUS_DST_ALPHA` (0x305) | src, dst | 8 | `0xb` | `0xb` (`0x0` if no dst alpha) |
| `GL_DST_COLOR` (0x306) | src | 9 | `0x8` | `0xa` |
| `GL_ONE_MINUS_DST_COLOR` (0x307) | src | 10 | `0x9` | `0xb` |
| `GL_SRC_ALPHA_SATURATE` (0x308) | src | 11 | `0xc` | `0x1` |
| `GL_CONSTANT_COLOR_EXT` (0x8001) | src, dst | 14 | `0x2` | `0xe` |
| `GL_ONE_MINUS_CONSTANT_COLOR_EXT` (0x8002) | src, dst | 15 | `0x3` | `0xf` |
| `GL_CONSTANT_ALPHA_EXT` (0x8003) | src, dst | 16 | `0xe` | `0xe` |
| `GL_ONE_MINUS_CONSTANT_ALPHA_EXT` (0x8004) | src, dst | 17 | `0xf` | `0xf` |

Source translator bodies [ICD]: `0x11d58c` ZERO, `0x11d5a4` ONE, `0x11d580`
SRC_ALPHA, `0x11d542` INV_SRC_ALPHA, `0x11d5ba` DST_ALPHA, `0x11d594`
INV_DST_ALPHA, `0x11d522` DST_COLOR, `0x11d5c5` INV_DST_COLOR, `0x11d516`
SRC_ALPHA_SAT, `0x11d570`/`0x11d59c`/`0x11d5af`/`0x11d562` the four EXT
constants, `0x11d4e6` reject.
Destination translator bodies [ICD]: `0x11d6bc` ZERO, `0x11d6cc` ONE,
`0x11d6b0` SRC_COLOR, `0x11d6df` INV_SRC_COLOR, `0x11d672` SRC_ALPHA,
`0x11d652` INV_SRC_ALPHA, `0x11d6ea` DST_ALPHA, `0x11d646` INV_DST_ALPHA,
`0x11d6a0`/`0x11d6c4`/`0x11d6d4`/`0x11d692` the EXT constants, `0x11d616` reject.

Worked `0x12e8` values for the modes ioquake3 actually issues (dither on,
equation `ADD`, blend enabled, alpha-capable render target):

| `glBlendFunc` | `0x12e8` |
|---|---|
| `GL_ONE, GL_ONE` (additive) | `0x00802223` |
| `GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA` | `0x0080eced` [HW, matches the capture] |
| `GL_DST_COLOR, GL_ZERO` (modulate) | `0x00801411` |
| `GL_DST_COLOR, GL_ONE` | `0x00803431` |
| `GL_ONE, GL_ONE_MINUS_SRC_ALPHA` (premultiplied) | `0x0080e2e3` |
| `GL_SRC_ALPHA, GL_ONE` | `0x00802c2d` |
| `GL_ZERO, GL_SRC_COLOR` | `0x0080c081` |
| `GL_SRC_ALPHA_SATURATE, GL_ONE` | `0x00802239` |

### 3.2 Alpha function and blend equation

`glAlphaFunc` [ICD 0x11d480] validates `GL_NEVER..GL_ALWAYS` (`enum − 0x200 ≤ 7`)
and stores the raw GL enum at `ctx+0x1028+0x04`. The flush @`0x113b90` remaps it
through the jump table at `.rodata 0x36bca4` (bodies `0x1152f9`, `0x11529c`,
`0x1152a6`, `0x115305`, `0x11530f`, `0x115319`, `0x115325`, `0x114b8f`), which
emit `1,2,3,4,5,6,7,8` for indices `0..7` — i.e. the mapping is exactly
`D3DCMP = GLenum − 0x1ff` — and sends it as ICD state slot `0x19`. Combined with
`mtx1344` this gives the GL → hardware column in §2.2. The identical table at
`0x36bce4` serves `glStencilFunc`, and `0x36bcc4` `glDepthFunc`.

`glBlendEquation` [ICD 0x11d700] accepts `{0x8006 ADD, 0x8007 MIN, 0x8008 MAX,
0x800A SUBTRACT, 0x800B REVERSE_SUBTRACT}` (the mask `test $0x37,%al` over
`enum − 0x8006` excludes `0x8009`) and stores the raw enum at `+0x40`. The flush
remaps it through `.rodata 0x36bd04` [ICD 0x114dac–0x114dcb] to the D3D blend-op
enum `ADD→1, MIN→4, MAX→5, SUBTRACT→2, REVSUBTRACT→3`. Composed with `mtx3082`:

| GL equation | hardware code |
|---|---|
| `GL_FUNC_ADD` | `0` |
| `GL_FUNC_SUBTRACT` | `1` |
| `GL_FUNC_REVERSE_SUBTRACT` | `2` |
| `GL_MIN` | `5` |
| `GL_MAX` | `6` |

---

## 4. `0x12ec` / `0x12f0` — the constant blend colour — PROVEN

Four **10-bit unsigned fixed-point** components, `trunc(f * 1023.0)`, two per
register [KRN 0x12c88–0x12ce8]:

```
12c90:  movss 0x4b(%r12),%xmm1 ; mulss .LC23,%xmm1 ; cvttss2si %xmm1,%esi
        ... 0x4f -> ecx ; 0x53 -> edx ; 0x57 -> eax
12cc8:  mov %si,0x16(%rbp)     ; float[0] -> 0x12ec bits [31:16]
12cd4:  mov %cx,0x14(%rbp)     ; float[1] -> 0x12ec bits [15:0]
12cd8:  mov %dx,0x1a(%rbp)     ; float[2] -> 0x12f0 bits [31:16]
12cdc:  mov %ax,0x18(%rbp)     ; float[3] -> 0x12f0 bits [15:0]
```

`.LC23` is `1023.0f` (`.rodata.cst4 + 0x9c`, file offset `0xc7fa0`, bytes
`00 c0 7f 44`) — verified directly, not assumed. The conversion truncates; it
does **not** round half-up the way the alpha reference does.

```
0x12ec = (R10 << 16) | G10
0x12f0 = (B10 << 16) | A10        R,G,B,A each 0..1023
```

The component order is `R,G,B,A` because `glBlendColor` [ICD 0x128bc0] clamps
each argument and stores them in that order at `ctx+0x1028 + 0x30/0x34/0x38/0x3c`
(`xmm0`→`+0x30`, `xmm1`→`+0x34`, `xmm2`→`+0x38`, `xmm3`→`+0x3c`), and the kernel
shadow's four consecutive floats `+0x4b/+0x4f/+0x53/+0x57` are that block
[INF, strong: the offsets are consecutive and in the same order].

The RGB pipe reads this colour through factor codes `0x2`/`0x3`
(`CONST_COLOR`) and its alpha component through `0xe`/`0xf` (`CONST_ALPHA`);
the alpha pipe uses `0xe`/`0xf` for both, per the table in §1.4. Both measured
captures have `0x12ec = 0x12f0 = 0` [HW], the GL default `(0,0,0,0)`.

Matches [PARD 0x9c7b0–0x9c7f4, constant `0x19c9b8 = 1023.0`]; PPC §3.2 had the
packing but marked the component order `[INF]` — it is now anchored on
`glBlendColor`'s argument order.

---

## 5. Dependencies on other registers — the complete answer

| register | does blend / alpha test depend on it? |
|---|---|
| `0x1300` DWGCTL | **Yes, for blend.** The enable bit is force-cleared unless the ROP nibble `[7:4]` is `0xC` (GXcopy) — §1.2. Alpha test is unaffected. |
| render-target format (`0x1224 + 8*slot`) | **Yes, twice.** It selects the dither encoding in `0x12e8[24:23]` (§1.3) and it substitutes the `DST_ALPHA`/`ONE_MINUS_DST_ALPHA` alpha-pipe factors (§1.5). It does **not** restrict any other factor. |
| `0x12fc` RASTERMODE | **No.** `mtx2480` writes it from a different shadow object with its own dirty bit (PPC §3.1 group `0x20000000`); there is no code path in which a blend or alpha-test field reads or writes it. |
| `0x12c8` depth/stencil | **No.** Separate shadow object, separate dirty bit `0x04000000`, no cross-reference. |
| fragment combiner program | **No.** Every fragment operation in this document is dedicated fixed-function state; changing a blend mode or alpha reference never recompiles or re-uploads a fragment program (FO §1, CONFIRMED against the live capture). |
| `0x1324` colour write mask | **No** — independent, though obviously it can mask away what the blender produced. |

One ordering note that is *not* a dependency but matters in practice: the four
registers are flushed as one DMA packet, header `0xbbbcb9ba` = indices
`{0xba, 0xb9, 0xbc, 0xbb}` = `{0x12e8, 0x12e4, 0x12f0, 0x12ec}` (PPC §3.1, and
the same group appears in the live capture as `reglist(6)`), so a driver that
writes blend state should write all four together.

---

## 6. PROVEN vs INFERRED

**PROVEN** — read directly out of the instruction stream, and where noted
confirmed by a second, independently compiled binary:

* `0x12e8` bitfield layout, all seven fields [KRN 0x12be7–0x12c7f].
* `0x12e4` bitfield layout, all three fields [KRN 0x12aa6–0x12af4, 0x13790].
* The 16-entry hardware blend-factor enum and every D3D→(rgb, alpha) row
  [KRN `mtx642` 0x10110–0x10250 **and** PARD 0x9a5f0–0x9a7d1, byte-identical].
* The 8-entry compare-function enum [KRN `mtx1344` 0xff10–0xff66].
* The 5-entry blend-equation enum [KRN `mtx3082` 0x100d0–0x10107].
* `MIN`/`MAX` force all four factors to `ONE` [KRN 0x12bb6–0x12be6].
* The identity (`ONE`/`ZERO`, `ADD`|`SUBTRACT`) enable suppression
  [KRN 0x12b6c, 0x138e2].
* The DWGCTL `BOP == 0xC` gate on the enable bit [KRN 0x12b92], with the ROP
  nibble position corroborated by `parhelia-2d-accel-complete.md` §4 and by the
  captured `0x1300 = 0x00a60ec4`.
* Dither `[24:23]` three-state encoding and the `{0x0f, 0x10, 0x1e}` format gate
  [KRN 0x12a67, 0x12a90, 0x13920, 0x137a0].
* Blend constants: four 10-bit components, `trunc(f * 1023.0)`, packing
  `0x12ec = (c0<<16)|c1`, `0x12f0 = (c2<<16)|c3`; the scale factor read out of
  `.rodata.cst4+0x9c` [KRN 0x12c88–0x12ce8].
* GL enum → D3D intermediate for blend factors, alpha/depth/stencil functions
  and blend equations [ICD 0x11d4b0, 0x11d5e0, 0x11d480, 0x11d700, 0x114dac,
  jump tables `0x36bca4`/`0x36bcc4`/`0x36bce4`/`0x36bd04`].
* `glBlendColor` argument order R,G,B,A into the ICD state block
  [ICD 0x128bc0].
* Arithmetic verification: the layout above reproduces all four measured
  hardware values bit for bit (§0).

**INFERRED** — reasoning given, no instruction proves it outright:

* That format-record byte `+0x12 = 1` means "render target carries no
  destination alpha". The *substitution it causes* proves the semantics
  (`DST_ALPHA → 1.0` is only correct when the alpha is absent); the mapping of
  the four flagged format ids `{0x07, 0x09, 0x0c, 0x0e}` to specific pixel
  layouts comes from their pairing with same-hardware-code siblings.
* That the kernel shadow floats `+0x4b/+0x4f/+0x53/+0x57` are R,G,B,A in that
  order. Anchored on `glBlendColor`'s store order and on the offsets being
  consecutive, but the slot dispatcher that copies ICD record → shadow was not
  traced.
* Which behaviour a no-alpha render target actually has on the **RGB** pipe.
  Linux leaves `0xa`/`0xb` in place; Windows has a second, chip-dependent path
  that rewrites them to `ONE`/`ZERO`. Emitting `ONE`/`ZERO` in both pipes is
  correct under either reading and is what this driver should do.

**OPEN:**

* Hardware factor code `0xd` — no vendor path emits it.
* Blend-equation codes `3` and `4` — likewise unused.
* Whether `0x12e4[31:24]` has any effect. The Linux kernel writes `0`, MTXPARD
  writes `0xff` (PPC §3.3 reads it as a mask). Writing `0` is proven safe by the
  capture.
