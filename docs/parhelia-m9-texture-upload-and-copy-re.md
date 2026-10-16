# M9 texture upload, copy and non-local storage: what the evidence says

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->

This note settles three questions that were being answered from this driver's
own limitations rather than from the hardware evidence: how texture data
reaches tiled storage, whether the M9 can sample from system memory, and
whether it can sample linear storage.  All three of this driver's current
restrictions turn out to be policy, not hardware.

Labels: **CAPTURED** = a saved vendor command stream or VRAM dump in
`captures/`; **STATIC** = a saved vendor binary; **MEASURED** = clean-room
hardware; **CODE** = this driver; **DERIVED** = a conclusion constrained by
those.

## 0. Corrections to the working assumptions

1. *"The M9 has no texture DMA and cannot texture from system RAM."*
   **Refuted, CAPTURED.**  The vendor binds a texture base in class-1 system
   memory: `78 = 40c3a001`, 84 times, in
   `captures/m9x-vendor-glamor-shaders-20260915/gradient-{linear,radial}-18`.

2. *"No linear buffer can be sampled."*  **Refuted, CAPTURED.**  The same bind
   carries `79 = 68180800`: descriptor kind `0x18`, whose layout selector is
   zero, which is the plain row-major layout.  Read the whole of section 4
   before acting on that: a linear **1-D** texture is sampled, and a linear
   **2-D** one is MEASURED not to be.

3. *"Our uploads are CPU-side because the physical address permutation inside
   a block was never recovered."*  **Refuted twice.**  The permutation *is*
   recovered — `m9x659` for the packed tail
   (`parhelia-m9-blocked-layout-boundary.md`, `parhelia-m9-texture-storage.md`)
   and a GF(2) basis per level size for the large POT levels
   (`tools/m9-texture-large-analyze.py`, 0 failing texels).  And only levels
   inside the 32x16 tail take the per-texel CPU path today: every larger level
   already goes through `DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS`, where the
   host-aperture descriptor makes the *hardware* do the permutation
   (`m9_texture_window()` in `m9_context.c`, `m9_surface.c` in the kernel).
   The CPU cost that remains is the aperture bandwidth, the bounce buffer, the
   ioctl and a transport drain — not a missing permutation.

4. *"MRT is fully reverse engineered"* — unrelated to this note, but the same
   failure mode: a captured *surface* side was read as a captured whole.

## 1. The descriptor word is one word, and its layout field is explicit

Indices `78/79/7a` (texture) and `b0/b1`, `b2/b3`, `b4/b5`, `b6/b7` (colour
targets 0..3), `b8/b9` (depth), `ba/bb` (auxiliary) all carry the **same**
address/descriptor pair.  `m9_surface_desc0()` in `m9_batch.c` already splits
it, and it matches the vendor field map in `parhelia-m9-texture-state.md`:

```text
[15:0]  aligned row span in elements
[18:16] allocation-layout selector   0 = row-major/linear, 1 = blocked
[20:19] dimension       0 = 2-D, 1 = cube, 2 = volume, 3 = 1-D, 4 = large 2-D
[21]    local memory vs host-aperture address family
[23]    resource-layout flag
[31:25] 7-bit hardware format code
```

The five kind bytes the captures show decompose cleanly:

| kind `[23:16]` | selector `[18:16]` | dimension `[20:19]` | bit 21 |
|---:|---:|---:|---:|
| `01` small 2-D | 1 | 0 | 0 |
| `09` cube | 1 | 1 | 0 |
| `11` 3-D | 1 | 2 | 0 |
| `18` 1-D | **0** | 3 | 0 |
| `21` large 2-D | 1 | 4 | 1 |

Selector 0 meaning linear is corroborated four ways: the captured 1-D texture
stores texel x at `4*x` (`parhelia-m9-texture-storage.md` section 2); the
kernel's identity aperture view is `0xf0000000`, i.e. selector 0, and is
described and used as "a linear view of any video memory address"
(`m9_address.c`, MEASURED on every raw VRAM read); the vendor D3D builder has
a path that clears `[18:16]` for a render surface
(`parhelia-m9-render-surfaces-clear.md`); and the same three bits are a
PROVEN layout enum with `0 = plain linear buffer` on the previous generation
(`parhelia-fragment-gating-re.md`, `parhelia-3d-integrated.md`).

