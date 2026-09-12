# GigaColor on the P690: 2:10:10:10 as sampler, render target and scanout

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Matrox marketed "GigaColor" as ten bits per colour channel throughout the
pipeline.  This note separates what the vendor binaries **prove** from what the
P690 silicon **does**, and it supersedes the earlier
`parhelia-gigacolor-textures.md` and section 1 of
`parhelia-gigacolor-faa-hardware-status.md` wherever the two disagree: those
notes assign the 10-bit formats to internal ids `0x0d`/`0x0e` and call the
layout X2R10G10B10 with no alpha.  Both are wrong; see §1.

Tags: **PROVEN** is read from a disassembly, a byte table or a hardware
readback; **MEASURED** is a hardware readback by
`parhelia-drm-test --gigacolor-*`; **INFERRED** is a constrained reading that
has not been executed.  Addresses in `mtx_parhelia.o`, `mtx_drv.so` and
`MTXPARD.dll`/`mtxpserd.dll` are as `objdump` prints them (`.text` offsets for
the ELF objects, RVAs for the two PE DLLs).

## 1. The format code: one code, `0x34`, for every usage

PROVEN.  The vendor's format-record table is built by `mtx1646` at
`mtx_parhelia.o .text+0x7c050` (224 rows; each row is a call with
`esi` = internal format id, `edx` = usage, `ecx` = hardware surface code).  The
ten-bit rows are:

| address | internal | usage | code |
|---|---|---|---|
| `.text+0x7d22b` | `0x0f` | 2 (sampler) | **`0x34`** |
| `.text+0x7d27e` | `0x0f` | 3 (sampler) | **`0x34`** |
| `.text+0x7d2ce` | `0x0f` | 1 (render target) | **`0x34`** |
| `.text+0x7d36b` | `0x0f` | 0 (2-D / host-aperture view) | `0x30` |
| `.text+0x7d3bb` | `0x0f` | 7 (colour view) | `0x34` |
| `.text+0x7d45e` | `0x10` | 2 (sampler) | **`0x34`** |
| `.text+0x7d4b1` | `0x10` | 3 (sampler) | **`0x34`** |
| `.text+0x7d501` | `0x10` | 1 (render target) | **`0x34`** |
| `.text+0x7d59e` | `0x10` | 0 | `0x30` |
| `.text+0x7d5ee` | `0x10` | 7 | `0x34` |

**The sampler code and the render-target code are the same, `0x34`.**  There is
no `0x34`/`0x35` split the way 4:4:4:4 splits into `0x24`/`0x25` and 1:5:5:5
into `0x26`/`0x27`; those pairs exist only for the X-variants that force alpha
to one, and there is no alpha-less ten-bit internal id.  `0x35` is not a code at
all: the bpp classifier `mtx969` at `.text+0x5b2e0` returns zero for it.

`mtx969` at `.text+0x5b3b1` compares `0x34000000` and jumps to `0x5b3e2`, which
returns `0x20` — **code `0x34` is 32 bits per pixel**, in the same group as
`0x32` and `0x36` (PROVEN).  The host-aperture ("2-D view") companion is `0x30`,
the generic 32 bpp view code shared with `0x0b`..`0x0e`.

The Windows static initialiser (`MTXPARD.dll 0x14f10` writing 247 rows to
`.data 0x1de5d0`, count `0xf7` at `0x1e3710`; `mtxpserd.dll 0x14f20` →
`0x1e05d0`, count at `0x1e5710`) carries the same rows at row indices 59..72,
plus a Windows-only third ten-bit internal id `0x2f` also coded `0x34`.  Note
the Windows table has **no** usage-1 row for `0x0f`; Windows reaches a ten-bit
render target only through `0x10`/`0x2f`, while the Linux kernel table has both.
The `.data` image in the DLL files is all zeros — the table is built at run time
by that initialiser, so a byte dump of `.data` is not a cross-check.

### 1.1 The channel layout has two alpha bits

PROVEN.  The channel-mask switch `mtx3404` at `.text+0x79c60` dispatches on
`internal_id - 6` through the jump table `.rodata+0x2600` (33 entries, resolved
through `.rela.rodata`), and writes R at `+0x60`, G at `+0x64`, B at `+0x68` and
A at `+0x6c` of its output record.  Index 9 (internal `0x0f`) reaches
`.text+0x7a421` and index 10 (internal `0x10`) reaches `.text+0x7a3d0`:

