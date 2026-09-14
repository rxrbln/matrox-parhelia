# Parhelia Fragment Anti-Aliasing (FAA-16x) — reverse engineering

Tags: **[C]** = confirmed, instructions or source read, address given; **[I]** =
inferred, self-consistent with the evidence but not proven; **[V]** = verbatim
vendor source comment; **[OPEN]** = not recovered.

Sources: `matroxdriver-x86_64-cvs-20090511/` (Linux — kernel sources,
`kernel/mtx_parhelia.o`, `xserver/7_4_0/mtx_drv.so`, `opengl6/lib64/libGL.so.1.3.0`)
and `xddm32_210_00_105_se_u_nowhql/MtxPar/` (Windows XPDM 2.10.00.105 —
`MTXPARD.dll`, `MTXOGL.dll`, `MTXPARM.sys`).  MTXPARD image base 0x00010000,
`.text` VMA 0x10300 = file 0x300; all MTXPARD addresses below are VMAs and match
the labels in `scratchpad/pard.asm` directly.

Companions: `parhelia-rendertarget.md` (§4 covers the Linux GL multisample side
from the capture direction and agrees with §6 here), `parhelia-gl-feature-map.md`,
`parhelia-gl-driver.md`, `parhelia-3d-state-complete.md`,
`parhelia-2d-accel-complete.md`, `parhelia-fragment-isa.md`,
`parhelia-variants.md`.

> **Namespace warning.**  Two distinct index namespaces appear below.  The
> `0x2121xxxx`-tagged **DMA packets** (§4.2) really do address MMIO as
> `0x1000 + index*4`.  The ICD's **region-2 state records** (§6.4, §7) do **not**
> — per the 2026-09-05 correction in `parhelia-gl-driver.md` their indices are GL
> state-slot ids.  Nothing in this document maps a state slot to a register.

---

## 0. The architecture answer, up front

**Parhelia's AA is a coverage/fragment buffer, not supersampling and not classic
"N full colour samples per pixel" multisampling.**  Matrox's own term in the
driver source is *"sort-independent antialiasing with the fragment buffer"*
[V, `kernel/src/MtxParhlParms.c:69,73`].  Decomposed:

- A **fragment** is the A-buffer sense: a pixel only partially covered by a
  primitive produces a side record (colour + depth + a coverage mask) written to
  a **separate fragment buffer**, not to the framebuffer; fully covered interior
  pixels take the ordinary single-sample path.  That is why the feature is "16x"
  while costing far less than 16× fill: only edge pixels pay.  The **separate
  buffer is proven** — its allocator, size formula and failure policy are in §3.
  The "16" is proven as a *sample count* (the driver reports 16 samples in the
  D3D caps, §2.6, and both GL ICDs hard-code `GL_SAMPLES = 16`, §6.1); that the
  16 subsamples are laid out as a 4×4 grid, and the exact bit layout of a
  fragment record, are **[OPEN]** — no code in these binaries reveals either.
- **Sort-independent** is the period's D3D vocabulary
  (`D3DANTIALIAS_SORTINDEPENDENT`): fragments accumulate, so primitives need not
  be submitted in depth order.  A **depth threshold** decides when two fragments
  at one pixel belong to the same surface and may be merged — that is
  `ThresholdAA` (§2.5).
- A **resolve** merges the fragment buffer back into the framebuffer.  It is an
  explicit driver-issued operation ("Send a FRAG_RESOLVE" [V]), triggered on
  render-target change and on device teardown (§5).

**Confidence summary.**  Recovered exactly and verified: the whole configuration
surface (§2), the `AAMode` bitfield decode (§2.6), the **fragment-buffer size
formula, clamps, retry policy and allocation descriptor** (§3), the AA
enable/disable command (§4.2), the resolve trigger and call graph (§5), the GL
`ARB_multisample` mapping on both platforms (§6), the line/point/polygon
smoothing behaviour (§7), and the restriction set (§8).  Still **[OPEN]**: the
register *index* that carries the AA control word and the AA threshold, and the
register that carries the fragment-buffer base address (§4.3).

**Recommendation for an open driver: do not implement FAA.**  Advertise
`GL_SAMPLE_BUFFERS = 0`, exactly as both vendor ICDs do.  The vendor's own Linux
stack never enables it (§2.3), and the Windows XPDM driver ships with it **off by
default** (`AAMode = 0x54`, §2.6).  The Windows binary does select its FAA path
for P690-class devices (§8), but this is not a safe programming reference until
the remaining opaque lower-level operation is captured.

---

## 1. Where the feature lives

| component | role |
|---|---|
| `kernel/src/MtxParhlParms.c` | declares the Linux module parameters — **source, readable** |
| `kernel/mtx_parhelia.o` | registers the same switches (`mtx722` @0x74950); **never reads them** [C] |
| `xserver/7_4_0/mtx_drv.so` | same switch table (`mtx724` @0x60d70); no consumer [C] |
| `opengl6/lib64/libGL.so.1.3.0` | multisample **state tracking only**, no hardware path [C] |
| `MtxPar/MTXOGL.dll` | multisample state tracking **plus** a command-stream emitter [C] |
| **`MtxPar/MTXPARD.dll`** | **the only binary that implements FAA** |
| `MtxPar/MTXPARM.sys` | contains no AA string or knob at all [C, negative] |

The knobs are Matrox *registry switches* in section `"3D"` — on Windows
`HKR\3D.AAMode` etc. under the adapter service key, the same `<Section>.<Name>`
form as `3D.D3DEnable` in `mtxpar.inf:177` [C].  On Linux the same values are
module parameters named `reg3D<Name>`.

---

## 2. The configuration surface — [C], complete

### 2.1 The knobs, with the vendor's own comments [V]

```c
/* Select the AA settings */
MTX_MODULE_PARM_DECL(reg3DAAMode, 0x52);                       /* line 62  */

/* Select the threshold values for sort-independent antialiasing with the
   fragment buffer (default value will be 0x0) */
MTX_MODULE_PARM_DECL(reg3DThresholdAA, 0x007FE000);            /* line 71  */
MTX_MODULE_PARM_DECL(reg3DFragSize,    0x3F800000);            /* line 75  */

/* Force a different AA mode when destination alpha is enabled */
MTX_MODULE_PARM_DECL(reg3DForceCompressedMask, TRUE);          /* line 202 */

/* Do not execute PLNWTMASK hack in FAA mode */
MTX_MODULE_PARM_DECL(reg3DForcePLNWMASK4FAA, TRUE);            /* line 205 */

/* Send a FRAG_RESOLVE even if alphablending was enable when rendering the scene
 * and no fragments should be cumulated */
MTX_MODULE_PARM_DECL(reg3DResolveAlphaBlendedFrags, FALSE);    /* line 212 */

/* Resolve due to stencil op changing */
MTX_MODULE_PARM_DECL(reg3DResolveOnStencilOpChange, TRUE);     /* line 215 */

MTX_MODULE_PARM_DECL(reg3DAdjustZ, FALSE);                     /* line 208 */
```

`FragSize` inherits `ThresholdAA`'s comment by copy-paste; it carries no
information of its own.

### 2.2 Compiled-in defaults, per platform  [C]

The registry-switch constructors carry their own defaults, and they differ from
`MtxParhlParms.c` **and between platforms**.

| switch | Linux blob | Windows `MTXPARD` | `MtxParhlParms.c` |
|---|---|---|---|
| `AAMode` | **0x52** | **0x54** (`push 0x54` @0x10973D) | 0x52 |
| `ThresholdAA` | **0x007FFFFF** | 0x007FFFFF (@0x1097AC) | 0x007FE000 |
| `FragSize` | **4.0f** (0x40800000) | **4.0f, or 5.4f on some silicon** (@0x109823) | 1.0f |
| `ForceCompressedMask` | 1 | 1 | TRUE |
| `ForcePLNWMASK4FAA` | 1 | *absent from MTXPARD* | TRUE |
| `ResolveAlphaBlendedFrags` | 0 | 0 | FALSE |
| `ResolveOnStencilOpChange` | 1 | 1 | TRUE |
| `AdjustZ` | 0 | 0 | FALSE |