## 2. The engine upload path

### The vendor's own upload command is not decoded, and is not needed

STATIC: the Windows D3D user-mode driver reaches a GPU transfer through
`0xb21b0 -> 0x56a10/0x56c60 -> 0x1b8d0 -> 0x1b650`, which takes logical
rectangles plus a source and a destination descriptor and posts to requester
MMIO `0x504` (`parhelia-m9-blocked-layout-boundary.md`,
`parhelia-m9-texture-coherency-re.md`).  Which indexed registers `0x1b650`
writes has never been recovered, and no capture in this tree contains that
route: every captured submission comes through `ExecuteDma` from the Linux GL
ICD.  The vendor Linux GL uploads texels with the CPU.

There is also no captured 2-D blit.  All 606 decodable submissions across the
768 saved `ExecuteDma` pairs were swept:

* index `0x2a` — the auto-index template `parhelia-m9-accel.md` describes for
  a copy — **never occurs**, nor do `0x2b`, `0x2c`, `0x2d`, and a raw byte
  scan of all 781 command images finds **zero** occurrences of either literal
  template `0x4545FCF3` or `0x45FCF2F3`;
* `f0` (2582 writes), `f2` (478), `f3` (478), `f4` (2178) and `fc` (470) are
  real, but `f2` — the claimed source origin — is `0x00000000` in **every one
  of its 478 writes**, and `fc` always equals `f4`.  That family is the
  vendor's vertexless rectangle fill/clear launch, not a copy;
* `e6` never carries the claimed `0x20000000 | (count-1)` GO encoding.

Only the vendor OpenGL ICD produced command streams at all.  No vendor
DDX/EXA submission exists anywhere in this tree, so the 2-D copy encoding in
`parhelia-m9-accel.md` remains an unverified STATIC lead.

It does not matter, because the M9 has no separate copy block.  CAPTURED:
X11 `CopyArea` through the vendor's glamor path is an ordinary textured quad —
`captures/m9x-vendor-glamor-shaders-20260915/copy-area/m9x-exec-4601-000014-bm0-in.bin`:

```text
0x02e0  b0 = 127f9000  b1 = cc210080                  destination pixmap
0x0410  78 = 12831000  79 = c8010004  7a = 00010004   source as texture unit 0
0x04d8  draw-numeric-range start=0 end=6              two triangles
```

The vendor's `glCopyTexSubImage2D` state emit has the same binding shape
(`captures/m9x-vendor-gl2ext-2d-video-modes-20260915/A-gl2ext/copytexsubimage-back/m9x-exec-4268-000023-bm4-in.bin`,
`b0/b1 = 12839000/c8210080` destination, `78/79/7a = 127f9000/cc210080/00010080`
source), although that saved pass carries no launch — the harness ran with
`limit=4` and the copy's own draw was not among the saved submissions.

### CAPTURED: the raster back end writes texture-layout surfaces

Kind `01` and kind `21` descriptors are bound at the *destination* indices
with a launch present, and the identical descriptor word on the identical
base is bound later at the sampler indices.  The cleanest instance is a shadow
map in `captures/m9x-vendor-openbits-20260917/cap-shadowmap_d24_lequal`:

```text
exec-8679-000016 bm0 0x0c20   b0 = 120c9000  b1 = c8010040   colour destination
                     0x0c30   b8 = 120d1000  b9 = fa010040   depth destination
                     0x0df0   draw-numeric-range 0..0x100    launch
exec-8679-000018 bm0 0x0e48   78 = 120d1000  79 = fa010040  7a = 00010040
```

`fa010040` is format `fa` (D24), kind `01`, 64 wide: rendered into as depth,
then sampled as a depth texture, same word, same base.  The colour pair
behaves the same way.  `captures/m9x-linux-gl2-matrix-20260914/fbo-chain`
repeats it for RGBA8 with a verified read-back — producer sequence 14 binds
`b0/b1 = 11f21000/c8010040`, consumer sequence 15 binds
`78/79/7a = 11f21000/c8010040/00010040` — and
`parhelia-m9-vram-render-target-join.md` records a third case at `c8010020`
with matching pixel values at both ends.  Across the whole capture set, 545
render passes with a launch write a kind `21` pixel destination and 16 write a
kind `01` one.