```
7a3d0: movl $0xc0000000,0x6c(%rdi)   ; A   internal 0x10
7a3d7: movl $0x3ff00000,0x60(%rdi)   ; R
7a3de: movl $0x000ffc00,0x64(%rdi)   ; G
7a3e5: movl $0x000003ff,0x68(%rdi)   ; B   => A2R10G10B10

7a421: movl $0xc0000000,0x6c(%rdi)   ; A   internal 0x0f
7a428: movl $0x000003ff,0x60(%rdi)   ; R
7a42f: movl $0x000ffc00,0x64(%rdi)   ; G
7a436: movl $0x3ff00000,0x68(%rdi)   ; B   => A2B10G10R10
```

So the two internal ids are the R/B pair, both on hardware code `0x34`, and the
alpha mask is `0xc0000000`:

```
31       30 29                  20 19                  10 9                   0
+-----------+----------------------+----------------------+---------------------+
|    A2     |         R10          |         G10          |        B10          |
+-----------+----------------------+----------------------+---------------------+
```

That is **A2R10G10B10, not X2R10G10B10**.  The alpha-less siblings elsewhere in
the same switch are explicit about it — internal `0x0c` (`0x7a4cc`) and `0x0e`
(`0x7a442`) write `0x6c = 0` — and neither the kernel nor either DLL has such a
case for ten bits.  Whether the sampler and the blender actually deliver those
two bits on this silicon is a separate question; see §3.4.

Since one hardware code carries both channel orders, the order is **not**
selected by the descriptor.  The vendor swaps the content instead: the row field
at `r8d` / DLL `+0x10` ("swap R/B") is 1 for `0x0d`, `0x0e`, `0x0f` and 0 for
`0x0b`, `0x0c`, `0x10`, `0x2f` — exactly the BGR-ordered formats — and the
packer at `MTXPARD 0x842f8` conditionally exchanges `[29:20]` with `[9:0]`.
Section 3 measures which order the hardware itself reads.

### 1.2 Internal ids 0x0d..0x10, corrected

PROVEN, from five independent tables (`mtx3404` masks, `MTXPARD 0x84aa0` masks,
the format-class table `MTXPARD 0x837a0`, the swap-R/B row field and the two
border-colour packers):

| internal | format |
|---|---|
| `0x0b` | A8R8G8B8 |
| `0x0c` | X8R8G8B8 |
| `0x0d` | **A8B8G8R8** |
| `0x0e` | **X8B8G8R8** |
| `0x0f` | **A2B10G10R10** |
| `0x10` | **A2R10G10B10** |
| `0x2f` | A2R10G10B10, Windows only |

The earlier note's "`D3DFMT_A2B10G10R10` (31) → internal `0x0d`" came from the
D3DFORMAT decoder at `MTXPARD 0x83af0` (byte table `0x83cac`, jump table
`0x83c48`), whose entries for inputs 31/32 do return `0x0d`/`0x0e` while inputs
40..43 return the ten-bit ids.  That decoder disagrees with every other table in
both binaries, so it is not the authority on what an internal id means; it is
either an off-by-one in the vendor table or a Matrox-private extension of the
D3DFORMAT range.  Recorded as an unresolved anomaly, not resolved by guessing.

Consequence: **do not** use code `0x36` for a ten-bit colour format.  `0x36` is
S8Z24 and is in use as the working depth target.

### 1.3 What else the format directs

* **Storage order `0x11e0[18:16]`: 2, exactly like any other 32 bpp surface.**
  PROVEN.  The field is dispatched at `MTXPARD 0x31b60`
  (`mov dl,[eax+0x6]; and edx,0x7; jmp [edx*4+0x31ba0]`): order 0 → `0x300e0`,
  order 1 → unsupported (`ret`), orders 2 and 3 → `0x304a0`, order 4 →
  `0x31080`, order 5 → `0x30a80`.  The value is chosen at `MTXPARD 0xc03b0`,
  and `0x304a0`'s only format dependence is the bits per pixel from
  `0xa3b40`/`mtx969` (`0x30644` 32 bpp → a 4096-byte granule, `0x30658` 16 bpp →
  2048, `0x3066c` 8 bpp → 1024).  There is no ten-bit case anywhere in
  `0x300e0..0x31c00` or `0xc03b0..0xc0700`.
