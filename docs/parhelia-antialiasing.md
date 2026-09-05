# Parhelia Fragment Anti-Aliasing (FAA-16x) — reverse engineering notes

Tags: **[CONFIRMED]** = read out of vendor source or disassembly, address given;
**[INFERRED]** = best reading of the evidence, self-consistent but not proven;
**[VENDOR]** = verbatim from a Matrox source comment; **[OPEN]** = not recovered.

Provenance: `matroxdriver-x86_64-cvs-20090511/` (Linux: kernel sources +
`kernel/mtx_parhelia.o`, `xserver/7_4_0/mtx_drv.so`, `opengl6/lib64/libGL.so.1.3.0`)
and `xddm32_210_00_105_se_u_nowhql/MtxPar/` (Windows XPDM: `MTXPARD.dll`,
`MTXOGL.dll`, `MTXPARM.sys`).  Companion docs: `parhelia-gl-feature-map.md`,
`parhelia-gl-driver.md`, `parhelia-3d-state-complete.md`, `parhelia-fragment-isa.md`,
`parhelia-variants.md`.

---

## 0. The architecture answer, up front

**Parhelia's AA is a fragment/coverage buffer, not supersampling and not classic
"N full samples per pixel" multisampling.**  Matrox's own term for it, in the
driver source, is *"sort-independent antialiasing with the fragment buffer"*
[VENDOR, `kernel/src/MtxParhlParms.c:69,73`].  Decomposed:

- **Fragment** here is the D3D/OpenGL-era *A-buffer* sense: an edge pixel that is
  only partially covered by a primitive produces a **fragment record** — colour,
  depth, and a **coverage mask** over a 4×4 (=16) subsample grid — which is
  written to a **side buffer**, not to the framebuffer.  Interior (fully covered)
  pixels take the ordinary single-sample path straight into the framebuffer.
  This is why the feature is "16x" while costing far less than 16× fill: only
  edge pixels pay.
- **Sort-independent** is the D3D vocabulary of the period
  (`D3DANTIALIAS_SORTINDEPENDENT`): fragments accumulate in the buffer, so the
  application does *not* have to submit primitives in depth order.  That is the
  functional difference from the older "antialiased edges" schemes which required
  back-to-front submission.
- Because fragments accumulate, the hardware needs a rule for *when two
  fragments at the same pixel belong to the same surface and may be merged*.
  That rule is what `reg3DThresholdAA` parameterises [INFERRED — see §2].