(`parhelia-m9-texture-storage.md` also states that `glCopyTexImage2D` renders
on the GPU into the texture at `11f89000` and the copied texels land at the
CPU-upload addresses.  That one is DERIVED from the VRAM dump rather than from
a captured packet; the binds above are the packet-level evidence.)

**The layout is a property of the descriptor, not of the engine unit.**  The
raster back end applies the permutation on the write side exactly as the
sampler applies it on the read side.  An engine-side upload or copy therefore
needs no packet this tree has not already decoded, and no address permutation
at all.

This also explains the `m9x474`/`m9x659` split that looked like a conflict
between render targets and textures: it is bit 21 of the descriptor, the
local/aperture address family, and both the sampler and the raster back end
honour it.  It is why the kernel's surface window must use the small kind for
a surface of 64x64 or less — `m9_surface_access()` in `m9_context.c` records
that MEASURED result already.

### Two corrections this sweep forces on `parhelia-m9-accel.md`

* `b8/b9` is the **depth** surface and `b6/b7` is **colour attachment 3**.
  That file's "RT0 base `0xb8` / RT0 mode `0xb9` / copy source `0xb6`/`0xb7`"
  table is wrong on both counts, as its own warning banner suspected.  The
  captured pairs are `b0/b1`, `b2/b3`, `b4/b5`, `b6/b7` for colour 0..3,
  `b8/b9` depth, `ba/bb` hierarchical Z — which is what
  `parhelia-m9-render-surfaces-clear.md` and `parhelia-m9-mrt-routing.md`
  already say.
* The `f0/f2/f3/f4/fc` launch is a **rectangle fill**, not a copy.  It is
  worth its own look for fast clears: the M9 clears a surface with no geometry
  at all, `fc` carrying the same extent as `f4` and `90/92` the colour and
  mask.  One captured case fills at a non-zero destination origin
  (`f3 = 00160001`, a 320x240 fill at (1,22)).

### What this makes possible

Every texture level this driver allocates is already a valid colour-target
shape:

* levels above 16x16 are laid out by `m9_render_surface_layout()`, the render
  family, and are bound through the kernel window as kind `21`;
* levels of 16x16 and below share one 32x16 atlas, which is a kind `01`
  surface with the level at an origin inside it.

So `resource_copy_region` and an unfiltered `blit` can be a raster draw:
destination level bound at `b0/b1` with the descriptor `m9_texture_window()`
already computes for it, source bound at `78/79/7a` as it is bound for
ordinary sampling, one quad, a passthrough fragment program.  Both are CPU
transfer loops today (`util_resource_copy_region` and `m9_blit_convert`), and
a scaled, scissored or filtered blit is refused outright.

## 3. System memory

The M9 is not the Parhelia `base = offset | class_tag` model.  It has a real
GPU page table — a compact one-dword-per-page table in VRAM, `valid = pte & 8`,
40-bit physical page number — and a class-1 GPU virtual window at
`M9_ADDRESS_SYSTEM_BASE = 0x40000000`, whose end is set by
`M9_ADDRESS_EXTRA_PAGES = 0x8000` pages of table headroom above VRAM, i.e.
128 MiB on this 1 GiB board.  Low address bits are field-specific tags, not a
memory class: `|1` on a translated class-1 GPUVA, `|2` on a raw bus address.
The Parhelia two-bit tag survives only in the M9 2-D surface slot encoding,
where no capture ever shows a non-zero value.

CAPTURED, the decisive bind
(`captures/m9x-vendor-glamor-shaders-20260915/gradient-linear-18/m9x-exec-6587-000013-bm4-in.bin`
at stream offset `0xc30`, and three further images):

```text
70 = 0004111c
78 = 40c3a001   79 = 68180800   7a = 00010001
7c = 7fc09200   7d = 0ff80000   87 = 00000000
```