* **FCOL takes the plain packed word.** PROVEN.  The solid-rect fill
  `MTXPARD 0x110420` indexes byte table `0x1109b0` by `bpp - 8` into jump table
  `0x1109a0`; 8 bpp replicates four times (`0x11051b`), 16 bpp builds `c|c<<16`
  (`0x110537`), and **24/32 bpp take the 32-bit word verbatim**
  (`0x11054c: mov eax,[esi+0x14]`).
* **PLNWT takes the plain 32-bit mask.** PROVEN.  The replicate helper
  `MTXPARD 0x9b320` (Linux `mtx3347 .text+0xffc0`) switches on bytes per pixel:
  1 → `m * 0x01010101`, 2 → `(m & 0xffff) | m << 16`, and **4 → verbatim**
  (`0x9b346`, Linux `0x1001a: mov %ebp,%ebx`).  So a ten-bit channel mask is
  the packed field mask itself, `A 0xc0000000 | R 0x3ff00000 | G 0x000ffc00 |
  B 0x000003ff`.
* **The clear-colour packer scales 0..255 by 1023/255.** PROVEN.  The colour
  packer `mtxpserd 0x9b900` special-cases only 5:6:5 and the 1:5:5:5 pair; every
  other internal id including `0x0f`/`0x10` falls into the generic
  mask-and-shift arm at `0x9bada`, which computes `((mask >> shift) * v) / 255
  << shift` per channel — i.e. `1023 * v / 255` for a ten-bit channel and
  `3 * a / 255` for the two alpha bits.  That is the same rule the texture
  border-colour packer uses (`MTXPARD 0x84560` arm `0x846f2`, Linux
  `mtx678 .text+0x7bc50` arm `0x7bd52`), and both packers group `0x0f`/`0x10`
  with the 5- and 6-bit formats in that arm.
* **Dither takes a third state.** PROVEN.  `mtx2480` at `.text+0x12a67` reads
  the render target's internal id and tests it against `{0x0f, 0x10, 0x1e}` —
  exactly the three formats with more than eight bits per component
  (A2B10G10R10, A2R10G10B10, L16).  Those jump to `0x137a0`, which clears
  `0x12e8` bit 23 and falls into the bit-24 clear, so **both dither bits are
  low**; every other format sets bit 23 and lets the request choose bit 24.
  Implemented in `p3d_blendctl()`.

## 2. The scanout path

PROVEN.  `DC_FORMAT` is `(code << 24) | (type << 16) | pitch_in_pixels`, and for
depth 30 the vendor writes **`0x34030000 | pitch`** — the same encoding as
`0x32030000 | pitch` for XRGB8888 and `0x28030780` for RGB565.  The builder is
`mtx1244` at `mtx_drv.so 0xac770`: `0xacc3c` masks the pitch to `[13:0]`,
`0xacc54`/`0xacc57` force type 3 unconditionally, and `0xacc73`..`0xacc7b`
merges the code into `[29:24]`.  The struct is pushed to MMIO
`0x1834 + head * 0x200` by `mtx2390` at `0xab850`.

`GetSurfFormat` = `mtx2829` at `0xac140` maps the HAL pixel-format enum to the
code; `0xc0000` (GigaColor) → `0x34` at `0xac541`.  X depth 30 maps to that enum
at `mtx48 0x3f4c0` (`0x3f50d: cmpl $0x1e,0xac(%rbx)` → `0x3f832: movl
$0xc0000,0xb0(%rbx)`).  Three further sites agree: `mtx2205 0x5baa0`
(`0x5be93`), `mtx1203 0x6f580`, and the kernel's `mtx3388 .text+0x4fff0`
(`0x5010e: or $0x34000000,%edx; 0x50119: cmp $0x1e,%esi; 0x5011e: cmovne`).
The Linux X driver's GigaColor post-draw transform
`CreateSurfFormatXform_8to10` (`mtx_drv.so 0x7fed0`) hard-codes
`DeviceBitmap::Create(pdev, 0x34)` at `0x7ffe4`.