Linux sites (`mtx_parhelia.o`, ctor `mtx722` @0x74950; identical table also at
`mtx1049` @0x765f0): 0x762d4 `mov r9d,0x52`; 0x76270 `mov r9d,0x7fffff`;
0x76300 `mov r9d,0x40800000`; then 0x76329 / 0x76352 / 0x763ca / 0x763a4 /
0x7637b for the booleans.  Member slots in the config object: `AAMode` +0x00,
`ThresholdAA` +0x08, `FragSize` +0x10, `ForceCompressedMask` +0x18,
`ForcePLNWMASK4FAA` +0x20, `ResolveAlphaBlendedFrags` +0x28, `AdjustZ` +0x30,
`ResolveOnStencilOpChange` +0x38.  Section string `"3D"` = `.rodata.str1.1+0x59`.

Windows: one ctor `sub_109704` builds a 0x45C-byte settings object stored at
`pDev+0xA680`; slots `AAMode` +0x00, `ThresholdAA` +0x04, `FragSize` +0x08,
`ForceCompressedMask` +0x0C, `ResolveAlphaBlendedFrags` +0x10, `AdjustZ` +0x14,
`ResolveOnStencilOpChange` +0x18.  A second wrapper (`sub_8F5C0`, object at
`pDev+0xA67C`) exposes each as an accessor whose `vtbl[3]` is *GetValue*:
`AAMode` → **+0x28C**, `ThresholdAA` → **+0x290**, `FragSize` → **+0x294**,
`ForceCompressedMask` → **+0x298**, `AdjustZ` → +0x29C,
`ResolveAlphaBlendedFrags` → +0x2A0, `ResolveOnStencilOpChange` → +0x2A4.

`FragSize`'s Windows default is chip-dependent (@0x109823–0x10983C) [C]:
`(caps & 0x0007FC00) == 0 ? 0x40800000 (4.0f) : 0x40ACCCCD (5.4f)`.

`ForcePLNWMASK4FAA` **does not exist in `MTXPARD.dll`** — it is a Linux-only
leftover [C, negative].

### 2.3 On Linux the knobs are dead  [C, negative]

- `libGL.so.1.3.0`: the byte strings `AAMode`, `ThresholdAA`, `FragSize`,
  `reg3D` **do not occur at all**.  The ICD has no registry machinery and never
  asks the kernel for an AA capability.
- `mtx_parhelia.o`: the constants `0x7fffff` and `0x40800000` occur **only** at
  the two constructor sites; the literal `0x007FE000` never occurs.
- `mtx_drv.so`: registers the switches; exports no `FAA`/`AntiAlias`/`Resolve`/
  `Frag` symbol; the string `FAA` appears in no Linux binary.

**Nothing in the Linux vendor stack ever turns FAA on.**  The values are
inherited from the shared Matrox core that the Windows D3D driver builds against.

### 2.4 Two knobs that describe the hardware

- **`ForceCompressedMask` — "Force a different AA mode when destination alpha is
  enabled" [V].**  The coverage mask therefore has (at least) two storage
  encodings, and the default one **collides with destination alpha**.  The
  guard is visible in code: `test BYTE PTR [surf+0x10],0x8` — the format's
  destination-alpha bit — at MTXPARD 0x9FBE4, 0x9B60B, 0x9B6FD, 0x9B711 [C].
  The natural reading is that the uncompressed coverage mask occupies bits the
  alpha channel would otherwise use [I].  `ForceCompressedMask` is programmed
  into the AA control word's bits[6:5] (§4.1).
- **`ForcePLNWMASK4FAA` — "Do not execute PLNWTMASK hack in FAA mode" [V].**
  PLNWT is the Matrox plane write-mask, on Parhelia **`0x1324`, index 0xC9**
  (`parhelia-2d-accel-complete.md` §CORRECTION 1).  A PLNWT workaround used
  elsewhere must be suppressed under FAA, i.e. **FAA needs the full plane write
  mask** [I] — consistent with the alpha/coverage-channel reading.

### 2.5 `reg3DThresholdAA`  [I]

Both defaults are, to within a bit, **half of the 24-bit depth range**:
`0x007FE000 / 0xFFFFFF = 0.49951`, `0x007FFFFF / 0xFFFFFF = 0.50000`.

The value is written verbatim (no shift, no mask) into the AA-threshold register
shadow, MTXPARD 0x13973A [C]:

```
139725: mov ecx,[edx+0x290]     ; the ThresholdAA accessor
139734: call [eax+0xc]          ; GetValue -> eax
139737: mov ecx,[edi+0x2c]      ; the render-state block
13973a: mov DWORD PTR [ecx+0xa4],eax
```

[I] It is a **depth tolerance in S8Z24 units**: fragments at one pixel whose
depths differ by less than this are treated as the same surface and merged; a
default of "half the Z range" is effectively "always merge".  A competing
reading the bit patterns also fit is an **IEEE-754 mantissa mask**
(`0x007FFFFF` is exactly the float mantissa field, `0x007FE000` its top 10 bits)
used to quantise Z before the comparison.  Both readings agree the parameter
controls *fragment merging tolerance*.  Which one — **[OPEN]**.

### 2.6 `AAMode` — DECODED  [C]

Three consumers pin the encoding down.

**(a) Per-application override — `sub_58410` @0x58410** [C].  The effective mode
is `AAMode` with its low nibble optionally replaced from a per-application
profile record (`sub_F2ED0`, record+0x04, values 1..5):

```
58446: jmp DWORD PTR [ecx*4+0x58478]        ; table = 5844d,58456,5845f,58465,5846e
  ov=1 -> and eax,~0xD ; or 2   -> nibble 2
  ov=2 -> and eax,~0xC ; or 3   -> nibble 3
  ov=3 -> and eax,~0xF          -> nibble 0
  ov=4 -> and eax,~0xE ; or 1   -> nibble 1
  ov=5 -> and eax,~0xB ; or 4   -> nibble 4
```

So **`AAMode[3:0]` is the AA level selector**; the upper bits are global flags a
per-app profile cannot change.

**(b) D3D multisample caps — `sub_86F90` @0x86F90**, called @0x87C5B with the
effective `AAMode` [C]:

```
86fa1: shr eax,1 ; and eax,7
       == 0 -> edx = 0x0008
       == 1 -> edx = 0x8000
       else -> edx = 0        (and the caps are cleared for every format)
86fb9: or edx,1
       ; store edx into both WORDs at entry+0x14/+0x16 of each 0x20-byte
       ; surface-format record, for `count` records
86fe4: if (entry.format == 0x23) entry.caps &= ~0x8
```

`0x0009` = bit0 (1 sample) + bit3 → **4 samples**; `0x8001` = bit0 + bit15 →
**16 samples**, in the DX8 `D3DMULTISAMPLE_TYPE` bitmask convention (bit n ⇒
n+1 samples).  Format tables are selected by silicon at 0x87BF7
(`test eax,0x7FC00`): 0x1D19A0 vs 0x1D1560, count `[this+0x75C]`, stride 0x20.

**(c) Fragment-buffer gate — `sub_9FBB0` @0x9FBB0** (§3.1) requires
`(AAMode & 0x0E) == 2` **and** `(AAMode & 1)`, with one exception for
destination-alpha surfaces [C].

Putting it together:

| `AAMode[3:0]` | bit0 | bits[3:1] | D3D caps | fragment buffer | UI index |
|---|---|---|---|---|---|
| 0 | 0 | 0 | 4 samples | no | 3 |
| 1 | 1 | 0 | 4 samples | no | 4 |
| 2 | 0 | 1 | 16 samples | only via the dest-alpha path | 1 |
| 3 | 1 | 1 | 16 samples | **yes — true FAA-16x** | 2 |
| ≥4 | – | ≥2 | none | no | 5 (off) |