`0x40c3a000` is an output of the vendor's mode-1 allocator in the same case
directory (`m9x-alloc-6587-000000-out.bin`), one of a series at stride
`0x21000` in the class-1 domain, with its own mmap cookie.  The `|1` tag is
the same one the kernel puts on `SYSTEM` and `RESOURCE_6C` relocations.  The
descriptor is format `0x34` — `A32B32G32R32F` — kind `0x18`, 2048 elements.

The shader explains it: `gradient-linear-18` declares
`uniform float stops[18]; uniform vec4 stop_colors[18];` and indexes them with
a loop variable.  The Matrox GLSL compiler lowers a dynamically indexed
uniform array into a fetch from a linear float 1-D texture held in a
system-memory constant arena.

Three of this driver's assumptions fall out of that one packet: a texture base
may be non-local, a sampled surface may be linear, and the sampler decodes
128-bit float texels.  The last is worth carrying to
`parhelia-m9-implementation-backlog.md`, which lists float textures under
"needs capture before it can be implemented at all".

CODE: this driver refuses all of it.  `m9_reloc_uses_vram()` in the kernel's
`m9_submit.c` lists `PARHELIA_M9_RELOC_TEXTURE`, so a texture relocation is
rejected unless the object is in VRAM, and a kernel selftest asserts that.
Mesa never offers anything else: `m9_resource_bo_flags()` places every
non-buffer resource in VRAM.

**Not established:** a *tiled* 2-D texture in system memory.  The only
captured non-local texture is linear and one-dimensional, and the vendor's own
address calculator keeps a separate leaf family for the aperture address
placement, so the blocked layout may well differ off-board.  Do not assume it.

## 4. Linear storage

CAPTURED: kind `0x18`, layout selector 0, stores texel x at `4*x`
(`parhelia-m9-texture-storage.md` section 2, and the model in
`tools/m9-texture-layout-analyze.py`).  `captures/m9x-linux-gl15-ff-20260915`
case `tex1d_nearest` shows `79: c8010010 -> c8180010` with `7a: 00010010 ->
00010001`, i.e. only the kind byte and the height change.  So **linear
sampling is not a hardware restriction.**

CODE: this driver refuses linear textures twice over.  `m9_texture_bind_view()`
rejects a `PIPE_TEXTURE_1D` view outright, and `m9_resource_create()` excludes
`M9_TEXTURE_KIND_1D` from the tiled layout so a 1-D texture lands in the
CPU-only `m9_mip_layout` that no sampler path accepts.  The one hardware
linear mode with a captured model is dead code: `m9_texel_offset()` implements
`lvl->offset + 4*x` and only `m9_batch_test.c` exercises it.  The screen also
advertises `DRM_FORMAT_MOD_LINEAR` for import while no imported linear buffer
can ever be sampled — an inconsistency in our own stack, and the source of
`texture is not stored in a captured sampler layout; draw skipped`.

Linear 2-D surfaces are captured too, and they are not rare.  Kind byte `00`
is dimension 0 with layout selector 0 — an ordinary linear 2-D surface — and
the vendor uses it constantly:

* **Engine writes, with a height.**  In
  `captures/m9x-vendor-gl2ext-2d-video-modes-20260915/A-gl2ext/copytexsubimage-back/m9x-exec-4268-000021-bm3-in.bin`
  at `0x8b8`, the rectangle-fill path binds `b0/b1 = 12829000/f0000010` —
  format `78` (raw 32-bit), kind `00`, width 16 — and launches
  `f4 = fc = 00280010`, i.e. **16 wide by 40 rows**.  The same allocation is
  bound later in the frame as the hierarchical-Z surface with
  `bb = f0010080`.  So one object is addressed once as a 16x40 linear surface
  and once as a kind `01` 128-wide one, and the engine writes both.
* **Large linear extents.**  The vertex-output targets are bound at
  `b0/b2/b4 = 113c1000/113d7800/113ee000` with `68002000`: format `34`
  (128-bit), kind `00`, width `0x2000` = 8192 elements
  (`parhelia-m9-vertex-output-target-re.md`).