### 2.1 The P690 is not gated out of depth 30

PROVEN.  A chip-class bitmask does exist — encoded by `mtx1470` at `0x72620`
(`0x72662: cmp $0x2539102b`, giving the P690 class bits `0x70000`) and stored to
`dev+0xc9b8` — but no depth-30 decision point consults it:

* the X depth whitelist `mtx41 0x3d5a0` is a hard immediate:
  `0x3d5c7: cmp $0x1e,%edx; ja` (reject above 30) then
  `0x3d5d7: test $0x41010100,%eax`, i.e. depths {8, 16, 24, 30}, with no device
  id in the function;
* the HAL mode validator `mtx1580 0xd3020` accepts the GigaColor enum
  unconditionally (`0xd32b6: cmp $0xc0000,%ecx; 0xd32c0: je`);
* `HALQueryBoardSupport` `mtx865 0x979f0` accepts `0x2539102b` at `0x97a22`;
* the `GigaColorFullScreen`/`GigaColorRender` registry switches (strings at
  `0x110afa`/`0x110b0e`, registered at `0x62340`/`0x62395`) are plain booleans
  with **no** `Validate` override, unlike the real gate
  `DsTrinityEnabled::Validate` at `0x64870`;
* the licence gate is inert: `SFAValidatePixelFormat` `mtx874 0xe65d0` is
  `(dev[0x1e4] & req) == req` and its only caller passes a requirement word that
  `mtx1290 0xd1160` has just zeroed.

The single Parhelia-512-exclusive action on the GigaColor path is at
`mtx1244 0xacd6c` (`cmpw $0x527,0x29e(%r12)`), which sets `DC_CTRL` bit 9 — and
bit 9 is the **primary-LUT bypass** (`HSLPARSetPrimaryLUTBypass` `mtx543
0xca0d0` sets exactly that bit at `0xca15d` and nothing else).  It is therefore
a workaround for a Parhelia-512 limitation, not a capability the P690 is denied:
on the P690 the hardware ten-bit LUT stays in the path.

There is also no bandwidth or pitch penalty: `HSLPARComputeBitRate` `mtx483
0xce630` returns 32 bits per pixel for code `0x34` through the same arm as
`0x32` (`0xce680`), and `HSLPARGetAddressAlignmentMask` `mtx664 0xacdf0` gives
`0xc0000` the same 8 KiB alignment as 32 bpp (`0xace30`), relaxed to 4 KiB for
device ids `0x2537`..`0x2539` at `0xace43`.  Width granularity is 32 pixels for
every format.

### 2.2 What does restrict it

* **TV-out forces GigaColor off.** PROVEN: inside `mtx48`'s TV-out branch,
  `0x3f619: cmpl $0xc0000,0xb0(%rax); 0x3f623: jne; 0x3f625: movl $0x20,...` —
  depth 30 plus `Option "TvOut"` silently becomes 32 bpp.
* **Display controllers 2 and 3 cannot do ten bits.** PROVEN:
  `HSLPARSetPrimaryLUT` `mtx1484 0xcc2a0` branches on the DC index at
  `0xcc322`..`0xcc328`; DC 0/1 take the `0xffc0` component mask and the LUT quad
  `0x1838`..`0x1844`, DC 2/3 take `0xff00` and `0x19b8`..`0x19c4`, which can
  never satisfy the `0xcc35b`/`0xcc367` test.  Irrelevant to a two-head P690.