**bit0 = "use the fragment buffer"; bits[3:1] = 0 → 4x, 1 → 16x, ≥2 → AA off.**
(The UI-index column is the inverse map computed by the telemetry path
`sub_58170` @0x58203, which packs it with `TEFilteringOptions`/`FlipOnVBlank`
for `sub_F1BA0`.)

Upper flag bits [C]:

| bit | site | meaning |
|---|---|---|
| **0x10** | 0x9FBD6 `test al,0x10` | master "fragment buffer allowed" gate (overridable per render target by `rt[0x360] & 0x40`) |
| **0x40** | 0x9FC18 `test al,0x40` | fragment-buffer size **scales with resolution**; clear ⇒ flat size |

Plus a driver-wide kill switch: at 0x139784 and 0x13978A,
`if (ds:0x1D2048 == 0) AAMode &= ~1` — demotes FAA to non-fragment AA globally.

**So the shipping defaults mean:**

- Windows `0x54` → nibble **4** ⇒ **AA off**, with flags 0x10 and 0x40 set.
- Linux `0x52` → nibble **2** ⇒ 16-sample caps, but bit0 clear, so no fragment
  buffer except on destination-alpha surfaces.  (Moot: Linux never reads it.)

### 2.7 `reg3DFragSize` — SOLVED: megabytes of fragment buffer at 1024×768  [C]

See §3.1.  It is a float that multiplies a resolution-normalised scale and 1 MiB.

---

## 3. The fragment buffer — allocation and size  [C]

**Yes, AA requires a separate allocation, and the formula is recovered.**  The
fragment buffer is a dedicated **flat linear VRAM blob** (no pitch, no 2-D
alignment) owned by an object embedded in the 3D context at `ctx+0x2DCC` (ctor
`sub_9B5A0`, constructed at 0x10186A).

### 3.1 The size formula — `sub_9FBB0` @0x9FBB0  [C, constants verified]

```
9fbc2  aaFlags = sub_58410(...)                      ; effective AAMode (§2.6)
9fbd1  if (!(rt[0x360] & 0x40) && !(aaFlags & 0x10)) return 0
9fbde  if ( (surf[0x10] & 8)                          ; destination alpha
        && (aaFlags & 0x0E)==2 && !(rt[0x360] & 0x10) && !(rt[0x364] & 4) )
           accept                                     ; compressed-mask path
       else if (!(aaFlags & 1) || (aaFlags & 0x0E)!=2) return 0
9fc18  if (aaFlags & 0x40) {                          ; resolution-scaled
           A = (float)(w*h)            * 1.271565793e-06   ; = 1/(1024*768)
           B = sqrtf((float)(w*w+h*h)) * 7.8125e-04        ; = 1/1280
           scale = max(A, B);
       } else scale = 1.0f;
9fca3  fragSize = GetValue(cache+0x294)               ; float bits from the registry
9fcae  bytes = (int)( fragSize * scale * 1048576.0f )
9fcbe  this[0xF8] = bytes
9fccb  sub_9E690(rt, bytes)
```

Constants read out of `.rdata`: `0x19CA08 = 1.0f`, `0x19CA10 = 1.271565793e-06`
(= 1/786432), `0x19CA0C = 0.00078125` (= 1/1280), `0x19C9E8 = 1048576.0f`,
`0x19C400 = 2^32` (the unsigned-`fild` fixup) — all verified [C].

> **fragment buffer bytes = `FragSize` × max( w·h / (1024·768) , √(w²+h²) / 1280 ) × 1 MiB**

Both terms equal exactly 1.0 at 1024×768, i.e. the formula is normalised to that
mode, and `FragSize` is therefore **"megabytes at 1024×768"**.  With the shipping
`FragSize = 4.0`: 1024×768 → 4 MiB, 1280×1024 → 6.67 MiB, 1600×1200 → 9.77 MiB.
`this[0xF8] = 0x100000` at 0x9BBDD is the 1 MiB fallback, matching the Linux
`FragSize = 1.0f`.

### 3.2 Clamps, retry and failure — `sub_9E690` @0x9E690  [C]

```
9e69c  if (ctx[0x28] >= 0x10) return 0
9e6c7  look the render target up in a 16 × 0x10-byte table at pDev+0xA8E0
9e6f3  clamp: (caps & 0x00070000) -> max 0x2000000  (32 MiB)
9e71b         (caps & 0x0007FC00) -> max 0x0800000  ( 8 MiB)
9e732         else                -> max 0x1000000  (16 MiB)
9e744  sub_9E570(heap, ctx[0x24], size)               ; allocate
9e760  on failure: size -= 0x200000 (2 MiB); retry while size >= 0x600000 (6 MiB)
9e7a2  map (sub_28CB0) and rep-stos the whole buffer to zero
9e7e3  publish the size into the AA control word (§4.1)
```

State byte `fragbuf[0xC4]`: 1/2 = live, **4 = allocation failed**, 5 = table hit.
So the hardware floor is **6 MiB** — below that the driver gives up on AA rather
than shrink further.

### 3.3 The allocation descriptor — `sub_9E570` @0x9E570  [C]

```
9e581  this[0x9C] = this[0xC0] = this[0x1C] = size
9e58e  this[0x98] = (old & 0xF001C001) | 0x00010001
9e59f  this[0x94] = (old & 0xD0F8C020) | 0x10000020
9e5d1  this[0x04] = 8                    ; heap / memory class
9e5d8  this[0x10] = 0x00010004
9e5f4  this[0xA0] = ctx[0x1C]            ; caps word copied into the descriptor
9e62b  sub_38760(...)                    ; the VRAM allocator
9e657  this[0x90] = this[0x78]           ; the allocated VRAM offset
```

**No pitch and no alignment computation** — a flat byte blob [C].  A second
256-byte zeroed companion surface is created by `sub_9E3F0` and cached at
`pDev+0xA9E0`, template descriptor in globals 0x1D1F30..0x1D1F44; purpose not
determined, plausibly the fragment-buffer head/descriptor page [I].

### 3.4 A per-render-target size rule — `sub_9F7D6`  [C]

```
bytes = rt[0x50] * rt[0x5C] * rt[0x60]
if (bytes > 0x003C0000)                                     ; 3.75 MiB
    ctx[0x04] += ((0x3C0000 / (bpp<<9)) / ceil(width/32)) << 4
    pDev[0xA674] = pDev[0xA670] = 0                         ; drop the AA target
```

---

## 4. Configuration registers

### 4.1 The AA control word and threshold — shadows located, index [OPEN]

Two adjacent dwords in the per-context render-state block (`ctx[0x2C]`) hold what
is unmistakably a **hardware register pair**:

| shadow | contents |
|---|---|
| **`[ctx[0x2C] + 0xA0]`** | `[1:0]` = AA mode/enable; `[6:5]` = `ForceCompressedMask`; `[31:14]` = **fragment-buffer size / 256** |
| **`[ctx[0x2C] + 0xA4]`** | **`ThresholdAA`**, raw (default 0x007FFFFF) |

Writers, all verified [C]:

```
13973a  [block+0xA4] = ThresholdAA
139740  if (caps & 0x3C0):                              ; ForceCompressedMask
139761     ecx = [block+0xA0]; eax = value << 5
13976a     eax ^= ecx; eax &= 0x60; ecx ^= eax          ; merge into bits[6:5]
139771     [block+0xA0] = ecx
9e7e3   edx = size << 6 ; edx ^= [block+0xA0] ; edx &= 0x3FFF ; edx ^= (size<<6)
9e809   [block+0xA0] = edx                              ; = (size<<6 & ~0x3FFF)|(old & 0x3FFF)
```

Dirty marking: `or [ctx[0x2C] + 0x0C], 0x6400000C` @0x9B5B2; the *SetupHardware*
pre-pass `sub_2D270` sets group bit **0x10** in `[this+0x3C]` when
`(caps & 0x7FC00) && ([block+0xA0] & 3)` (@0x2D2B3–0x2D2BF) [C].