* **The sampler accepts kind `00`.**  Every rectangle fill binds
  `78/79/7a = <base>/f0000001/00010001` — a 1x1 linear 2-D texture.  That is
  the mandatory unit-0 bind this driver already reproduces.

DERIVED from the fill above: the row stride of a linear surface is the
descriptor's `[15:0]` width field in elements, not a 32-aligned span.

**Settled on hardware, and the answer is no.**  MEASURED on the M9128,
2026-09-18.  A probe (`M9X_TEX_FORCE`, `M9X_LINEAR_TEX`) keeps a sampled
single-level image in the texture layout and switches only its storage and
its kind byte, so nothing else about the path changes:

| case | storage | kind | result |
|---|---|---:|---|
| `tex:16x16` | tiled, written through the raw map | `01` | **PASS** |
| `texture` 128x128 | tiled, written through the kernel window | `21` | **PASS** |
| `tex:16x16` | linear | `00`, `08`, `10`, `18`, `20` | all **FAIL** |
| `texture` 128x128 | linear | `00`, `20` | **FAIL** |
| `texture` 128x128 | linear | `01`, `21` (wrong-kind controls) | **FAIL** |

Every failure is 76800 of 76800 pixels.  The `tex:16x16` tiled case is the
control that matters: it writes through the *same* raw mapping the linear
cases use and samples correctly, so the write route is engine-faithful and the
layout is the only variable left.

So the sampler reads the blocked layouts and the captured linear 1-D, and does
not read a linear 2-D surface at any of the five dimension encodings.  That is
consistent with the captures, where every sampled linear surface is either 1-D
or 1x1, while the linear 2-D surfaces the engine writes -- the hierarchical-Z
plane and the vertex-output targets -- are never sampled.

## 5. What is implementable, and what each step still needs

In dependency order, with what the hardware said on 2026-09-18.

1. **DONE.**  Engine-side `resource_copy_region` and `blit`.  Fully
   supported by section 2: a raster draw with the destination level bound as
   colour target 0 and the source bound as a texture unit, both with
   descriptors this driver already computes.  Needs no new decode.  Hardware
   check: copy a coordinate-coded texture between two levels and read both
   back, once through the kernel window and once through the raw map, so an
   engine write into the packed tail is checked against the CPU formula rather
   than assumed to agree with it.

2. **DONE.**  Filtered and scaled blit on the same mechanism, since the
   sampler does the filtering.

3. **BLOCKED by E2.**  Engine-side texture upload.  The vendor's design is linear CPU staging
   plus a GPU transfer, and every part of it is now evidenced: linear 2-D
   surfaces exist and the engine writes them, and the sampler accepts a linear
   2-D descriptor at 1x1 and a linear 1-D one at 2048 elements.  The one
   missing step was sampling a linear 2-D surface above 1x1, and it is
   MEASURED not to work.  What remains is a linear **1-D** staging texture,
   which is CAPTURED: fed in runs of up to 2048 texels, with the coordinate
   `u = (y*w + x)/(w*h)`, which is affine in x and y and so interpolates as an
   ordinary varying and needs no shader arithmetic.  That is the shape of the
   next attempt, and it needs the kernel relocation rule below as well.

   The staging buffer in class-1 system memory is worth 1380 MB/s against the
   aperture's measured 260 MB/s (`m9x-frame-cost-breakdown`), and removes the
   ioctl and the transport drain from every upload.

5. **A vertexless clear**, which fell out of this sweep rather than being
   looked for.  The vendor clears a surface with the `f0/f3/f4/fc` rectangle
   launch and no geometry at all, binding the target with the raw format code
   `78` and `90`/`92` as colour and mask.  This driver's full-screen clear is
   measured at 1.19 ms (`parhelia-m9-fill-rate.md`); whether the rectangle
   path is cheaper is worth one measurement.

4. **BLOCKED by E2.**  System-memory texture placement — only after the tiled-in-system-memory
   question in section 3 is answered, and only for the storage kind the answer
   covers.

### What was measured, 2026-09-18