* **A digital output whose wire format needs packed-pixel encoding demotes
  `0x34` to `0x32` silently.**  PROVEN lists, INFERRED mapping.  In
  `GetSurfFormat`'s `0xc0000` branch three groups of per-head digital output
  format codes force the `0x32` return at `0xac1a1`:
  group 1 (`0xac23d`) `{0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x13, 0x14, 0x16,
  0x18, 0x19, 0x1a}`, group 2 (`0xac3e0`) `{0x0d, 0x0e, 0x0f, 0x15}`, group 3
  (`0xac4c9`) `{0x10, 0x11}`.  The per-head format lives at `dev+0x3d0[head]`
  (`HSLPARSetDigitalOutputFormat 0xc5210`).  Group 1 is byte-identical to the
  list that halves the computed bandwidth in `HSLPARGetModeBandwidth` and to the
  packed-pixel output-type list in `HSLPARXFormLUT 0xcbd80`, so these are the
  dual-link / packed-pixel / serial-TMDS classes where one 24-bit wire word
  carries two ten-bit samples and a ten-bit surface is impossible.  An analog
  CRT has no digital output format at all, so `dev+0x3d0[head]` is 0, which is
  in none of the lists — INFERRED that an ordinary single-link output keeps
  `0x34`.  This is the one place where ten-bit can become eight-bit without an
  error, so a connector that shows eight-bit banding at depth 30 should be
  checked here first.

### 2.3 A defect this uncovered in our own KMS driver

`PARHELIA_DC_CTRL_ON` is `0x00040201` (`parhelia_drv.h`), which sets bit 9.
Bit 9 is the primary-LUT bypass (§2.1), so `parhelia_pipe_enable()` brings the
CRTC up with the display LUT out of the path **at every depth**, and
`parhelia_pipe_load_gamma()` is writing a LUT nothing reads.  The vendor builds
`DC_CTRL` from zero (`0xacbbc`) and only ever ORs bit 9 in, for the
Parhelia-512 workaround or when a mode flag asks for it.  Candidate fix
`0x00040001`.  Not changed here: it is outside this work's files and needs its
own verification on the scanout.  Recorded so it is not lost.

## 3. What the hardware does

**Not yet measured, except for the wedge in §5.**  Three probes are implemented
and on the target, but the hardware runs are not done: the first attempt wedged
the engine (§5), the recovery reboot left the machine off the network until a
physical power cycle, and the target was then taken for a demonstration run.
Nothing in this section may be quoted as a hardware result until it is filled
in, and the driver's capabilities are set accordingly (§4).

The probes are `parhelia-drm-test --gigacolor-texture-probe`,
`--gigacolor-target-probe` and `--gigacolor-scanout-probe`, all against
`/dev/dri/card0`.  They drive format `0x34` through the ordinary state builder
and then rewrite the surface-configuration, plane-mask and blend words in the
finished batch, because the shared batch validator accepts only the two colour
codes the driver drives today.  The rewrite decodes the register-list framing
rather than searching for a value, and every phase reports how many words it
rewrote, so a substitution that missed shows up as a failure instead of passing
silently.

### 3.1 Sampling — `--gigacolor-texture-probe`

Every dword of the texture BO holds its own index, packed as three eight-bit
bytes widened to ten bits by repeating the top two into the bottom pair.  That
widening makes the recovered index independent of how the fragment pipeline
narrows a ten-bit texel: a truncating and an exact 10 → 8 conversion both return
the original byte.  The image is sampled 1:1 with nearest filtering into the
proven `0x32` target, and each pixel's three bytes name the dword the address
unit fetched.  The fetched addresses are then matched against the candidate
storage orders (linear, the generic 32x16 block, the chip's native 64x16
texture order with its low-two-y-bit exchange, and the render-target order) and
against both channel assignments, so the probe reports the address model **and**
the channel order it actually found rather than assuming either.  Code `0x32`
runs first as the control.  A second phase uploads four flat images, each with
one field saturated, which names the output channel every stored field reaches
and whether the two alpha bits are delivered at all.

Expected: order 2, the same native 64x16 order as any other 32 bpp texture (§1.3
proves the vendor has no ten-bit case in its granule arithmetic).  Unverified.

### 3.2 The render target — `--gigacolor-target-probe`

Seven phases: the 2-D fill's FCOL and PLNWT at ten-bit field boundaries; the
storage order, from 128x32 single-pixel fills each carrying its own coordinates
in the R and G fields; full-quad red, green, blue and white 3-D draws compared
against the packed A2R10G10B10 words; the precision test (§3.2.1); the 3-D
colour write mask through the red and then the green plane; blending; and a
ten-bit colour target with the ordinary Z24S8 depth buffer.