Two independent readers of `[block+0xA0] & 3` confirm the low field is the live
AA-enable: `sub_2D270` above, and `sub_2CF90` (§8, guardband).

**[OPEN] — the emitter that turns `[block+0xA0]`/`[block+0xA4]` into a
(index, value) record was not found.**  Checked and ruled out: no fixed-
displacement read of `+0xA0`/`+0xA4` exists anywhere in the packet-building
ranges (0x90000–0xA5000 and 0xC0000–0x16FFFF), and no table containing the pair
of block offsets 0xA0/0xA4 exists in `.rdata` or `.data`.  So the emission is
either a bulk copy of a block range or a table keyed on something other than the
raw offsets.  This is the one thing an open driver would still need.

### 4.2 The AA enable/disable command — `sub_9DCC0` @0x9DCC0  [C]

This *is* recovered, and it is a **DWGCTL one-shot command**, not a mode bit:

```
9dcc8  gate: test [pDev+0xAB50], 0x0007FC00              ; silicon family
       ; --- (capable silicon only) a 3-value packet, header 0x21AFAFC0:
9dd4d    count = 3 ; indices C0, AF, AF, pad
           0x1300 (idx 0xC0, DWGCTL) = 0x00000207
           0x12BC (idx 0xAF)         = tag | 1
           0x12BC                    = tag        ; tag = [pDev[4]+0x1C] += 0x10
       ; --- the mode word:
9dcd9    eax = 0x0A | (arg1 ? 0x200 : 0) | (arg2 ? 0x800 : 0)
9dcf6    if (eax == ds:0x1D1F2C) return               ; cached, no redundant write
       ; --- a 2-value packet, header 0x2121C0C0:
9ddc2    count = 2 ; indices C0, C0
           0x1300 = eax                              ; 0x00A / 0x20A / 0x80A / 0xA0A
           0x1300 = [pDev[0x44][4]+0x4C]             ; restore the running DWGCTL
         submit via sub_2BEF0
```

`sub_9DCC0(cmdbuf, 1, 0)` → `DWGCTL = 0x20A` = **AA / fragment accumulation ON**;
`sub_9DCC0(cmdbuf, 0, 0)` → `DWGCTL = 0x00A` = **OFF**, which is the resolve
point (§5).  On non-matching silicon the packet is skipped entirely and only the
cached word `ds:0x1EBE20` is updated.

Only three register indices are touched by the whole AA/fragment path [C]:

| index | MMIO | role |
|---|---|---|
| **0xC0** | **0x1300** (DWGCTL) | the AA command word |
| **0xAF** | **0x12BC** | fence/sync tag, written as `tag\|1` then `tag` |
| **0xAD** | **0x12B4** | one command parameter (`sub_9D528`) |

Sibling emitters in the same class, same idiom [C]: 0x9D4BC (`0x1300 = 0x108`),
0x9D528 (`0x12B4 = arg`, `0x1300 = 0x008`), 0x9D5E3 (`0x1300 = 4` then `0x407`
plus a fence pair), 0x9D69F / 0x9D75E (`0x1300 = flags|7` plus a fence pair,
flags carrying 0x100/0x200/0x400).

Packet grammar, for reference: `[p+0] = count N`, `[p+4] = four packed index
bytes, low byte first, 0x21 = pad`, `[p+8…] = N values`; `MMIO = 0x1000 +
index*4` — the same convention `parhelia-2d-accel-complete.md` records for
`0x212121C9` → PLNWT 0x1324.  **This is the DMA-packet namespace**, where that
mapping is valid, and it is a different namespace from the ICD's state slots of
§6.4 (see the 2026-09-05 correction in `parhelia-gl-driver.md`): the AA command
really does write DWGCTL `0x1300`.

### 4.3 The fragment-buffer base address  [OPEN]

Not found.  The allocated VRAM offset lands in `fragbuf[0x78]` → `fragbuf[0x90]`
and only the **size** is visibly published into a register shadow (§4.1).  The
base is presumably emitted by the same unresolved path.

---

## 5. The resolve  [C for the mechanism and triggers]

The vendor calls it **`FRAG_RESOLVE`** and it is *sent* — a command in the
stream, not a scanout side effect [V].

### 5.1 What is issued

Turning AA off for a render target (`sub_9DCC0(cmdbuf, 0, 0)`, §4.2) is the write
that ends fragment accumulation and forces the merge.  A second family,
`sub_9D560` / `sub_9D6E0`, emits `count = 4`, header `0xC0AFAFC0`:
`0x1300 = flags|7`, `0x12BC = tag|1`, `0x12BC = tag`, `0x1300 = restore` — where
`flags` bits 0x100/0x200 select which of two attached surfaces gets its fence
stamped and 0x400 prepends `0x1300 = 4` then `0x1300 = 0x407`.  Low nibble 7 is
the documented 3D-primitive DWGCTL opcode, consistent with the resolve being a
hardware full-surface pass [I].

### 5.2 What triggers it  [C]

The real trigger is **render-target change**, tracked in two per-device slots:
`pDev+0xA670` = the surface currently accumulating fragments, `pDev+0xA674` =
the current render target.

```
9f74f  if (rt != pDev[0xA674] && pDev[0xA674] != 0)
9f765      sub_9DCC0(pDev[0x44], 0, 0)        ; AA OFF == resolve the old target
9f7a7  else sub_9DCC0(pDev[0x44], 1, 0)       ; AA ON for the new target
9f7b7  pDev[0xA674] = rt ; 9f7d0: pDev[0xA670] = rt
```

`sub_9DE00` does the same on the `0xA670` slot, additionally consulting
`ds:0x1D3AB0` and the setting at `cache+0x2C0`.  `sub_2C124` resolves from the
device flush/teardown path.  Full call-site list for `sub_9DCC0`: 0x2C124,
0x9DE2C, 0x9DE67, 0x9DE86, 0x9DED4, 0x9DF10, 0x9DF21, 0x9DFA2, 0x9DFCF, 0x9E055,
0x9E3AB, 0x9EBA2, 0x9EBD1, 0x9F765, 0x9F7AC, 0x9FA38, 0x9FA94, 0xA06B9;
`pDev+0xA670` is force-cleared at 0xA06A6 and 0xA3542.

### 5.3 The two "resolve" registry switches are dead in this build  [C, negative]