**E2 (linear 2-D sampling): negative**, in the table in section 4.  This kills
the linear staging route for uploads, and with it system-memory *texture*
placement: the only captured non-local texture is linear and one-dimensional,
and there is no linear 2-D layout to put a 2-D texture in.

**E4 (engine copy and blit): positive, and implemented.**  `resource_copy_region`
and `blit` run on the raster back end through `util_blitter`, with the CPU
transfer path kept as the fallback.  Fixtures `blitlin` and `blitlin:32x128`
magnify with `GL_LINEAR`; they fail on the pre-change binary with 76776 and
76720 of 76800 pixels wrong, because the CPU path ignored the filter and
sampled nearest, and are exact afterwards.  165 of 165 fixtures pass.

Taking the engine path needed one unrelated fix.  `util_blitter` builds its
shaders with `ureg`, which delivers them as **TGSI**, and `m9_create_fs_state`
only reached the native backend for NIR.  Every blitter draw was dropped with
a null reason, so the engine path looked available and silently did nothing.
TGSI is now converted and its IO lowered.  Any Gallium helper shader was
affected by this, not just the blitter.

**E1 (the kernel window's linear mode): not run, superseded.**  Its question
was whether a linear 2-D surface shape exists at all.  The captures already
answer that -- the engine writes kind `00` surfaces 16 wide by 40 rows and
8192 elements wide -- and E2 answers the part that governs a driver, which is
whether the *sampler* reads one.  Running E1 would cost a kernel change and a
reboot to refine a question no longer on the path to an implementation.

**E3 (a non-local texture base): not run, blocked by E2.**  Extending the
kernel's texture relocation to a class-1 GPUVA only pays off for a layout the
sampler can read from there, and the single captured case is a linear 1-D
lookup table.

### The kernel's texture relocation rule

`m9_reloc_uses_vram()` in `m9_submit.c` lists `PARHELIA_M9_RELOC_TEXTURE`, and
a kernel selftest asserts it, so a texture base must be in VRAM.  **That is
stricter than the hardware**: the vendor binds `78 = 40c3a001`, a class-1
system GPUVA with the `|1` tag, 84 times in
`captures/m9x-vendor-glamor-shaders-20260915`.  The correct rule is the one
the stream slots `00..09` already use -- the address class is in the tag, not
in the slot -- so index `78` should accept a VRAM base untagged **or** a
class-1 GPUVA as `bo->system.start + target_offset | 1`, exactly as
`PARHELIA_M9_RELOC_SYSTEM` resolves it.

It is left unchanged here on purpose: relaxing it without a use would widen
what a submission may reference for no measured gain, and the only use the
evidence supports is a linear 1-D texture, which this driver cannot yet
produce.  The selftest assertion is the part to watch, because it will keep
the restriction alive long after the evidence has moved; it should be
rewritten to assert the rule above when the 1-D path lands.

### The experiments, in the order that costs least

**E1, kernel only, no sampler.**  `m9_surface.c` accepts only kind bytes `01`
and `21`.  Relax it locally to accept `00`, write a coordinate-coded image
through the window over a BO the CPU also reads raw, and compare.  This tests
the address unit's linear mode directly, in the same block whose selector-0
identity view already works, and confirms the row stride is the descriptor's
width field.  No job is submitted.

**E2, sampler.**  Bind a coordinate-coded linear BO at `78/79/7a` with
`79 = c8000040`, `7a = 00010040` (kind `00`, 64x64) and sample it; `c8010040`
over the same bytes is the control and must come back scrambled.  This is the
one remaining gap.  Repeat once with bit 21 set to separate the address family
from the layout selector, and once at `7a = 00010004` with kind `18` to see
whether the 1-D mode also takes rows.

**E3, non-local.**  Extend the kernel's texture relocation to accept a class-1
GPUVA — the address form is already built for `SYSTEM` and `RESOURCE_6C` — and
sample the linear 1-D case the vendor capture shows, byte for byte, before
trying anything tiled or two-dimensional.

**E4, engine copy.**  Item 1 above.

Each of these must open with `m9-mesa-glsl texture` on a freshly booted
target, because a stream timeout or a plain module swap leaves the device
returning `-EBUSY` with no new log line.