The blend phase is the one that distinguishes real ten-bit arithmetic from an
eight-bit blender writing into a ten-bit word.  The destination is filled with
R = 0x201, G = 0x105, B = 0x083 — each has a set bit below the top eight, so
none survives an eight-bit round trip — and an identity blend (src `ZERO`,
dst `ONE`) makes the blender read, blend and write the destination back.  If it
comes back unchanged the read-modify-write path is ten bits wide.  A second case
adds 0.25 over a destination of 256/1023 with `ONE`/`ONE`.

#### 3.2.1 Is the extra precision real?

The same interpolated gradient is drawn into a ten-bit and an eight-bit target
of identical geometry and the two are compared code by code.  The ramp spans
only the bottom sixteenth of the range, so an eight-bit pipeline can produce at
most 17 distinct codes across a 256-pixel row where a ten-bit one can produce
65.  That fits the discriminator inside one 256-pixel-wide surface, which
matters because three large native targets in one DRM file wedge the engine on
an unrelated fill (an allocator defect,
`parhelia-p690-multi-primitive-packet-hang.md` §8).  The test passes only if the
ten-bit target resolves strictly more codes than the eight-bit one over the same
ramp and the row is monotonic.

Note what this does and does not settle.  It measures the whole chain —
interpolator, combiner, output packer — because only the target format differs
between the two passes.  It does not tell us where a shortfall would be if the
count came out at 17.

### 3.3 Scanout — `--gigacolor-scanout-probe`

3-D draws eight ten-bit colour bars into a native scanout object, verifies the
stored words through the kernel transfer, publishes the object with
`ADDFB2 DRM_FORMAT_XRGB2101010` and modesets onto it.  `PROBE_HOLD=<seconds>`
keeps the CRTC on it so `--scanout-dump` and the monitor can be read — which is
how this must be checked, never from logs.

### 3.4 Alpha

The vendor descriptor carries two real alpha bits (§1.1), but this texture unit
delivers no sampled alpha for **any** format on the P690: a texel with A = 0
still paints RGB, on 32-bit `0x32` and on 16-bit `0x26` alike
(`parhelia-p690-sampler-alpha-emission-re.md`).  So the two bits are expected to
be storable and maskable but not sampleable, exactly as for the other formats.
The `A[31:30] = 3` case of the channel-identity phase measures it.

## 4. Driver state

* `p3d_surface_format_bytes()` accepts `0x34` as a four-byte colour code, so the
  2-D fill and the transfer path take it.
* `p3d_colour_mask()` builds the 2:10:10:10 field mask for a `0x34` target.
* `p3d_blendctl()` clears both `0x12e8` dither bits for a `0x34` target,
  reproducing `mtx2480`'s gate.
* `ph_sampler_format()` maps `PIPE_FORMAT_B10G10R10A2_UNORM` to code `0x34`,
  four bytes per texel.  That is the Gallium format whose channels, LSB first,
  are B10 G10 R10 A2 — i.e. the hardware's own word.
* `ph_target_format()` maps the same format to surface code `0x34`.
* `ph_format_supported()` advertises `PIPE_FORMAT_B10G10R10A2_UNORM` for
  sampling only, and **off by default**: `ph_gigacolor_sampler()` requires
  `PARHELIA_GIGACOLOR=1` until §3.1 is measured, because the address unit's
  acceptance of code `0x34` has not been read back from this silicon and a
  descriptor it rejects stops the engine rather than failing the draw (§5).
  It now also **refuses** `R10G10B10A2`,
  `R10G10B10X2`, `A2R10G10B10` and `A2B10G10R10`.  Those are four bytes wide, so
  the catch-all arm used to accept them and both the sampler and the colour
  output then read the word as 8:8:8:8 and delivered the wrong channels;
  refusing them makes the state tracker pick `B10G10R10A2_UNORM` for
  `GL_RGB10_A2` and `GL_RGB10`.
* The kernel scanout path accepts `DRM_FORMAT_XRGB2101010` in the plane format
  list and in `parhelia_fb_create()`, and `parhelia_dc_surface_code()` programs
  `DC_FORMAT` code `0x34` for it, which §2 proves is the vendor's own encoding.
  No allocation change was needed: a ten-bit pixel is four bytes, which is
  `CREATE_RENDER`'s default `cpp`.  A KMS format is not a GL capability, so this
  is exposed on the strength of the static evidence; §3.3 still has to confirm
  the silicon scans it out.