Neither `ResolveAlphaBlendedFrags` (accessor `cache+0x2A0`) nor
`ResolveOnStencilOpChange` (`cache+0x2A4`) is ever *read*: no
`mov edx,[ecx]; call [edx+0xC]` getter call is made on either slot anywhere in
`MTXPARD.dll`, and no code path keys a resolve on a stencil-op or blend change.
They document an earlier design (and the shape of the hardware's constraints)
but do not act in driver 2.10.00.105.  `AdjustZ` *is* read, at 0xA05AE.

### 5.4 The recovered resolve orchestration  [C]

`sub_9E000` is the operation the driver invokes to retire an existing
fragment-buffer target.  `sub_9E860` calls it at 0x9E8AC when the target being
prepared differs from `pDev+0xA9E4`; the surrounding code then updates that slot
to the new target at 0x9E8E1.  This closes the call-graph ambiguity in §5.1:
`sub_9D560` and `sub_9D6E0` are fence/stamp helpers used *by* retirement, not
standalone evidence of the fragment merge instruction.

Before calling the still-opaque lower helpers (`sub_2E560` or `sub_2E360`),
`sub_9E000` restores the ordinary colour/depth attachment state with two direct
DMA packets:

```
count=4, indices 88 89 8c 8d:
  0x1220 = fragctx+0x10c    0x1224 = fragctx+0x11c
  0x1230 = fragctx+0x114    0x1234 = fragctx+0x124
count=2, indices 8a 8b:
  0x1228 = fragctx+0x110    0x122c = fragctx+0x120
```

It then emits the ordinary `DWGCTL=9` retirement fence, stamps the two attached
surfaces through `sub_9D6E0` with flags `0x100` and `0x200`, and calls
`sub_2CE60` with `0x2400000f`.  On an LX/P690-class capability word
(`caps & 0x7fc00`) it additionally sets bit 28, yielding `0x3400000f`.
The value is a helper argument, not yet an MMIO register value.  In particular,
these packets restore render targets; they do **not** expose the fragment-buffer
base register.  The merge remains somewhere inside `sub_2E560`/`sub_2E360` or
`sub_2CE60` and cannot yet be replayed independently.

---

## 6. `GL_ARB_multisample` — how the ICDs map it  [C, cross-verified]

The Linux `libGL.so.1.3.0` (1.4.7.0, 2009) and the Windows `MTXOGL.dll`
(2.10.00.105, 2007) are the same codebase; their multisample state object is
byte-identical in layout, which makes the following cross-confirmed.

### 6.1 The multisample state object — 24 bytes, identical on both platforms

Constructors: Linux **@0x1C7380**, Windows **@0x10345FB0** [C, both read]:

| offset | GL state | initial value |
|---|---|---|
| +0x00 | `GL_MULTISAMPLE` enable | **1** (the GL spec default) |
| +0x04 | `GL_SAMPLE_BUFFERS` | **0** |
| +0x08 | `GL_SAMPLES` | **0x10 = 16** |
| +0x0C | `GL_SAMPLE_ALPHA_TO_COVERAGE` | 0 |
| +0x0D | `GL_SAMPLE_ALPHA_TO_ONE` | 0 |
| +0x0E | `GL_SAMPLE_COVERAGE` | 0 |
| +0x10 | coverage value (f32) | **1.0f** |
| +0x14 | coverage invert | 0 |
| +0x18 | attrib-stack depth | 0 |
| +0x20 / win +0x1C | attrib save stack | `new char[0x180]` = 16 × 24 B |
| +0x28 / win +0x20 | "multisample unavailable" flag | Linux: virtual call on the caps singleton `0x65CF18`, vtable +0x10, arg 0.  Windows: global byte `ds:0x104EDBB8`. |

In the live context the block sits at **`ctx+0x7C70`** (Linux) / **`ctx+0x74AC`**
(Windows).

**`GL_SAMPLES` is hard-coded to 16 — the FAA-16x sample count** — while
`GL_SAMPLE_BUFFERS = 0`, so per the GL spec multisampling is inactive regardless.
That pairing is the clearest surviving trace of the intended feature.

### 6.2 Visuals / pixel formats — no multisample config, ever

Linux [C, negative]:

- client GLX extension string is `GLX_ARB_get_proc_address GLX_EXT_visual_rating
  GLX_EXT_visual_info GLX_SGI_swap_control` — **no `GLX_ARB_multisample`**;
  `libglx.so` offers only `GLX_EXT_visual_info/_rating/_import_context`;
- `glXGetFBConfigAttrib` @0x35D0A0 compares 23 attribute codes, none of them
  `GLX_SAMPLE_BUFFERS` (100000 = 0x186A0) or `GLX_SAMPLES` (100001) — both return
  `GLX_BAD_ATTRIBUTE`.  `glXChooseVisual` @0x35EB80 likewise;
- the only `0x186A0` immediates in `libGL` (@0x13B7F6, @0x13BBBC) are
  `GL_MAX_ELEMENTS_VERTICES/INDICES = 100000`, and `mtx_drv.so`'s twelve are all
  `imul reg,reg,100000` unit conversions.  Unrelated.

Windows [C] — `MTXOGL.dll` *does* handle `WGL_SAMPLE_BUFFERS_ARB` (0x2041) and
`WGL_SAMPLES_ARB` (0x2042) in its `WGL_ARB_pixel_format` builder, and this is
where the dead FAA branch shows:

```
10043eaa:  mov  DWORD PTR [eax+esi*1+0x38],ebx   ; pf[fmt].aa_samples = 0   <-- forced
   ...     ; WGL_SAMPLE_BUFFERS_ARB value object built with 0
10043eff:  mov  ecx,DWORD PTR [ecx+esi*1+0x38]
10043f03:  neg  ecx
10043f05:  sbb  ecx,ecx                          ; ecx = aa_samples ? -1 : 0
10043f07:  and  ecx,0x10                         ; -> WGL_SAMPLES_ARB = 16 or 0
```

The field `pf[fmt]+0x38` is written in exactly one place (to zero) and read in
exactly one place in the whole 5.2 MB DLL [C, exhaustive].  Every pixel format
reports 0/0, while the code that would report **16** is still there, unreachable.

### 6.3 `glSampleCoverage` / `glSampleCoverageARB`

Linux: exported thunks @0x1AC0F0 / 0x1B09B0 dispatch through context-table slot
**+0x9B0**; the real implementation @0x128AB0 clamps to [0,1] (helper 0x12CE30
against `.rodata` `0x367A0C = 1.0f`, `0x36992C = 0.0f`), stores to `ctx+0x7C80` /
`ctx+0x7C84`, sets dirty bit `0x10000`.  Other dispatch tables bind the same slot
to a stub raising `GL_INVALID_OPERATION` (0x502) or to `repz ret`.

`glGet` [C]: `GL_SAMPLE_BUFFERS` (0x80A8) → `ctx+0x7C74`, `GL_SAMPLES` (0x80A9) →
`ctx+0x7C78`, `GL_SAMPLE_COVERAGE_VALUE` (0x80AA) → `ctx+0x7C80`,
`GL_SAMPLE_COVERAGE_INVERT` (0x80AB) → `ctx+0x7C84`.  The four *enable* tokens
0x809D/0x809E/0x809F/0x80A0 are **absent from the `glGet` switch** and fall to the
default arm @0x13ADB6 which raises `GL_INVALID_ENUM` (0x500) — a spec bug; they
are handled by `glIsEnabled` (@0x1271C0).

`glEnable(GL_MULTISAMPLE)` is **gated** @0x125148: it sets the enable byte only
when the "multisample unavailable" flag at `ctx+0x7C98` is zero.

### 6.4 Linux is state-only; Windows also emits a command record

On Linux nothing but `glGet`, `glIsEnabled` and push/pop-attrib reads the
multisample fields, and dirty bit `0x10000` is tested only by attribute
save/restore and context copy — **no hardware emitter** [C, exhaustive over
`ctx+0x7C70/74/78/7C/7D/7E/80/84`].

On Windows the lazy state flush *does* read them and emits three records
[C, `MTXOGL.dll` @0x103A116A–0x103A1273]:

| site | record | payload |
|---|---|---|
| 0x103A11C0 | opcode **0x05**, slot **0x1D** | mode bitfield (below) |
| 0x103A120A | opcode **0x05**, slot **0x1F** | coverage value, raw f32 bits |
| 0x103A124D | opcode **0x05**, slot **0x1E** | coverage invert, 0/1 |

```
bit 0 = GL_MULTISAMPLE                (ctx+0x74ac)
bit 1 = GL_SAMPLE_ALPHA_TO_COVERAGE   (ctx+0x74b8)
bit 2 = GL_SAMPLE_COVERAGE            (ctx+0x74ba)
bit 3 = GL_SAMPLE_ALPHA_TO_ONE        (ctx+0x74b9)
bits[31:4] = [esp+0x14] & ~0xF
```

The upper term is a stack slot last written @0x103A0DB9 from `ctx+0xCD8` — the
**viewport Y** of the `key 0x26` viewport record emitted earlier in the same
function.  Either the register genuinely carries something else in bits[31:4] and
the ICD reuses the cached copy, or this is a stale-slot bug in a path that never
runs in shipping configurations (it does not, since `SAMPLE_BUFFERS = 0`).
**[OPEN]** — flagged, not resolved; do not build on it.

**These are GL state slots, not register offsets.**  The emitter is
`AllocRecord(this, opcode, payload_bytes, count)` — Linux @0x1C9C50
(`esi = opcode, edx = payload bytes, ecx = count`), Windows @0x10003860 (stdcall,
same order) — and it writes the two-dword `(opcode, count)` header itself,
followed by `count` payload pairs.  That is independent binary confirmation of
the capture-side grammar and of the correction now carried at the top of
`parhelia-gl-feature-map.md` / `parhelia-gl-driver.md` (2026-09-05): region-2
indices are **GL state-slot ids routed through a per-slot handler table in the
kernel**, and `reg = 0x1000 + index*4` does **not** apply to them.  So slots
0x1D/0x1E/0x1F have no known MMIO address, and neither does the point-size pair
of §7.  Opcode 0x0C is the general state-slot stream; opcode 0x05 is a second one
(it also carries the blend factors, ids 0x09–0x0C).

The `0x1000 + index*4` mapping *does* hold for the `0x2121xxxx`-tagged DMA
packets, which is the namespace the FAA control command of §4.2 uses — those
register identities (0x1300 / 0x12BC / 0x12B4) are unaffected by the correction.

Cross-reference: `parhelia-rendertarget.md` §4 covers the Linux GL side of this
from the capture direction and reaches the same verdict.

---

## 7. Edge, line, point and polygon smoothing  [C]

Independent of FAA; fully recovered from Linux `libGL`.

| cap | state byte | implementation |
|---|---|---|
| `GL_POLYGON_SMOOTH` (0x0B41) | `ctx+0x9F4` | **silently ignored.**  Complete reference set: glEnable @0x125710, glDisable @0x125E14, glIsEnabled @0x1275B6 and the four `glGet` arms.  No renderer reads it.  (Contrast the adjacent `GL_POLYGON_STIPPLE` byte at `ctx+0x9F5`, consumed at 0xEAA2D / 0xF323D.) |
| `GL_LINE_SMOOTH` (0x0B20) | `ctx+0xAAC` | **emulated in geometry.**  @0xE7245 the line routine branches to 0xE74E1, normalises the direction (`pow(dx²+dy², 0.5)`), offsets both endpoints by ±0.5·width and emits a float-positioned widened quad.  With smoothing off it replicates `(int)(width+0.5)` one-pixel lines.  No AA bit anywhere. |
| `GL_POINT_SMOOTH` (0x0B10) | `ctx+0xAD0` | **the only one that reaches the chip.**  @0x114343 it picks the point-size clamp range: smooth → [0.25, 10.0] (`GL_SMOOTH_POINT_SIZE_RANGE`), aliased → [1.0, 2048.0] (`GL_ALIASED_POINT_SIZE_RANGE`), then emits the clamped **min** as opcode-0x0C state slot **0x9B** and the clamped **max** as slot **0xA6**. |

Those are state slots, not registers (§6.4) — the MMIO addresses they end up in
are unknown.  A second consumer @0xEBF31 routes smooth points into the 0xE6FD0
fallback rasterizer.  Note in passing that the value `0x45000000 = 2048.0f`
recorded in `parhelia-3d-state-complete.md`'s draw capture is the
`GL_ALIASED_POINT_SIZE_RANGE` upper clamp emitted here, not a viewport scale —
useful for whoever re-decodes that capture under the corrected grammar.

So Parhelia has classic **antialiased points** (a real state change with a real
clamp range), software-emulated smooth lines, and no polygon smoothing.  None of
this touches the fragment buffer.

---

## 8. Interactions and restrictions  [C unless noted]

| interacts with | evidence | effect |
|---|---|---|
| **render-to-texture / special surfaces** | 0x1018B5: `test [rt+0x360],0x10` / `test [rt+0x364],0x04`, either set ⇒ skip; also `if (rt[0x1A0]==0) skip` @0x101899 | **no fragment buffer for that render target** |
| **destination alpha** | `test BYTE [surf+0x10],8` @0x9FBE4 / 0x9B60B / 0x9B6FD / 0x9B711 | switches to the *compressed-mask* branch (allowed even with `AAMode` bit0 clear); `ForceCompressedMask` is what lands in the AA control word bits[6:5], gated by `caps & 0x3C0` |
| **guardband clipping** | `sub_2CF90` @0x2CF90 | when `[block+0xA0] & 3` (AA live), the guardband shadow `[block+0x780]` is reduced by one: **32.0f → 31.0f**, **16.0f → 15.0f**, and dirty bit 0x10000000 is set.  This explains the 31.0/15.0 values recorded for register **0x12F4** in `parhelia-3d-state-complete.md` §3 — they are the *AA-active* guardband, not a texture-stage distinction |
| **AA level** | everything requires `(AAMode & 0x0E) == 2` | only the 16x mode ever allocates a fragment buffer; the 4x path never does |
| **master enable** | `AAMode & 0x10` @0x9FBD6, overridable by `rt[0x360] & 0x40` | global on/off |
| **global kill switch** | `ds:0x1D2048 == 0 ⇒ AAMode &= ~1` @0x139629, 0x13978F | demotes FAA to non-fragment AA driver-wide |
| **silicon** | `test [pDev+0xAB50],0x0007FC00` @0x9DCC8, 0x2CF92, 0x2D299, 0x16FE01 | on non-matching parts the AA command packet is never emitted.  All P690 class bits are `0x10000`, `0x20000` or `0x40000`, so each passes this Windows-driver gate **[C]**.  This proves vendor-path eligibility on P690, not working FAA hardware on the test card. |
| **memory** | §3.2 | 8 / 16 / 32 MiB cap by silicon, **6 MiB floor** with 2 MiB back-off retry, refusal if `ctx[0x28] >= 0x10`, plus the 3.75 MiB per-RT rule of §3.4 |
| **plane write mask** | `ForcePLNWMASK4FAA` [V] | a PLNWT (`0x1324`) workaround must be suppressed in FAA mode [I] |
| **stencil / blending** | `ResolveOnStencilOpChange`, `ResolveAlphaBlendedFrags` [V] | documented as resolve triggers, but **not implemented** in 2.10.00.105 (§5.3).  The comments still tell you the hardware cannot carry fragments across a stencil-op change, and that a blended scene cumulates no fragments — i.e. **blending defeats FAA** |
| **surface format** | `sub_86F90` @0x86FE4 | multisample cap bit 3 is cleared for format code `0x23` |

The performance/memory cliff the vendor guards is fragment-buffer exhaustion:
the size is a fixed budget (§3.1), it is clamped and can fail (§3.2), and a
failure sets `fragbuf[0xC4] = 4` rather than degrading gracefully in software.

---

## 9a. 2026-09-14: the FAA command stream in `MTXPARD.dll`

Re-read from `scratchpad/pard.asm` with the DMA packet grammar of §4.2
(`MMIO = 0x1000 + index*4`, index byte `0x20` = pad `0x1080`, `0x21` = unused).
This section supersedes the [OPEN] statements of §4.1, §4.3 and §5.4 where it
disagrees with them.

### 9a.0 Correction: most DWGCTL 0x0a/0x08 traffic is the Z unit, not FAA  [C]

`parhelia-p690-zcompression-clear-banks-re.md` §2 decodes the `mtxpserd.dll`
twins of these routines, and the `MTXPARD.dll` code matches them instruction
for instruction:

| `MTXPARD.dll` | `mtxpserd.dll` | what it is |
|---|---|---|
| `sub_9DCC0` | `0x9f270` | Z tile tracking mode: `0x00a` off, `0x20a` on, `0xa0a` decompress |
| `sub_9DBB0` | `0x9f160` | Z tile state reset: `0x108` (compressed) / `0x308` |
| `sub_9F470` -> `sub_9DA40` / `sub_9D480` | `0x9eff0` | Z-init load: `0x12b4 = packed clear value`, `DWGCTL 0x008`/`0x208`, or reset `0x108`/`0x308` |
| `sub_9FCE0` -> `sub_9F4B0` | `0xa0a60` | D3D Clear for `caps & 0x7fc00` |
| `sub_9DE00`, `0x9F74F` | `0x9f3b0` and the Z-init block | ownership of the Z compression (`pDev+0xA670`) and Z-init (`pDev+0xA674`) surfaces |

`sub_9AC70`, which produces the value `sub_9F4B0` hands to `0x12b4`, is the
depth clear packer (D16 `z16 | z16<<16`, Z24 with stencil).  Therefore:

* §4.2 "AA enable/disable command" and the call-site list of §5.2 describe the
  Z unit's tracking mode, as `parhelia-p690-fast-depth-clear.md` measured on
  P690 silicon.  `0x20a` is not "enter fragment accumulation".
* `pDev+0xA670`/`+0xA674` are the Z compression / Z-init owner surfaces, not
  "the surface accumulating fragments" and "the current render target".
* `0x12b4` is the Z-init value register.  The FAA code below issues the tracking
  commands because it has to leave the Z unit in a defined state around its
  own passes; what is FAA-specific is everything keyed on the fragment context
  (`fragctx+0xC4` state 1/2 from `sub_9E690`, `fragctx+0xE8`).

### 9a.1 The state block is a register mirror  [C for the slots, I for 0x12b4/0x12b8]

`sub_2EC10` (SetupHardware) emits the surface slots straight from the state
block `[ctx+0x2C]` [C]:

```
count 0xf/0xd, indices 88 89 8c 8d:  0x1220=[blk+0xaa4] 0x1224=[blk+0xab4]
                                     0x1230=[blk+0xaac] 0x1234=[blk+0xabc]
count 0xa,     indices 8a 8b c5 ..:  0x1228=[blk+0xaa8] 0x122c=[blk+0xab8]
```

so the slot addresses mirror at `+0xaa4/+0xaa8/+0xaac` and their configuration
words at `+0xab4/+0xab8/+0xabc`.

The depth/stencil group mirrors linearly at `0x1214 + offset` [C]: the save
code at 0x9D210 copies `[blk+0xb4]`, `+0xb8`, `+0xbc` into `fragctx+0x100..0x108`,
and `sub_2E360`/`sub_2E560` write exactly those three values back to `0x12c8`,
`0x12cc`, `0x12d0`.  Extending that mirror places the AA control word
`[blk+0xA0]` at `0x12b4` (index `0xad`) and `ThresholdAA` `[blk+0xA4]` at
`0x12b8` (index `0xae`).  That extrapolation is **not** supported: `0x12b4` is
the Z-init value register (9a.0), and the only other `0x12b8` write is the
engine initialiser at 0x2C49E (`1`, or `0x70601` on capability bits
`0x38`/`0x7ffc0`) [C].  No packet header anywhere in the binary names index
`0xad` or `0xae` except those two sites, so the AA control word and threshold
reach the chip through an emitter that is still not identified [OPEN].

### 9a.2 The Z-init load — `sub_9F470`  [C, not FAA]

Kept for the record because an earlier reading took it for an FAA target bind.
`sub_9F470(cmdbuf, value, clear_depth, zsurf, reset)` marks `zsurf+0x28 ->
+0x368 |= 8` and dispatches by silicon: `caps & 0x7fc00` (LX/P690 class) jumps to
`sub_9DA40`, otherwise to `sub_9D480`.  Both emit:

| case | packets |
|---|---|
| `reset != 0` | `DWGCTL 0x1300 = 0x108 [| 0x200]`, then restore DWGCTL |
| `reset == 0` | `[zsurf+0x78] = clear_depth`; `0x12b4 = value`, `DWGCTL = 0x008 [| 0x200]`, restore DWGCTL |

`sub_9DA40` first emits `DWGCTL = 0x207` with a fence pair (`0x12bc = tag|1`,
`tag`) while Z compression is active for the clear (`ds:0x1EBE18 != 0`), and
adds `0x200` ("not compressed") otherwise; the last word is cached in
`ds:0x1D1F28`.  `value` is the packed clear word from `sub_9AC70`.  Callers: the
clear at 0x9F86C and 0x9FB31 (inside `sub_9F4B0`, reached from the D3D Clear
`sub_9FCE0`), and the teardown paths 0xA3363 / 0xA3515 with `reset = 1`.

### 9a.3 The fragment buffer as a surface  [C]

`sub_9E3F0` builds the fragment buffer's surface descriptor in the globals
`ds:0x1D1F30..0x1D1F40` and copies it (five dwords) into the allocation object
at `+0x30`:

```
+0 (0x1D1F30) address  = [fragbuf+0x78] after allocation (VRAM offset), low 2 bits clear
+4 (0x1D1F34) config   = (old & 0xc020 | 0x20) & 0xf6fbffff | 0x36030000   ; code 0x36, type 3
+8 (0x1D1F38) layout   = (old & 0xc002 | 2) & 0xf001ffff | 0x10000
+0x10 (0x1D1F40)       = [rt+0x1c] (silicon capability word)
```

After allocation the first 0x40 dwords of the buffer are cleared (0x9E52F).

### 9a.4 The resolve pass — `sub_9E000`  [C]

Run when a fragment target is retired (§5.4).  In order:

1. Z tracking off (`sub_9DCC0(.., 0, 0)`, `DWGCTL = 0x00A`) if it was on.
2. State block overrides, then SetupHardware (`sub_2EC10`) emits them:
   `[blk+0xA0] = fragctx+0xFC | 3` (AA control, enabled);
   `[blk+0xB4] (0x12c8) = (old & ~0x000e0000) | 0x0010000e`;
   slot 0 config `0x1224 = (old & 0xc020) | 0x20`;
   **slot 1 `0x1228 = fragment-buffer address`, `0x122c = its 0x36/type-3
   config`** (the globals of 9a.3); slot 2 `0x1230/0x1234` = the saved
   `fragctx+0x114/+0x124`; `[blk+0x8c] = 5`, `[blk+0x8e] = 3`; dirty
   `0x24000000`.
3. Stamp through `sub_9D560(.., 0xc00)`.
4. WARP constants, one 14-dword packet:
   `0x1160, 0x1174, 0x1178, 0x1180 = 1.0, 0.5, 0.25, 0.25`;
   `0x1164, 0x1174, 0x1178, 0x1180 = 1.0, 0.5, 0.25, 3.5`;
   `0x1168, 0x1174, 0x117c, 0x1180 = 1.0, 0.5, 1.25, 0.25`.
   With the vertex layout the Z-uncompress pass uses (`0x1174` = Z, `0x1178` /
   `0x117c` = y, `0x1180` = x; zcompression doc §3.4) this is one small
   triangle at depth 0.5 spanning x 0.25..3.5, y 0.25..1.25 [I].
5. `DWGCTL 0x1300 = 0x207`, fence `0x12bc = tag|1`, `tag`, restore DWGCTL.
6. Lock the fragment buffer, read dwords 0..3 and 32..35 (`& 0x3ffff`) into
   `fragctx+0xC8..0xE4`, and zero them — per-pass counters kept in the buffer
   head.
7. Z tracking back on if it was on; `[blk+0xB4] = (old & ~0xc) | 2`.

So retirement draws one small triangle with the fragment buffer bound as the
depth slot and the AA control word enabled, fences it with DWGCTL `0x207`, and
collects eight counters from the buffer head [C for the sequence].  Whether
that primitive is the merge trigger or a flush sentinel is [OPEN].

### 9a.5 Restoring the target — `sub_9E860`, `sub_2E360`, `sub_2E560`  [C]

After a retirement `sub_9E860` re-emits the saved slot descriptors (`0x1220,
0x1224, 0x1230, 0x1234`, then `0x1228, 0x122c`) and calls the family restore
(`caps & 0x7fc00` -> `sub_2E560`, else `sub_2E360`) with the saved
`fragctx+0xFC, +0x100, +0x104, +0x108`:

| register | original board (`sub_2E360`) | LX/P690 class (`sub_2E560`) |
|---|---|---|
| PLNWT `0x1324` | `0xffffffff` | same |
| `0x12cc`, `0x12d0` | saved `+0x104`, `+0x108` | same |
| DWGCTL `0x1300` | `0x0c4` | `0x2000c4` |
| scissor `0x132c` / `0x1328` | `0x0fff0fff` / 0 | same |
| `0x12c8` | saved `+0x100` | same |
| `0x12fc` | saved `+0xFC` | same |
| guardband `0x12f4` | not written | `31.0f` |
| WARP `0x116c..0x1198` | 34-dword packet, zeros except `0x1168 = 0x1188 = 1/16` | same |

then marks the block dirty (`0x6400000f` / `0x7400000f`) and emits `DWGCTL = 9`
with the two surface stamps (`sub_9D6E0`, flags `0x100`/`0x200`).  The saved
values come from `sub_9D1B0`, which snapshots `[blk+0xB4..0xBC]` and all slot
descriptors `[blk+0xAA4..0xAD0]` into `fragctx+0x100..0x138` when FAA starts on
a target (`fragctx+0xA4 == 3`), and from 0xA062C, which stores the render
state word as `fragctx+0xFC`.

### 9a.6 Still open

* The bulk emitter of the `0x1214`-mirrored range, which would confirm
  `0x12b4`/`0x12b8` for the AA control word and threshold (9a.1).
* How the fragment buffer is bound while fragments accumulate: only the
  retirement pass (9a.4) binds it, through depth slot `0x1228`.
* The meaning of the eight counter words of 9a.4.
* A capture with `AAMode = 0x53` remains the only way to validate any of it on
  hardware; none of this is exercised by the open driver.

---

## 9. What an open driver should do

1. **Do not implement FAA.**  Report `GL_SAMPLE_BUFFERS = 0`; do not advertise
   `GLX_ARB_multisample`; do not expose multisample FBConfigs.  This matches the
   vendor stack exactly on both platforms, so nothing regresses.
2. If `GL_ARB_multisample` is advertised for API completeness, track
   `glSampleCoverage` state and return it from `glGet`.  Do **not** copy the
   vendor's bug of raising `GL_INVALID_ENUM` for `glGetIntegerv(GL_MULTISAMPLE)`.
3. `GL_POINT_SMOOTH` is worth wiring up — real hardware, two register writes
   (§7).  `GL_LINE_SMOOTH` is geometry widening in the vertex path.
   `GL_POLYGON_SMOOTH` can be ignored, as the vendor does.
4. The multisample state slots 0x1D–0x1F (§6.4) are ICD state-slot ids with no
   known MMIO address; there is nothing to program.
5. If FAA is ever attempted: budget the allocation with the §3.1 formula, honour
   the 6 MiB floor and the silicon caps of §3.2, and remember the guardband must
   drop by 1.0 while AA is live (§8).

---

## 10. What would settle the rest

§9a (2026-09-14) answers item 4 and part of item 2 and corrects the command
readings of §4.2/§5.2; what remains is listed in §9a.6.

1. **The AA control/threshold register index.**  The values are staged at
   `[ctx[0x2C] + 0xA0]` and `+0xA4` (§4.1) and no fixed-displacement reader
   exists.  Next step: find the state-block → command-stream emitter reached from
   `sub_2EB80` / `sub_2C8D0` / `sub_2DC20` that copies block ranges in bulk or
   walks a (block offset, register index) table indirectly.
2. **The fragment-buffer base-address register.**  Track `fragbuf[0x90]` (set at
   0x9E657) to its consumer; it is very likely emitted next to the size.
3. **`ThresholdAA`'s units** — settled the moment (1) is: if it lands in a
   register beside an S8Z24 comparator it is a depth tolerance; if it is masked
   against a float it is a mantissa mask.
4. **`FRAG_RESOLVE`'s exact opcode** — the operation is reached through
   `sub_9E000` (§5.4), but the actual merge remains inside `sub_2E560`,
   `sub_2E360` or `sub_2CE60`; distinguishing those needs a trace with AA
   forced on.
5. **Live confirmation:** on Windows, set `HKR\3D.AAMode` to `0x53` (nibble 3 =
   16x + fragment buffer, flags 0x50 retained) and capture the command stream;
   every open item above falls out of one such capture.  On Linux this is
   impossible — the parameters are not read (§2.3).
6. **What the kernel's per-slot handler table does with slots 0x1D–0x1F** — the
   172-entry table named in the 2026-09-05 correction to
   `parhelia-gl-driver.md`.  That is the only route from the ICD's multisample
   state to whatever hardware bits (if any) it reaches.

---

## 11. Address index

`kernel/mtx_parhelia.o` (Linux)
: `mtx722` @0x74950 switch table; AA ctor arms @0x76270 / 0x762D4 / 0x76300 /
  0x76329 / 0x76352 / 0x7637B / 0x763A4 / 0x763CA; `RegistrySwitch<u32>` ctor
  `mtx2860` @0xBC470, `<bool>` `mtx2302` @0xBC540; duplicate table `mtx1049`
  @0x765F0.

`xserver/7_4_0/mtx_drv.so`
: `mtx724` @0x60D70; name strings @0x110BAB…0x110C0E; section `"3D"` @0x11078C.

`opengl6/lib64/libGL.so.1.3.0`
: multisample ctor @0x1C7380; `glSampleCoverage` @0x128AB0 (thunk 0x1AC0F0, ARB
  alias 0x1B09B0, dispatch slot +0x9B0); `glEnable` @0x124FD0 (`GL_MULTISAMPLE`
  arm 0x12513C); `glDisable` @0x125780; `glIsEnabled` @0x1271C0; `glGetIntegerv`
  @0x12B850 (default/error arm 0x13ADB6); `glXGetFBConfigAttrib` @0x35D0A0;
  `glXChooseVisual` @0x35EB80; record allocator @0x1C9C50, commit @0x1C9AD0;
  line-smooth quad expansion @0xE74E1; point-size emitter @0x114343.
  Multisample block at `ctx+0x7C70`.

`MtxPar/MTXOGL.dll` (base 0x10000000)
: multisample ctor @0x10345FB0; state flush / record emitter
  @0x103A116A–0x103A1273; `glSampleCoverage` @0x103B5890; `wglGetProcAddress`
  registration @0x1002181C / 0x10021857; WGL pixel-format builder @0x10043D10
  (multisample arms 0x10043EA5–0x10043F3F); record allocator @0x10003860 →
  0x10003640.  Multisample block at `ctx+0x74AC`.

`MtxPar/MTXPARD.dll` (base 0x00010000)
: registry strings @0x108C40 `AAMode`, 0x108C48 `"3D"`, 0x108C4C `ThresholdAA`,
  0x108C5C `FragSize`, 0x108C6C `ForceCompressedMask`, 0x108C84
  `ResolveOnStencilOpChange`, 0x108CA4 `AdjustZ`, 0x108CB0
  `ResolveAlphaBlendedFrags`; settings ctor `sub_109704` (AAMode arm 0x10973D,
  FragSize arm 0x109823); accessor wrapper `sub_8F5C0`; effective-mode
  `sub_58410` (+ jump table 0x58478); caps patch `sub_86F90` (call site
  0x87C5B); telemetry `sub_58170` @0x58203; state defaults `sub_1396A0`
  (ThresholdAA 0x13973A, ForceCompressedMask 0x139761, AAMode 0x13977F);
  fragment-buffer class ctor `sub_9B5A0` (@0x10186A), sizing `sub_9FBB0`,
  clamp/retry `sub_9E690`, allocation `sub_9E570` (allocator `sub_38760`),
  companion surface `sub_9E3F0`, per-RT rule `sub_9F7D6`; AA command
  `sub_9DCC0`; resolve-on-RT-change `sub_9DE00` / 0x9F74F; guardband adjust
  `sub_2CF90`; SetupHardware `sub_2EC10`, pre-pass `sub_2D270`, group emitter
  `sub_2EB80`; absolute-address register helper `sub_170E10`.