- A **resolve** pass ("FRAG_RESOLVE" in the vendor's words [VENDOR]) folds the
  fragment buffer back into the framebuffer.  It is not free-running: the driver
  issues it explicitly, and the vendor exposes switches for *when* to issue it
  (§5).

**How far this RE got.**  The whole *configuration surface* is recovered exactly
(§2), the *GL/ICD mapping* is recovered exactly and cross-confirmed on two
independent builds (§6), and the *edge/line/point* behaviour is recovered exactly
(§7).  The *hardware register encoding of the AA mode, the threshold and the
fragment-buffer base/size* is **[OPEN]** — see §3/§4 and §10 for precisely what
would settle it.  An open driver can ship without FAA; nothing else in the 3D
pipeline depends on it.

**Bottom line for a from-scratch driver:** do not implement FAA.  Advertise
`GL_SAMPLE_BUFFERS = 0`, exactly as both vendor ICDs do (§6).  The vendor's own
Linux stack never enables FAA at all — the knobs are registered and then never
read by any Linux component (§2.3).

---

## 1. Where the feature lives in the vendor stack

| component | role w.r.t. AA |
|---|---|
| `kernel/src/MtxParhlParms.c` | declares the module parameters (Linux) — **source, readable** |
| `kernel/mtx_parhelia.o` | registers the same knobs as registry switches; **never reads them** [CONFIRMED, §2.3] |
| `xserver/7_4_0/mtx_drv.so` | same registry-switch table (`mtx724` @0x60d70); no FAA consumer found |
| `opengl6/lib64/libGL.so.1.3.0` | multisample **state tracking only**, no hardware path [CONFIRMED, §6] |
| `MtxPar/MTXOGL.dll` (Windows GL ICD) | multisample state tracking **plus** a command-stream emitter [CONFIRMED, §6.4] |
| `MtxPar/MTXPARD.dll` (Windows D3D/DDI display driver) | the only binary that consumes `AAMode`/`ThresholdAA`/`FragSize` |
| `MtxPar/MTXPARM.sys` (Windows miniport) | contains **no** AA string or knob [CONFIRMED, negative] |

The knobs are Matrox *registry switches*, section `"3D"`, i.e. on Windows they
are `HKR\3D.AAMode` etc. under the display-adapter service key — the same
`<Section>.<Name>` form used by `3D.D3DEnable` in `mtxpar.inf:177` [CONFIRMED].
On Linux the same values are module parameters named `reg3D<Name>`.

---

## 2. The configuration surface — CONFIRMED, complete

### 2.1 The knobs, with the vendor's own comments

From `matroxdriver-x86_64-cvs-20090511/kernel/src/MtxParhlParms.c` [VENDOR]:

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

Two comments are copy-paste duplicates (`FragSize` inherits `ThresholdAA`'s
comment); the useful ones are the four below `FlipOnVBlank`.

### 2.2 The compiled-in defaults differ from the module-parameter defaults

The registry-switch constructor inside the binaries carries its *own* defaults,
and two of them do **not** match `MtxParhlParms.c`.  Both binaries agree with
each other, so the source file is the outlier.

`mtx_parhelia.o`, ctor `mtx722` @0x74950 (the same table exists twice, second
copy at `mtx1049` @0x765f0); `mtx_drv.so`, ctor `mtx724` @0x60d70 —
call is `RegistrySwitch<T>::RegistrySwitch(this, &ok, "3D", "<Name>", 0, default)`
[CONFIRMED]:

| switch | member | site (`mtx_parhelia.o`) | type | compiled default | `MtxParhlParms.c` |
|---|---|---|---|---|---|
| `AAMode` | cfg+0x00 | 0x762d4 `mov r9d,0x52` | u32 | **0x52** | 0x52 ✔ |
| `ThresholdAA` | cfg+0x08 | 0x76270 `mov r9d,0x7fffff` | u32 | **0x007FFFFF** | 0x007FE000 ✗ |
| `FragSize` | cfg+0x10 | 0x76300 `mov r9d,0x40800000` | f32 | **4.0f** | 1.0f (0x3F800000) ✗ |
| `ForceCompressedMask` | cfg+0x18 | 0x76329 | bool | 1 | TRUE ✔ |
| `ForcePLNWMASK4FAA` | cfg+0x20 | 0x76352 | bool | 1 | TRUE ✔ |
| `ResolveAlphaBlendedFrags` | cfg+0x28 | 0x763ca | bool | 0 | FALSE ✔ |
| `AdjustZ` | cfg+0x30 | 0x763a4 | bool | 0 | FALSE ✔ |
| `ResolveOnStencilOpChange` | cfg+0x38 | 0x7637b | bool | 1 | TRUE ✔ |

(The section string `"3D"` is `.rodata.str1.1+0x59`; the eight names are the
contiguous block `+0x406 … +0x469` of the same section.)

### 2.3 On Linux the knobs are dead

Searched exhaustively across all three Linux binaries:

- `libGL.so.1.3.0`: the strings `AAMode`, `ThresholdAA`, `FragSize`, `reg3D`
  do **not occur at all** (byte-level grep) [CONFIRMED negative].  The ICD has no
  registry machinery, and never asks the kernel for an AA capability.
- `mtx_parhelia.o`: the constants `0x7fffff` and `0x40800000` occur **only** at
  the two constructor sites above — nothing else in the 2 MB object reads them
  [CONFIRMED].  The literal `0x007FE000` never occurs.
- `mtx_drv.so`: registers the switches, exports no `FAA`/`AntiAlias`/`Resolve`/
  `Frag` symbol.

So the Linux port registers the FAA configuration and then never consumes it.
The values are inherited from the shared Matrox core that the Windows D3D driver
also builds against.  **Nothing in the Linux vendor stack ever turns FAA on.**

### 2.4 What the individual knobs tell us about the hardware

- **`ForceCompressedMask` — "Force a different AA mode when destination alpha is
  enabled".**  The coverage mask therefore has (at least) two storage encodings,
  a normal one and a *compressed* one, and the normal one **conflicts with
  destination alpha** [INFERRED, strong].  The natural reading is that the
  uncompressed 16-bit coverage mask is stashed in the render target's alpha
  channel (or in bits the alpha channel would otherwise use), so a render target
  whose destination alpha the application actually reads forces the driver into
  the compressed-mask mode.  This is the single most useful hint about the
  fragment format that this RE recovered.
- **`ForcePLNWMASK4FAA` — "Do not execute PLNWTMASK hack in FAA mode".**  PLNWT
  is the Matrox plane write-mask register — on Parhelia **`0x1324`, index 0xC9**
  (see `parhelia-2d-accel-complete.md` §CORRECTION 1).  Something in the normal
  (non-FAA) path programs PLNWT as a workaround; in FAA mode that workaround must
  be suppressed, i.e. **FAA needs the full plane write mask** [INFERRED].  This is
  consistent with the alpha/coverage-channel reading above.
- **`AdjustZ`** sits inside the same block, between the FAA switches; likely a
  depth bias applied when fragments are cumulated [INFERRED, weak].

### 2.5 `reg3DThresholdAA` — reading the default value

Both defaults are, to within a bit, **half of the 24-bit depth range**:

```
0x007FE000 / 0xFFFFFF = 0.49951
0x007FFFFF / 0xFFFFFF = 0.50000
```

[INFERRED] `ThresholdAA` is a **depth threshold in S8Z24 units**: two fragments
at the same pixel are treated as the same surface (and their coverage merged)
when their depths differ by less than this, and as different surfaces otherwise.
A default of "half the Z range" is effectively "always merge", i.e. AA on by
default with maximum cumulation.

A competing reading, which the bit patterns also fit, is that the value is an
**IEEE-754 mantissa mask** (`0x007FFFFF` is exactly the float mantissa field;
`0x007FE000` is its top 10 bits) used to quantise Z before the same-surface
comparison.  Both readings agree that the parameter controls *fragment merging
tolerance*.  The vendor's plural — "the threshold value**s**" — hints the field
may pack two sub-fields (`0x7FE` vs `0x7FF` in bits [23:12]).  **[OPEN]** which.

### 2.6 `reg3DAAMode = 0x52` — NOT DECODED  [OPEN]

`0x52 = 0b0101_0010`.  No decode was recovered: the only Linux references are the
constructor sites, and the consumer lives in `MTXPARD.dll`.  Plausible shapes,
none confirmed:

- two 4-bit fields, `[3:0]=2` and `[7:4]=5` — e.g. "normal mode" and
  "dest-alpha/compressed-mask mode", which would pair naturally with
  `ForceCompressedMask`;
- three independent flags at bits 1, 4 and 6.

Do not guess this into a driver.  See §10 for how to settle it.

### 2.7 `reg3DFragSize` — a float  [INFERRED]

The value is an IEEE float (`1.0f` in the source, `4.0f` compiled in), and the
name is "fragment size".  The two readings that survive the evidence:

1. a **multiplier for the fragment-buffer allocation** — "reserve `FragSize` ×
   (one fragment slot per pixel) worth of storage".  A compiled-in default of
   4.0 with a source default of 1.0 fits a "we found 1× too small in practice"
   history.  This is the reading this document leans on;
2. a size in **megabytes** for the fragment buffer.

Under either reading, **FAA requires a driver-allocated side buffer whose size is
a tunable multiple of the render-target area** — which is the practically
important consequence and is stated as [INFERRED], not proven.

---

## 3. The fragment buffer  [OPEN — partially characterised]

What is established:

- It exists and is called that by the vendor: "sort-independent antialiasing
  **with the fragment buffer**" [VENDOR].
- It is written by the rasterizer for edge pixels only and consumed by a
  **`FRAG_RESOLVE`** operation [VENDOR, §5].
- It is *not* a Windows-miniport resource: `MTXPARM.sys` contains no AA string,
  no `Frag`/`Resolve`/`AAMode` reference [CONFIRMED negative].  Whatever allocates
  it does so through the ordinary surface/VRAM allocator, from the display driver.
- Its size is scaled by `reg3DFragSize` [INFERRED, §2.7].
- Its per-fragment payload includes a **coverage mask** with at least two
  encodings, one of which collides with destination alpha [INFERRED, §2.4].

What is **not** recovered: the allocation call site, the size formula, the
alignment/pitch, the base-address register and the size/limit register.  This RE
did not locate them; see §10.

**Consequence for an open driver:** FAA cannot be enabled by flipping a register.
It needs a memory allocation whose formula is unknown, so it is not a
"cheap win" feature — which is the main reason to skip it.

---

## 4. Configuration registers  [OPEN for FAA; the GL-side registers are recovered]

No register carrying `AAMode`, `ThresholdAA` or `FragSize` was located.

What *was* located is the **GL multisample state path** in the Windows ICD, which
is the nearest thing to an AA register set that this RE recovered.  See §6.4 —
three records in the **key = 0x5 bank at indices 0x1D, 0x1E, 0x1F**.  Since the
same ICD reports `GL_SAMPLE_BUFFERS = 0`, these are almost certainly the *GL*
multisample state registers (a mode bitfield plus `glSampleCoverage`), not the
FAA fragment-buffer controls.  They are nevertheless the only AA-adjacent
registers this RE can name.

Register-file conventions used above (from `parhelia-gl-driver.md` §Register
banks, unchanged here): the ICD emits `(key, index, value)` records;
**key = 0x0C is the main bank**, `reg = 0x1000 + index*4`; **key = 0x05 is a
second bank** whose absolute base is not static and must be resolved by capture.

---

## 5. The resolve step  [partially recovered]

The operation is named **`FRAG_RESOLVE`** [VENDOR, `MtxParhlParms.c:211`] and is
*sent* — i.e. it is a command in the stream, not a side effect of scanout:

> "Send a FRAG_RESOLVE even if alphablending was enable when rendering the scene
> and no fragments should be cumulated"

The two vendor switches enumerate two of its **triggers** [CONFIRMED as triggers,
[OPEN] as an exhaustive list]:

| trigger | switch | default | meaning |
|---|---|---|---|
| stencil-op change | `reg3DResolveOnStencilOpChange` | **on** | the fragment buffer cannot survive a change of stencil operation, so the driver flushes AA state first |
| alpha-blended scene | `reg3DResolveAlphaBlendedFrags` | **off** | normally a scene rendered with blending produces no cumulated fragments and the resolve is skipped; this forces it anyway |

Both are *defensive* switches, which tells you the resolve is normally issued at
scene end (before the buffer is presented or read back) and that these are extra,
mid-scene resolves.

That the resolve must be re-issued on a stencil-op change, plus the destination
alpha conflict of §2.4, is exactly the shape of the contemporary reports that
FAA-16x could not be combined with stencil-shadow rendering.

**[OPEN]:** the command opcode / register write that performs the resolve.  It is
not in any Linux binary (the Linux stack never enables FAA); it is in
`MTXPARD.dll`.

---

## 6. `GL_ARB_multisample` — how the ICDs map it  [CONFIRMED, cross-verified]

Both vendor ICDs — the Linux `libGL.so.1.3.0` (1.4.7.0, 2009) and the Windows
`MTXOGL.dll` (2.10.00.105, 2007) — are the same codebase.  Their multisample
state object is **byte-identical in layout**, which is what makes the following
cross-confirmed rather than merely observed.

### 6.1 The multisample state object — 24 bytes, identical on both platforms

Constructor: Linux `libGL` **@0x1c7380**, Windows `MTXOGL` **@0x10345fb0**
[CONFIRMED, both disassembled]:

| offset | GL state | initial value |
|---|---|---|
| +0x00 | `GL_MULTISAMPLE` enable | **1** (GL spec default: enabled) |
| +0x04 | `GL_SAMPLE_BUFFERS` | **0** |
| +0x08 | `GL_SAMPLES` | **0x10 = 16** |
| +0x0C | `GL_SAMPLE_ALPHA_TO_COVERAGE` | 0 |
| +0x0D | `GL_SAMPLE_ALPHA_TO_ONE` | 0 |
| +0x0E | `GL_SAMPLE_COVERAGE` | 0 |
| +0x10 | coverage value (f32) | **1.0f** |
| +0x14 | coverage invert | 0 |
| +0x18 | attrib-stack depth | 0 |
| +0x20 (win +0x1C) | attrib save stack | `new char[0x180]` = 16 entries × 24 B |
| +0x28 (win +0x20) | "multisample unavailable" flag | Linux: virtual call on the caps singleton at `0x65cf18`, vtable slot +0x10, arg 0. Windows: global byte `ds:0x104edbb8`. |

In the live context that object sits at **`ctx+0x7C70` (Linux)** /
**`ctx+0x74AC` (Windows)**.

**`GL_SAMPLES` is hard-coded to 16 — the FAA-16x sample count.**  This is the
clearest surviving trace of the intended feature: the ICD is built to claim 16
samples, but `GL_SAMPLE_BUFFERS = 0` means no multisample buffer is ever
attached, so per the GL spec multisampling is inactive regardless.

### 6.2 Visual / pixel-format enumeration — no multisample config, ever

Linux [CONFIRMED negative]:

- client GLX extension string is `GLX_ARB_get_proc_address GLX_EXT_visual_rating
  GLX_EXT_visual_info GLX_SGI_swap_control` — **no `GLX_ARB_multisample`**;
  `libglx.so` advertises only `GLX_EXT_visual_info/​_rating/​_import_context`;
- `glXGetFBConfigAttrib` @0x35d0a0 compares 23 attribute codes, none of which is
  `GLX_SAMPLE_BUFFERS` (100000 = 0x186A0) or `GLX_SAMPLES` (100001); those
  return `GLX_BAD_ATTRIBUTE`.  `glXChooseVisual` @0x35eb80 likewise.
- the two `0x186A0` immediates that do exist in `libGL` (@0x13b7f6, @0x13bbbc)
  are `GL_MAX_ELEMENTS_VERTICES/INDICES = 100000`, unrelated.
- `mtx_drv.so`'s twelve `0x186A0` occurrences are all `imul reg,reg,100000`
  unit conversions, not GLX attributes.

Windows [CONFIRMED] — `MTXOGL.dll` *does* handle `WGL_SAMPLE_BUFFERS_ARB`
(0x2041) and `WGL_SAMPLES_ARB` (0x2042) in its `WGL_ARB_pixel_format` attribute
builder @0x10043EA5…0x10043F3F, and this is where the dead FAA branch is visible:

```
10043eaa:  mov  DWORD PTR [eax+esi*1+0x38],ebx   ; pf[fmt].aa_samples = 0   <-- forced
...        ; WGL_SAMPLE_BUFFERS_ARB (0x2041) value object built with 0
10043eff:  mov  ecx,DWORD PTR [ecx+esi*1+0x38]   ; pf[fmt].aa_samples
10043f03:  neg  ecx
10043f05:  sbb  ecx,ecx                          ; ecx = aa_samples ? -1 : 0
10043f07:  and  ecx,0x10                         ; -> WGL_SAMPLES_ARB = 16 or 0
```

The field `pf[fmt]+0x38` is written in exactly one place (to zero) and read in
exactly one place, in the whole 5.2 MB DLL [CONFIRMED by exhaustive grep].  So
every pixel format reports `SAMPLE_BUFFERS = 0`, `SAMPLES = 0` — while the code
that would have reported **16** is still there, unreachable.

### 6.3 `glSampleCoverage` / `glSampleCoverageARB`

Linux `libGL`: exported thunks at 0x1ac0f0 / 0x1b09b0 dispatch through context
table slot **+0x9b0**.  The real implementation is **@0x128ab0** [CONFIRMED]:
clamp the value to [0,1] (helper 0x12ce30 against the `.rodata` constants
`0x367a0c = 1.0f`, `0x36992c = 0.0f`), store to `ctx+0x7c80`/`ctx+0x7c84`, set
dirty bit `0x10000` in `ctx+0x10`.  Other dispatch tables map the same slot to a
stub that raises `GL_INVALID_OPERATION` (0x502) or to a `repz ret` no-op.

`glGet` returns [CONFIRMED]: `GL_SAMPLE_BUFFERS` (0x80A8) → `ctx+0x7c74`,
`GL_SAMPLES` (0x80A9) → `ctx+0x7c78`, `GL_SAMPLE_COVERAGE_VALUE` (0x80AA) →
`ctx+0x7c80`, `GL_SAMPLE_COVERAGE_INVERT` (0x80AB) → `ctx+0x7c84`.  The four
*enable* tokens `GL_MULTISAMPLE` (0x809D), `GL_SAMPLE_ALPHA_TO_COVERAGE` (0x809E),
`GL_SAMPLE_ALPHA_TO_ONE` (0x809F) and `GL_SAMPLE_COVERAGE` (0x80A0) are **absent
from the `glGet` switch** and fall through to the default arm @0x13adb6 which
raises `GL_INVALID_ENUM` (0x500) — a spec bug.  They *are* handled by
`glIsEnabled` (@0x1271c0).

`glEnable(GL_MULTISAMPLE)` is **gated**: @0x125148 it only sets the enable byte
when the "multisample unavailable" flag at `ctx+0x7c98` is zero.  That flag comes
from the device-capability query listed in §6.1.

### 6.4 Linux: state-only.  Windows: state **plus** a command-stream emitter

On Linux the multisample fields are read by nothing except `glGet`,
`glIsEnabled` and push/pop-attrib; the dirty bit `0x10000` is tested only by the
attribute save/restore and context-copy paths.  **No hardware emitter reads them**
[CONFIRMED by exhaustive reference enumeration of `ctx+0x7c70/74/78/7c/7d/7e/80/84`].

On Windows the same fields *are* read by the lazy state-flush routine, which
emits three records into the ICD→driver command stream
[CONFIRMED, `MTXOGL.dll` @0x103A116A…0x103A1273]:

| site | record | payload |
|---|---|---|
| 0x103a11c0 | key **0x05**, index **0x1D** | mode bitfield (below) |
| 0x103a120a | key **0x05**, index **0x1F** | `glSampleCoverage` value, raw f32 bits |
| 0x103a124d | key **0x05**, index **0x1E** | `glSampleCoverage` invert, 0/1 |

The index-0x1D bitfield is packed at 0x103a116A–0x103a11AA:

```
bit 0 = GL_MULTISAMPLE                (ctx+0x74ac)
bit 1 = GL_SAMPLE_ALPHA_TO_COVERAGE   (ctx+0x74b8)
bit 2 = GL_SAMPLE_COVERAGE            (ctx+0x74ba)
bit 3 = GL_SAMPLE_ALPHA_TO_ONE        (ctx+0x74b9)
bits[31:4] = (a stale stack slot holding the viewport Y) & ~0xF   <-- see note
```

The upper-bits term is `[esp+0x14]`, which in that stack frame was last written
at 0x103a0db9 from `ctx+0xcd8` — the **viewport Y** of the `key 0x26` viewport
record emitted a few hundred bytes earlier.  Either the register genuinely
carries an unrelated value in bits [31:4] and the ICD is re-using the cached
copy, or this is a stale-slot bug in a code path that never runs in shipping
configurations (which it does not, since `SAMPLE_BUFFERS = 0`).  **[OPEN]** —
flagged rather than resolved, because building on it would be unsafe.

Record-encoding cross-check, so the key/index reading above can be trusted
[CONFIRMED]: the same emitter writes the fog state as key **0x0C** index
**0x8C**, and `parhelia-gl-feature-map.md` §Fog records the live-captured
`glFogi(GL_FOG_MODE, GL_LINEAR)` → **register `0x1230` = 3**, and
`0x1000 + 0x8C*4 = 0x1230`.  The allocator signature is
`AllocRecord(this, key, payload_bytes, count)` — Linux `libGL` @0x1c9c50
(`esi=key, edx=size, ecx=count`), Windows `MTXOGL` @0x10003860 (stdcall, same
order) — and it writes the two-dword `(key, count)` header itself, which is
exactly the record grammar documented in `parhelia-gl-feature-map.md` §2.

**Note for the existing register map:** this also means the bare
"(index, value) register pairs" seen in the Linux capture are each preceded by a
`(0x0C, 1)` header.  That accounts for the value-less entries `0x1030`, `0x1020`,
`0x1098` in the captured state list of `parhelia-3d-state-complete.md` — index
0x0C is `0x1030`.  Worth re-checking in the decoder.

---

## 7. Edge, line, point and polygon smoothing  [CONFIRMED]

These are **independent of FAA** and were fully recovered from Linux `libGL`.

| cap | state byte | implementation |
|---|---|---|
| `GL_POLYGON_SMOOTH` (0x0B41) | `ctx+0x9f4` | **silently ignored.**  The complete reference set is glEnable @0x125710, glDisable @0x125e14, glIsEnabled @0x1275b6 and the four `glGet` arms — no renderer reads it.  (Contrast the adjacent `GL_POLYGON_STIPPLE` byte at `ctx+0x9f5`, which *is* consumed at 0xeaa2d / 0xf323d.) |
| `GL_LINE_SMOOTH` (0x0B20) | `ctx+0xaac` | **emulated in geometry.**  @0xe7245 the line routine branches to 0xe74e1, normalises the direction (`pow(dx²+dy², 0.5)` via `pow@plt`), offsets both endpoints by ±0.5·width and emits a **float-positioned widened quad**.  With smoothing off it instead replicates `(int)(width+0.5)` one-pixel lines.  No AA bit is set anywhere. |
| `GL_POINT_SMOOTH` (0x0B10) | `ctx+0xad0` | **the only one that reaches the chip.**  @0x114343 it selects the point-size clamp range: smooth → [0.25, 10.0] (`GL_SMOOTH_POINT_SIZE_RANGE`), aliased → [1.0, 2048.0] (`GL_ALIASED_POINT_SIZE_RANGE`), and then emits the clamped min and max as main-bank (key 0x0C) records at indices **0x9B** and **0xA6**. |

The point-size max is corroborated by capture: `0xA6` → `0x1000+0xA6*4 = 0x1298`
and the live draw capture in `parhelia-3d-state-complete.md` shows
`0x1298 = 0x45000000 = 2048.0f`, i.e. exactly the aliased point-size upper clamp
— which identifies that register better than the previous "viewport/scale" guess.
The companion index `0x9B` would be `0x126c`, which `parhelia-fragment-isa.md`
assigns to the fragment combiner; that is a **conflict**, left flagged, not
resolved.  (`parhelia-gl-driver.md` already documents this register file as
format-multiplexed in places.)

So: Parhelia has classic **antialiased points** in hardware, software-emulated
smooth lines, and no polygon smoothing.  None of this touches the fragment
buffer.

---

## 8. Interactions and restrictions

From the switch semantics, all [INFERRED] unless marked:

| interacts with | evidence | effect |
|---|---|---|
| **destination alpha** | `ForceCompressedMask` [VENDOR] | forces a *different* AA mode; the default coverage-mask encoding is unusable when the app needs destination alpha |
| **plane write mask (PLNWT `0x1324`)** | `ForcePLNWMASK4FAA` [VENDOR] | a PLNWT workaround used elsewhere must be suppressed in FAA mode |
| **stencil** | `ResolveOnStencilOpChange`, default **on** [VENDOR] | a stencil-op change forces a fragment resolve — FAA cannot span a stencil-op change |
| **alpha blending** | `ResolveAlphaBlendedFrags`, default **off** [VENDOR] | a blended scene normally cumulates no fragments, i.e. **blending effectively defeats FAA** |
| **depth** | `ThresholdAA` (§2.5), `AdjustZ` | fragment merging is depth-thresholded; the tolerance is tunable, which implies visible artefacts when it is wrong |
| **memory** | `FragSize` (§2.7) | a side allocation proportional to render-target area |
| **render-to-texture / read-back** | resolve is explicit | any read of the colour buffer must be preceded by a resolve |
| **chip variant** | `parhelia-variants.md` | FAA-16x on Parhelia-512 (0x0520/0x0521/0x0527/0x0528); "mode-gated" on the later P-series/P690 parts |

No performance cliff guard was found in the Linux binaries (there is no FAA code
there to guard).  The one clear "cliff" the design implies is fragment-buffer
exhaustion: a scene with a very high edge-pixel count overruns whatever
`FragSize` reserved, at which point quality must degrade.  **[OPEN]** whether the
hardware signals that or silently drops fragments.

---

## 9. Recommendation for the open driver

1. **Do not implement FAA.**  Report `GL_SAMPLE_BUFFERS = 0`; do not advertise
   `GLX_ARB_multisample` and do not expose multisample FBConfigs.  This matches
   the vendor stack exactly on both platforms, so no application regresses.
2. If `GL_ARB_multisample` is advertised for API-completeness, track
   `glSampleCoverage` state and return it from `glGet`, but do not claim samples.
   Do **not** copy the vendor's bug of raising `GL_INVALID_ENUM` for
   `glGetIntegerv(GL_MULTISAMPLE)`.
3. `GL_POINT_SMOOTH` is worth wiring up — it is real hardware and costs two
   register writes (§7).  `GL_LINE_SMOOTH` is a geometry widening in the vertex
   path.  `GL_POLYGON_SMOOTH` can be ignored, as the vendor does.
4. Leave `0x1074`-adjacent / key-0x5 indices `0x1D`–`0x1F` alone unless and until
   the key-0x5 bank base is resolved.

---

## 10. What would settle the rest

In priority order.  Everything below needs `MTXPARD.dll` (32-bit PE, image base
0x10000000; pre-made disassembly at `scratchpad/pard.asm`, helper
`scratchpad/pf.sh <startvma> <endvma>`), because it is the only binary in this
tree that consumes the AA configuration.

1. **`AAMode` decode.**  Find the RVA of the string `"AAMode"` in `MTXPARD.dll`,
   find the registry-switch construction that references it, then follow the
   stored value to every `cmp`/`test`/`and`/`shr`.  That yields the mode enum and
   almost certainly the AA-enable register.
2. **The fragment buffer.**  From the `AAMode` consumer, walk to the surface
   allocation performed when AA is enabled; the size expression and the
   base-address register write are adjacent.  This is the one item an open driver
   would truly need.
3. **`FRAG_RESOLVE`.**  Search for the code reached from a stencil-op change and
   from end-of-scene that is guarded by the AA-enabled flag; the opcode or
   register write it performs is the resolve.
4. **`ThresholdAA` / `FragSize` registers.**  Both are written verbatim (or after
   one shift) into a register; finding either confirms §2.5 and §2.7.
5. **Live confirmation, if a Windows install is available:** set
   `HKR\3D.AAMode` to a non-default value and capture the command stream.  On
   Linux this is not possible — the parameters are not read (§2.3).
6. **The key-0x5 bank base**, which is an already-recorded open item in
   `parhelia-gl-driver.md`; resolving it also gives the multisample registers of
   §6.4 an absolute address.

---

## 11. Address index (for re-checking)

`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`
: `mtx722` @0x74950 registry-switch table; AA ctor arms @0x76270 (`ThresholdAA`),
  0x762d4 (`AAMode`), 0x76300 (`FragSize`), 0x76329, 0x76352, 0x7637b, 0x763a4,
  0x763ca.  `RegistrySwitch<u32>` ctor `mtx2860` @0xbc470, `<bool>` `mtx2302`
  @0xbc540, list-add `mtx2098`.  Second copy of the table: `mtx1049` @0x765f0.

`matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so`
: `mtx724` @0x60d70; string block @0x110bab…0x110c0e; section `"3D"` @0x11078c.

`matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0`
: multisample state ctor @0x1c7380; `glSampleCoverage` impl @0x128ab0, thunk
  @0x1ac0f0, ARB alias @0x1b09b0, dispatch slot +0x9b0; `glEnable` @0x124fd0
  (`GL_MULTISAMPLE` arm @0x12513c); `glDisable` @0x125780; `glIsEnabled`
  @0x1271c0; `glGetIntegerv` @0x12b850; `glXGetFBConfigAttrib` @0x35d0a0;
  `glXChooseVisual` @0x35eb80; record allocator @0x1c9c50, commit @0x1c9ad0;
  line-smooth quad expansion @0xe74e1; point-size emitter @0x114343.
  Context field base: multisample block @`ctx+0x7c70`.

`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll` (image base 0x10000000)
: multisample state ctor @0x10345fb0; state flush / record emitter
  @0x103a116a–0x103a1273; `glSampleCoverage` impl @0x103b5890, `wglGetProcAddress`
  registration @0x1002181c/0x10021857; WGL pixel-format attribute builder
  @0x10043d10 (multisample arms @0x10043ea5–0x10043f3f); record allocator
  @0x10003860 → 0x10003640.  Context field base: multisample block @`ctx+0x74ac`.

`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`
: contains the strings `AAMode`, `ThresholdAA`, `FragSize`,
  `ResolveAlphaBlendedFrags`, `ResolveOnStencilOpChange` — the consumer side,
  **not yet decoded**.