* A ten-bit **render target** is not advertised, and cannot be from this work's
  files: `parhelia_batch.c`'s state validator checks the colour surface code
  against the two formats the driver drives, and that file belongs to another
  work item.  Opening it is a one-line change, but the render-target cap should
  wait on §3.2 in any case.
* A GL-level test is in `tools/parhelia-gl-10bit-test.c`: a `GL_RGB10_A2`
  renderbuffer in an FBO (GLX offers no ten-bit visual on this driver) plus a
  `GL_RGB10_A2` texture, read back with
  `GL_UNSIGNED_INT_2_10_10_10_REV`, checking the clear, a draw, the depth test,
  a colour whose codes no eight-bit channel can hold, the gradient of §3.2.1,
  the colour mask, and a 1:1-sampled ten-bit texture.  It exits 77 (skip) when
  the driver does not offer a ten-bit renderbuffer, which is what it does today
  without `PARHELIA_GIGACOLOR=1`.

## 5. What does not work, and what is untested

* **Nothing in §3 has been measured.** The probes exist and run; the hardware
  results do not.  Until they do, the only ten-bit claim this driver makes to
  applications is none: the Gallium sampler cap needs `PARHELIA_GIGACOLOR=1`,
  and no ten-bit render target is offered at all.
* **The one default-path behaviour change still needs a regression run.**
  Refusing `R10G10B10A2`, `R10G10B10X2`, `A2R10G10B10` and `A2B10G10R10` (§4)
  means `GL_RGB10_A2` and `GL_RGB10` now resolve to eight-bit storage instead of
  to a ten-bit-labelled buffer whose contents the hardware read as 8:8:8:8.
  That is a correctness fix and GL lets the implementation pick the internal
  format, but the X server, glamor and the GL regression suite have not been run
  against it yet.  Nothing in `glxgears`, `ioquake3` or the existing GL tests
  asks for those formats, so the expected result is no change at all.
* **Storage order 0 (linear) with code `0x34` wedges the engine.** MEASURED: the
  first sweep pass, `--gigacolor-texture-probe` with `texelcode = 0`, never
  retired — `asynchronous submit fence 00000048 made no progress for 2002 ms
  (done 00000040); wedged`, `BM=03000000 prim=35cc0230/35cc0237` — and a reboot
  was needed.  A descriptor the address unit rejects does not fail the draw, it
  stops the engine.  The probe therefore defaults to the vendor's order 2 alone
  and `PROBE_TEXELCODES` sweeps the rest one run at a time.
* The 10-bit **gamma LUT** path is not implemented.  A correct ten-bit ramp needs
  the interval-encoded entries `Program10BitPrimaryLUT` builds
  (`mtx_drv.so 0xcb800`: each of 256 physical entries is `base10 << 6 | slope3
  << 16` and the hardware interpolates `base + slope * k`, k = 0..3, so 256
  entries cover 1024 codes) plus `DC_CTRL` bit 10 and the six compensation
  registers at `DC+0x0c`..`DC+0x20`.  A plain 256-entry ramp masked `0xffc0`
  still works and is what this driver already writes, so this is a
  gamma-accuracy item, not a prerequisite for ten-bit pixels.  See also §2.3.
* There is **no DAC-side ten-bit enable**.  PROVEN (negative):
  `HSLDACWriteRegister` `0x9eb30` is only reachable from legacy VGA/DAC bring-up
  (`0x9ee00`..`0xa1100`, DAC indices `0x19`, `0x1d`, `0x1e`) and never from a LUT
  or format path.  The DACs are ten-bit unconditionally.
* `A2B10G10R10` (internal `0x0f`, the R/B-swapped order) is not exposed.  It
  shares hardware code `0x34`, so the descriptor cannot select it; the vendor
  swaps the content in software.  Gallium can already express the swap through
  its own conversion, so there is nothing to gain by adding it.
* FAA is untouched by this work; `parhelia-gigacolor-faa-hardware-status.md` §2
  still stands.
