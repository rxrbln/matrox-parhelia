# Rendering into a render target we allocate

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Clean-room reverse engineering. Evidence is (a) decoded GPU command streams
captured from hardware into `captures/`, and (b) `objdump`/`nm` on the vendor
binaries. No proprietary source was read.

This document answers one question: **what exactly do we have to emit so the 3D
engine rasterises into a buffer WE choose?** It supersedes the parts of
`docs/parhelia-surface-address-translation.md` §3 that said the binding is
kernel-only and invisible to a client — that is refuted below.

---

## 0. Answers at a glance

1. The 3D colour/depth binding is **one ordinary opcode-0 register-write packet**,
   six dwords, and it lives **in a called section of the command stream, not in
   some kernel-private channel**. We can emit it ourselves, verbatim. [PROVEN]

   ```
   00000004  8d8c8988  <colour_base> <colour_cfg> <depth_base> <depth_cfg>
   ```

2. It is emitted **once per frame**, at the tail of the big per-frame 3D state
   section, immediately before the plane write mask, the DWGSYNC fence and the
   3D `DWGCTL`. The draw packet (`0x97080001`) is in a **later, separate**
   section. [PROVEN]

3. **Colour = slot 0 (`0x1220`/`0x1224`), depth = slot 2 (`0x1230`/`0x1234`).**
   Confirmed against 40 hardware captures and by `mtx2954`, which never emits
   slot 3 at all. Our tooling patches slot 3 by default — bind slot 0 (and see
   item 7 and §7). [PROVEN]

4. `cfg = width_px | (type << 16) | (fmt << 24)`. The low field is a **width in
   pixels, 32-aligned — not a byte pitch**; the hardware derives the stride from
   width, type and format. The vendor's 3D colour cfg is **`0x32020780`** =
   width 1920, type 2, format 0x32 (A8R8G8B8). Bits `[31:30]` are **never
   written by any code path in either vendor binary** — every writer explicitly
   preserves them — so the "tiling bits [31:30]" hypothesis is dead. [PROVEN]

5. **On a linear 3D target: the tiling question itself is now in doubt, and a
   guaranteed linear read-back exists regardless.** The vendor flips one live
   surface — the screen pixmap included — between cfg **type 3 for 2D** and cfg
   **type 2 for 3D** with *no data conversion*
   (`SurfDesc2dList::AssignCurrentSurfDescTo`), and types 2 and 3 are
   byte-for-byte identical throughout the allocator. So the 3D destination is not
   laid out differently from the 2D destination, and the measured 64x32 swizzle
   is either a real physical layout that both engines share, or an artefact of
   the base the measurement assumed (§5.4). Either way, kernel `mtx3388` BITBLTs
   any surface into a **type-1, arbitrary-pitch, linear** buffer with
   `DWGCTL = 0x2c2` in one packet — that is the read-back path (§5.5, §6 step 6).
   Bits `[31:30]` are never written by any code in either binary. [PROVEN]

6. Per-target state that must be re-emitted with the binding: the **scissor**
   `0x1328`/`0x132c` — and our tooling writes it **inverted**, which alone is
   enough to rasterise nothing (§7) — and the **viewport constants**
   `c[0x160]`/`c[0x161]`.

7. DWGCTL `[11:10]` is the **destination slot** and `[9:8]` the source slot
   (`SurfDesc2d::Init` stores `slot << 10`), obeyed by every 2D site. The 3D
   DWGCTL `0x00a60ec4` nominally says slot 3 while `mtx2954` binds slot 0 — that
   contradiction is unresolved, so **bind the target into slots 0, 1 and 3** for
   the first experiment and the question goes away (§3.0).

8. The real clear is `DeviceBitmap::ClearSurface`: `DWGCTL = 0x0cc0`, colour in
   `0x1314`, origin in `0x1308`, extent `(h<<16)|w` in `0x138c` — which is our
   own working `--dmafill` instrument with the destination slot changed (§4).

---

## 1. The binding packet, exactly  [PROVEN]

### 1.1 Framing

Opcode-0 (register-write list) framing, as everywhere else:

```
  hdr   = (opcode << 24) | (count << 17) | ((totalDwords - 2) & 0xffff)
```

For this packet the vendor emits `0x00000004`: opcode 0, count field 0,
`totalDwords - 2 = 4`, so the packet is 6 dwords long.

The next dword is a **tag** packing four register indices, LSB first;
`register = 0x1000 + index * 4`. Tag `0x8d8c8988` decodes as:

| lane | index | register | meaning |
|---|---|---|---|
| byte 0 | `0x88` | `0x1220` | colour surface base (slot 0) |
| byte 1 | `0x89` | `0x1224` | colour surface cfg (slot 0) |
| byte 2 | `0x8c` | `0x1230` | depth surface base (slot 2) |
| byte 3 | `0x8d` | `0x1234` | depth surface cfg (slot 2) |

Verified: `0x88 = 0x1220`, `0x89 = 0x1224`, `0x8c = 0x1230`, `0x8d = 0x1234`.
Four data dwords follow in that order. No pads, no terminator — all four lanes
are live, so the tag is fully consumed.

### 1.2 The packet as captured

`captures/gl-matrix/base-secs.txt`, section `1950d5f6..1950dc37` (400 dwords),
dword offset 341 — decoded:

```
  [341] hdr 00000004  1220=02000000  1224=32020780  1230=00000000  1234=32020000
  [347] hdr 00000002  1228=01000000  122c=36020780
  [351] hdr 00000001  1324=ffffffff
  [354] hdr 00000004  1300=00000707  12bc=00063291  12bc=00063290  12b0=3fff8842
  [360] hdr 00000001  1300=00a60ec4
  [363] hdr 00000001  12f4=42000000
  [366] hdr 00000002  121c=00000000  1374=00000000
  [370] hdr 00000004  1200=0001000f  1204=00000000  1208=00000000  120c=00000000
  [376] hdr 00000009  1264=00000200 1260=00000200 126c=01400400 1268=01400400 ...
```

Raw dwords, exactly as they sit in GPU memory:

```
  00000004 8d8c8988 02000000 32020780 00000000 32020000
  00000002 21218b8a 01000000 36020780
  00000001 212121c9 ffffffff
```

### 1.2b The emitter in the binary  [PROVEN]

`mtx2954` in `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`,
`.text+0xf8e0`, disassembled:

```
    f96c:  movl $0x4,(%rcx)                 ; header 0x00000004 -> 6 dwords
    f972:  movl $0x8d8c8988,0x4(%rcx)       ; tag: 88 89 8c 8d
    f97c:  mov  0x144(%r12),%eax
    f988:  mov  %eax,0x8(%rcx)              ; -> 0x1220  base[0]
    f98b:  mov  0x154(%r12),%eax
    f993:  mov  %eax,0xc(%rcx)              ; -> 0x1224  cfg[0]
    f996:  mov  0x14c(%r12),%eax
    f99e:  mov  %eax,0x10(%rcx)             ; -> 0x1230  base[2]
    f9a1:  mov  0x15c(%r12),%eax
    f9a9:  mov  %eax,0x14(%rcx)             ; -> 0x1234  cfg[2]
    f9ac:  add  $0x18,%rcx                  ; 24 bytes = 6 dwords

    f9bd:  movl $0x2,(%rcx)                 ; header 0x00000002 -> 4 dwords
    f9c3:  movl $0x21218b8a,0x4(%rcx)       ; tag: 8a 8b term term
    f9ce:  mov  0x148(%r12),%eax
    f9dc:  mov  %eax,0x8(%rcx)              ; -> 0x1228  base[1]
    f9df:  mov  0x158(%r12),%eax
    f9eb:  mov  %eax,0xc(%rcx)              ; -> 0x122c  cfg[1]
```

with `base[i] = ctx[0x144 + 4i]` and `cfg[i] = ctx[0x154 + 4i]`, snapshotted from
the surface object by `mtx2372` (`.text+0xcc30`, a 4-iteration loop over
`surf[0xb28 + 4i]` / `surf[0xb38 + 4i]` / `surf[0xb48 + 4i]`). **`0x21218f8e`
(slot 3) does not occur anywhere in `mtx2954`** — the 3D path never binds slot 3.

`surf[0xb48]` ("dims", `[13:0]` height, `[27:16]` slice count, per
`Surface2d::ModifySurfDim` `mtx_drv.so` `.text+0x7c290`) is copied into
`ctx[0x164 + 4i]` but **never reaches a register** — it is read only by
`mtx641` (`.text+0xc6d0`) as software state. So there is no surface-height
register to program; the extent comes from the cfg width plus the scissor.

The buffer is `ctx->[0xe0]->[0x68]`, write cursor at `+0x60`, limit at `+0x70`,
and the post (`mtx1937` `.text+0x32360`) appends
`20292820 00000000 <start|type> <end|type> 00000000` to the primary ring at
`buf->[0x80]` and then writes the new ring end to MMIO `0x408` — exactly the
call-and-doorbell shape our own ring already uses. (`mtx1937` also documents an
engine status register we had not seen: poll `0x318`, or `0x31c` on parts where
`[dev+0x2440] & 0x70000`, until `(val & 0x57) == 0x50`.)

Our own `dma_build_hdr()` in `tools/parhelia-3dtri.c` already emits exactly this
framing — `header = register count`, one tag, then the data — so four
`warp_add(..., 0x1220, base)` / `0x1224` / `0x1230` / `0x1234` calls in that order
reproduce the vendor's packet dword for dword.

`0x21218b8a` is the slot-1 packet: indices `0x8a`, `0x8b` = `0x1228`/`0x122c`,
then a `0x21` terminator in lane 2 (lane 3's `0x21` is filler — the terminator
already ended the tag). Slot 1 is the drawable's *other* GLX buffer (used by
the swap blit and as a 2D blit source); the 3D draw does not read it.

### 1.3 Where in the frame

Section order in one captured frame (`base-secs.txt`, consecutive calls):

```
  SEC 1950d5f6 (400 dw)  the whole per-frame 3D state block
                         ... [341] the binding packet ... [360] DWGCTL 0x00a60ec4
  SEC 1950dc36 (4 dw)    00000002 21211000 00000003 00000004   (0x1000=3, 0x1040=4)
                         -> immediately followed in memory by the vertex dwords
  SEC 1950dc82 (3 dw)    97080001 1950dc44 1950dc80              <- THE DRAW
  SEC 1950dc9e (6 dw)    00000004 c0afafc0 00000307 ... 00a60ec4  (post-draw sync)
```

So: **bind, then DWGCTL, then a separate draw packet.** The binding is per frame,
not per draw — a second draw in the same frame reuses it.

### 1.4 It is NOT kernel-private  [CORRECTION]

`docs/parhelia-surface-address-translation.md` §3.2 stated the binding "is
emitted by the kernel into the primary stream, never by the client into its
secondary sections", and that this is why a standalone replay does not
rasterise. **That is wrong.** Scanning every captured called section across 40
GL variants finds **2 486 well-formed `0x00000004 0x8d8c8988` packets**, always in a
section the primary ring `0x20292820`-calls. Exactly two payloads exist:

| colour base | colour cfg | depth base | depth cfg | occurrences | what it is |
|---|---|---|---|---|---|
| `0x00000000` | `0x32020000` | `0x00000000` | `0x32020000` | 1678 | the null/reset binding (width 0) |
| `0x02000000` | `0x32020780` | `0x00000000` | `0x32020000` | 808 | **the live 3D colour target** |

(A further ~19 "hits" are the dword `0x8d8c8988` landing at a section boundary
where the following dwords belong to the next packet; they are excluded.)

Every one of the 2 541 sections that contains the binding is called with the
START tag's low two bits = **2**, i.e. the *header-ful* framing our replay
already reproduces (`same-boot-replay.sh`: "the call's START low bits select the
secondary's framing: |2 = header-ful, |0 = header-less bare tags"). Nothing about
this packet needs a different call shape.

Whether the *vendor's* copy is produced by its kernel module or its ICD is
irrelevant to us: the bits reach the engine as an ordinary register-write packet
in a callable section, which is precisely the shape our own DMA ring already
emits. Nothing about the binding is privileged.

### 1.5 Proof that `0x02000000 / 0x32020780` really is the render target

Three independent measurements agree, per capture variant:

```
variant   window   0x132c (scissor max)   c[0x160] (viewport scale)
base      304x240  0x00f00130 = 240,304   ( 152, -120, 0.5, 1.0)   W/2=152 H/2=120
blend     352x240  0x00f00160 = 240,352   ( 176, -120, 0.5, 1.0)
scissor   360x240  0x00f00168 = 240,360   ( 180, -120, 0.5, 1.0)
big       640x480  0x01e00280 = 480,640   ( 320, -240, 0.5, 1.0)
```

`tools/gltest.c` creates its window at `(0,0)` of the root with
`GLX_DOUBLEBUFFER`, and `c[0x161] = (W/2, H/2, 0.5, H)` gives viewport origin
`(0,0)` in every case. The colour binding is `0x02000000` with width 1920 in all
of them — i.e. the shared, screen-pitch GL back buffer, with the window's
content in its top-left corner. The colour target follows the drawable; the
scissor and viewport follow the window. Nothing else in the stream carries a
target address.

### 1.6 The base register is the framebuffer-BAR offset, 1:1  [PROVEN]

Worth stating because a captured base and a measured footprint appear to
disagree. Our own 2D fill instrument writes `0x1220 = X` and then finds its
120000 green pixels at **BAR offset X**, re-measured on a clean engine at
`0x400000`, `0x1000000`, `0x4000000`, `0x6000000`, `0x7000000`, `0x8400000` and
`0xc000000` — every one draws the full rectangle
(`tools/parhelia-3dtri.c:432`, comment). There is no aperture translation, no
window base, no low limit: the value in `0x1220` is the byte offset we read back
through the BAR.

The apparent disagreement is a capture artefact, not a translation.
`captures/live-ring-0528/secs2.txt` binds `0x1220 = 0x02000000` in a section
whose scissor is `0x00f00140` = 320x240 and whose viewport constants are
`c[0x160] = (160, -120, 0.5, 1)`, `c[0x161] = (160, 120, 0.5, 240)` — i.e. a
`gltri` frame with viewport origin `(0,0)` — while the VRAM watch from the same
session found the triangle at BAR `0x2fa1020..0x3095f7f`
(`captures/live-ring-0528/rt-scan.txt`). Under the §5 tiling that footprint
decodes to `(0,18)..(175,159)` **only** for a base near `0x02fa0000`; for
`0x02000000` it would decode to x = 1280..1519, impossible for a 320-wide
viewport at origin 0. The capture script runs `gltri` eight times and dedups the
call list with `sort -u`, so the section set spans eight drawables; the captured
binding is simply from an earlier run whose buffer was freed and reallocated.

The practical rule: **a base in a captured stream is valid only for that
drawable on that boot.** Never replay one — substitute your own. That is the
whole point of this document.

The depth slot is `base 0, cfg 0x32020000` (width 0) in **every** capture,
including the `depth`, `depthfunc` and `depthmask` variants: a null depth
binding is accepted by the engine, and a 3D draw does not require a Z surface.

---

## 2. The cfg word  [PROVEN]

Full field map, from `Surface2d::ModifySurfMode(uint w, uint type, uint fmt,
uint sub)` in `mtx_drv.so` (`mtx740` `.text+0x7c230`), the canonical setter — the
masks are explicit in the disassembly:

```
   7c238:  and  $0x3fff,%si          ; w    -> [13:0]
   7c244:  and  $0x7,%edx            ; type -> [18:16]
   7c24b:  and  $0x3f,%ecx           ; fmt  -> [29:24]
   7c24e:  shl  $0x3,%r8d            ; sub  -> [20:19]
   7c252:  and  $0xc000,%r9w         ; PRESERVE [15:14]
   7c258:  and  $0xffffffc0,%eax     ; PRESERVE [31:30]
   7c263:  and  0xc6(%rdi),%sil      ; 0xf8, PRESERVE [23:21]
```

```
 31 30 29        24 23  21 20 19 18   16 15 14 13          0
 +-----+-----------+------+-----+-------+-----+-------------+
 | never|  format  |swap  | sub | type  |never| width in px |
 | written        |[23]   |     |       |written           |
 +-----+-----------+------+-----+-------+-----+-------------+
```

`[13:0]` is a **width in pixels with granularity 32**, not a byte pitch
(`GetWidthGranularityREVB` `.text+0x74350` is a bare `mov $0x20,%eax; ret`, and
`DeviceBitmap::Create` rounds with `and $0x3fe0`). The byte stride is derived by
the hardware — see §5.1. Bit `[23]` is the front/back swap flag
(`Surface2d::SwapSurfMode` `.text+0x7c2d0`: `orl $0x800000` / `andl
$0xff7fffff`).

Values that actually occur in captured 3D/2D streams:

| cfg | width | type | fmt | seen as |
|---|---|---|---|---|
| `0x32020780` | 1920 | 2 | 0x32 A8R8G8B8 | **the 3D colour target** |
| `0x32020000` | 0 | 2 | 0x32 | the null/reset binding, and the null depth slot |
| `0x30030780` | 1920 | 3 | 0x30 X8R8G8B8 | the X desktop as a 2D destination |
| `0x30030020` | 32 | 3 | 0x30 | a 32-px-wide 2D scratch |
| `0x36020780` | 1920 | 2 | 0x36 | slot 1, the drawable's other buffer |

Format codes: `GetBitsPerPixel` (`mtx887` `.text+0x73e70`) gives the full
code -> bpp table — `0x00,0x02,0x04,0x05` 1 bpp;
`0x08` 4 bpp; `0x09,0x0a..0x0c,0x10,0x11,0x12` 8 bpp; `0x20..0x22,0x24..0x28,0x2a..0x2e` 16 bpp;
`0x30,0x32,0x33,0x34,0x36,0x38,0x3a,0x3b` 32 bpp. Note it masks exactly
`0x3f000000`, i.e. bits `[31:30]` are deliberately **not** part of the format
field. Matching `src/parhelia_reg.h`: `0x11` 8 bpp, `0x28` 16 bpp, `0x30`
X8R8G8B8, `0x32` A8R8G8B8, `0x34` X2R10G10B10, `0x36` A2R10G10B10.

The vendor's own explicit cfg constructor, `mtx3388` `mtx_parhelia.o`
`.text+0x4fff0` (the overlay/PutImage path), builds it exactly this way:

```
   500e0:  sub  (%rdx),%ecx           ; width = x1 - x0, in pixels
   500f2:  or   $0x10000,%ecx         ; type = 1
   505c2:  or   $0x10000000,%ecx      ; bpp  8 -> fmt 0x10
   505cd:  or   $0x20000000,%ecx      ; bpp 16 -> fmt 0x20
   5010e:  or   $0x34000000,%edx      ; bpp 30 -> fmt 0x34
   50114:  or   $0x30000000,%eax      ; else   -> fmt 0x30
   5015d:  mov  %ecx,0xb38(%rdx)      ; cfg[0]
   502ab:  or   $0x3,%eax
   502b6:  mov  %eax,0xb28(%rcx)      ; base[0] = addr | 3   (system memory)
```

`cfg = width | (type << 16) | (fmt << 24)`, bits `[31:30]` zero, and the base
carries its class tag in the low bits.

`tools/parhelia-3dtri.c:rt_ctrl()` already builds this correctly:

```c
    fmt = (bpp == 8) ? 0x11 : (bpp == 16) ? 0x28 : 0x32;
    cfg = 0x20000u | (fmt << 24) | (pitch_px & 0xffff);
```

`rt_ctrl(32, 1920) == 0x32020780` — bit-identical to the vendor. **Use it.**

### Allocation rules

From the vendor allocator (`GetWidthGranularityREVB`, `GetHeightGranularityREVB`,
`GetLocalAlignmentAndGranularityREVB` in `mtx_drv.so`):

* width granularity 32 px, **height granularity 32 px for type 2/3**;
* **local alignment 0x2000 (8 KB) for a 32 bpp type-2/3 surface**;
* `size = roundup(w,32) * roundup(h,heightGran) * bpp/8 * slices`, rounded up to
  the alignment (`GetPhysicalMemSizeREVB` `.text+0x74490`); for a 32 bpp type-2/3
  surface `heightGran` is 32.

Independently corroborated by the observed base set: `0x01000000 + 1920 *
roundup(1200,32) * 4 = 0x01000000 + 0x8E8000 = 0x018E8000`, which is one of the
bases the capture actually contains, and `0x018EA000` is exactly one 8 KB tile
past it.

For a target we allocate, additionally make the **width a multiple of 64** so the
tile-row stride `(W/64) * 8192` of §5 is exact, and so `next_pow2(w)` and
`roundup(w,32)` do not diverge more than they must.

### The base word

`base = byte_offset_in_that_space | class_tag`, `class_tag` = bits `[1:0]`:
`0` local VRAM, `2` AGP, `3` PCI/system. No shift, no page table. A VRAM target
at byte `0x04000000` is written literally as `0x04000000`.

---

## 3. Everything else that is per-target

| register / slot | value | must change with the target? |
|---|---|---|
| `0x1220`/`0x1224` | colour base / cfg | **yes** |
| `0x1230`/`0x1234` | depth base / cfg | **yes** (null = `0` / `0x32020000`) |
| `0x1328` | scissor min = `(y0 << 16) \| x0` | **yes** |
| `0x132c` | scissor max = `(y1 << 16) \| x1`, exclusive | **yes** |
| `c[0x160]` | `(W/2, -H/2, depthScale, 1.0)` | **yes** |
| `c[0x161]` | `(x0 + W/2, y0 + H/2, depthBias, H)` | **yes** |
| `0x1324` | plane write mask, `0xffffffff` | no |
| `0x12f4` | guardband, `0x42000000` (= 32.0f) | no |
| `0x12c8`,`0x12cc`,`0x12d0`,`0x12fc` | see below | **yes — they come from the surface object** |
| `0x12f8` | `0x007fe000` | no |
| `0x1228`/`0x122c` | slot 1, the other GLX buffer | only for the swap blit |
| `0x138c` | 2D `LEN` = `(h << 16) \| w` | **2D only** — see below |

### 3.0 DWGCTL `[11:10]` is the destination slot — and it disagrees with the 3D binding

`SurfDesc2d::Init` (`mtx_drv.so` `.text+0x7bc80`) is the slot map, and it pairs
each slot's packet tag with a DWGCTL bit pattern:

```
   7bca6:  movl $0x000,0x10(%rdi) ; movl $0x21218988,0x14(%rdi)   ; slot 0 -> 0x1220/0x1224
   7bcb5:  movl $0x400,0x10(%rdi) ; movl $0x21218b8a,0x14(%rdi)   ; slot 1 -> 0x1228/0x122c
   7bcc4:  movl $0x800,0x10(%rdi) ; movl $0x21218d8c,0x14(%rdi)   ; slot 2 -> 0x1230/0x1234
   7bc97:  movl $0xc00,0x10(%rdi) ; movl $0x21218f8e,0x14(%rdi)   ; slot 3 -> 0x1238/0x123c
```

`slot << 10`, i.e. **DWGCTL `[11:10]` = destination slot, `[9:8]` = source
slot**. Three independent 2D sites obey it exactly:

| site | DWGCTL | `[11:10]` | slot it binds |
|---|---|---|---|
| `mtx1192` priming draw | `0x005000f5` | 0 | `0x1220`/`0x1224` ✓ |
| `DeviceBitmap::ClearSurface` | `0x00000cc0` | 3 | `0x1238`/`0x123c` ✓ |
| `mtx3388` readback blit | `0x000002c2` | 0 dst, `[9:8]`=2 src | dst `0x1220`, src `0x1230` ✓ |
| `ParhlXAASubsequentSolidFillRect` | from `shr $0xa` of the assign result | — | matches |

**The 3D DWGCTL does not fit.** `0x00a60ec4` has `[11:10] = 0b11` — nominally
slot 3 — yet `mtx2954` binds the live target into **slot 0** and never emits
`0x21218f8e` at all, and slot 3 is `base 0 / width 0` in every capture. Those
bits come from the unconditional `or $0x860e00` in the 3D DWGCTL builder
(`mtx3712` `.text+0x18c49`), so nothing per-draw ever varies them.

The full decode of `0x00a60ec4` is `[3:0] = 4` opcode, `[7:4] = 0xC` GXcopy,
`[9:8] = 2`, `[11:10] = 3`. Note `[9:8] = 2` **is** the depth slot — which may be
coincidence from the constant, or may be how the 3D path names its Z surface.

The two readings are: (a) the 3D rasteriser ignores `[11:10]` and always uses
slot 0 for colour and slot 2 for depth — strongly supported, since `mtx2954`
*is* the 3D binder and it emits exactly that pair; or (b) it honours them and
something outside the captured sections binds slot 3. We have not separated them.

**Practical consequence for the first experiment: bind our target into slot 0,
slot 1 *and* slot 3.** It costs four extra dwords, it makes the question moot,
and it is what our `--dmafill` instrument already does for slots 0 and 1.

### 3.1 The scissor encoding  [PROVEN]

`0x1328` is the **minimum** corner and `0x132c` the **maximum**, both packed
`(y << 16) | x`. Proof, three ways:

* the per-variant correlation in §1.5: `0x132c` low half == window width,
  high half == window height, `0x1328 == 0` in every capture;
* the vendor's 1-pixel pipeline-drain rectangle is `0x1328 = 0x00000000`,
  `0x132c = 0x00010001` — a 1x1 box, which only parses one way;
* the same `(y << 16) | x` packing is used by the 2D `DSTXY 0x1308` and
  `LEN 0x138c` in our own, hardware-verified 120000-pixel fill instrument
  (`0x1308 = (150 << 16) | 200`, `0x138c = (300 << 16) | 400`);
* and, decisively, the vendor's scissor emitter `mtx2187`
  (`mtx_parhelia.o` `.text+0x3ccb0`), which `mtx2954` calls, writes the
  full-range default with the constant in `0x132c`:

  ```
     3cda3:  movl $0xb2cacbc0,-0x20(%rcx)   ; tag: c0 cb ca b2
                                            ;   -> 0x1300, 0x132c, 0x1328, 0x12c8
     3cdaa:  movl $0xc4,-0x1c(%rcx)         ; 0x1300 DWGCTL = 0xc4
     3cdbf:  movl $0xfff0fff,-0x18(%rcx)    ; 0x132c = 0x0fff0fff   scissor MAX
     3cdd0:  movl $0x0,-0x14(%rcx)          ; 0x1328 = 0            scissor MIN
     3cdd7:  mov  %r13d,-0x10(%rcx)         ; 0x12c8 <- ctx[0x138]
     3cddb:  movl $0x2121bf20,-0xc(%rcx)    ; PAD, 0x12fc, term
     3cde9:  mov  %r14d,-0x4(%rcx)          ; 0x12fc <- ctx[0x134]
  ```

  `0x0fff0fff` = 4095x4095, the maximum box — and it goes in `0x132c`.
  `tools/parhelia-3dtri.c:3090` puts that same constant in `0x1328` and zero in
  `0x132c`, i.e. exactly inverted (§7).

  The same function also emits, immediately before:

  ```
     3ccfd:  movl $0xb4b320c9,-0x14(%rcx)   ; tag: c9 20(PAD) b3 b4
                                            ;   -> 0x1324, <pad>, 0x12cc, 0x12d0
     3cd04:  movl $0xffffffff,-0x10(%rcx)   ; 0x1324 = plane mask, all planes
     3cd0b:  movl $0x0,-0xc(%rcx)           ; pad dword, consumed, writes nothing
     3cd1a:  mov  %eax,-0x8(%rcx)           ; 0x12cc <- ctx[0x13c]
     3cd16:  mov  %r12d,-0x4(%rcx)          ; 0x12d0 <- ctx[0x140]
  ```

  a worked example of the `0x20` PAD lane consuming a data dword.

### 3.2 The viewport constants  [PROVEN]

Block-file constants, uploaded with the opcode-0x41 packet:

```
  hdr    = 0x41000000 | (n << 17) | ((n*4 + 3) & 0xffff)     n = number of vec4
  [n] [first] [1] [0] then n*4 payload dwords
```

For `n = 2, first = 0x160` that is `0x4104000b`, exactly as captured (this frame
had a 364x240 window):

```
  4104000b 00000002 00000160 00000001 00000000
           43360000 c2f00000 3f000000 3f800000     c[0x160] = (182, -120, 0.5, 1.0)
           43360000 42f00000 3f000000 43700000     c[0x161] = (182,  120, 0.5, 240)
```

with, for `glViewport(x, y, w, h)` into a target of height `Ht`:

```
  x0 = x                y0 = Ht - y - h
  c[0x160] = ( w/2, -h/2, (f-n)/2, 1.0 )
  c[0x161] = ( x0 + w/2, y0 + h/2, (n+f)/2, (float)h )
```

The Y flip lives entirely in the negative `y` scale. `glDepthRange(0,1)` gives
`0.5, 0.5`, which is what the capture holds.

### 3.2b `0x12c8` / `0x12cc` / `0x12d0` / `0x12fc` are per-surface  [PROVEN]

`mtx2372` copies three scalars off the surface object alongside the four
descriptors — `surf[0x98]`, `surf[0x9c]`, `surf[0xa0]` into `ctx[0x138]`,
`ctx[0x13c]`, `ctx[0x140]` — and `mtx2187` (`.text+0x3ccb0`, called by
`mtx2954`) emits them:

```
   ctx[0x138] -> 0x12c8      ctx[0x13c] -> 0x12cc      ctx[0x140] -> 0x12d0
   ctx[0x134] -> 0x12fc
```

So these four move with the target, not with the window. Captured values in the
3D state block: `0x12c8 = 0x001000e4`, `0x12cc = 0x12d0 = 0x000000ff`,
`0x12fc = 0x0bb80024`; at context reset `0x12c8 = 0`, `0x12fc = 0x18004014`.
They are constant across all 40 variants because every variant renders into the
same drawable surface. Copy the vendor's live values, and re-check them if a
target of a different format or class is ever bound.

### 3.3 Depth

Every captured 3D draw binds the depth slot **null**: `0x1230 = 0`,
`0x1234 = 0x32020000` (width 0) — and that is despite `tools/gltest.c` asking for
`GLX_DEPTH_SIZE, 16` in its visual and the `depth`, `depthfunc` and `depthmask`
variants enabling `GL_DEPTH_TEST`. A Z surface is therefore allocated lazily, or
not at all on this path. Scanning every capture in the tree finds **no** non-null
value ever written to `0x1230`. Bind the same and leave depth test off for the
first experiments. When a real Z surface is wanted, allocate it under the same
type-2 rules and the same geometry as colour; whether it carries the identical
64x32 tiling is **OPEN** — nothing in the captures binds one.

The depth-adjacent state in the same section, for reference:
`0x12c0 = 0x80000000`, `0x12c4 = 0x7fffffff` (Z limits),
`0x12c8 = 0x001000e4`, `0x12cc = 0x12d0 = 0x000000ff`.

**The depth format code is not in either binary.** An exhaustive scan of every
byte-3 format setter and every `$0x??000000` immediate in both files yields only
colour and YUV formats — `0x04/0x05` 1 bpp, `0x10` 8 bpp, `0x20`/`0x28` 16 bpp
RGB, `0x2e` 16 bpp YUV, `0x30` 32 bpp 2D view, `0x32` 32 bpp 3D view, `0x34`
30 bpp, `0x36`, `0x3b`. Within the codes `GetBitsPerPixel` recognises but nothing
ever programs, the depth candidates are **16-bit `0x21 0x22 0x24..0x27
0x2a..0x2d`** and **32-bit `0x33 0x38 0x3a`**. The switch
`ForceDepthToS8Z24Sundog` (`mtx_drv.so .rodata 0x110c70`, kernel `0xc7ee6`) is
registered in both binaries and read by neither, alongside `EnableZCompSundog`
and `UseZCompInMultContextSundog` — so the depth-format table lives on the
libGL/Sundog client side. That is the next RE target, and it matches
`parhelia-mesa-re-targets`.

The per-context depth descriptor is `ctx[0x14c]`/`ctx[0x15c]`, and the
Z-uncompress / scene-begin pass `mtx2153` (`.text+0xd0ad`, guarded by the
multisample test `testl $0x7fc00`) writes it back into the shadow at
`+0xb30`/`+0xb40` — the same slot-2 pair.

---

## 4. The clear path

### 4.1 What the captures contain is *not* a clear

There is no colour clear anywhere in the captured spans — `tools/gltest.c` only
calls `glClear` under `GLTEST_CLEAR`, which none of the 40 variants set. What is
in the stream, 112 times, is a 1x1 rectangle:

```
=== SEC 1950ce1e..1950ce4b (11 dw) ===
  hdr 00000009  1300=005000f5  1328=00000000  132c=00010001  12c8=00000000
                12e4=00000001  12fc=00000000  1220=00000000  1224=32020000
  (tags: b2cbcac0 = 0xc0,0xca,0xcb,0xb2 ; 8988bfb9 = 0xb9,0xbf,0x88,0x89)
```

and its twin with `0x1300 = 0x001000f4`. That is `mtx1192` in
**`mtx_parhelia.o`** `.text+0x3dcb0` — note `mtx1192` in `mtx_drv.so` is a
*different* function (`GetPhysicalMemSizeREVB`); the two binaries' obfuscated
numbering spaces collide, so always qualify which file:

```
    3dcce:  mov  0xb38(%rax),%r12d         ; current slot-0 cfg
    3dcd5:  mov  0xb28(%rax),%r13d         ; current slot-0 base
    3dcee:  and  $0xc0ffffff,%r12d         ; clear the format field only
    3dcf5:  or   $0x32000000,%r12d         ;   -- TYPE IS PRESERVED
    3dd2e:  movl $0x9,-0x2c(%rcx)          ; header, 11 dwords
    3dd35:  movl $0xb2cbcac0,-0x28(%rcx)   ; -> 0x1300, 0x1328, 0x132c, 0x12c8
    3dd3c:  movl $0x5000f5,-0x24(%rcx)     ; DWGCTL = 0x005000f5
    3dd51:  movl $0x0,-0x20(%rcx)          ; 0x1328 = 0
    3dd7c:  mov  %eax,-0x1c(%rcx)          ; 0x132c = 0x00010001   (1x1)
    3dd63:  movl $0x8988bfb9,-0x14(%rcx)   ; -> 0x12e4, 0x12fc, 0x1220, 0x1224
    3dd6a:  movl $0x1,-0x10(%rcx)          ; 0x12e4 = 1
    3dd78:  mov  %r13d,-0x8(%rcx)          ; 0x1220 = base
    3dd7f:  mov  %r12d,-0x4(%rcx)          ; 0x1224 = cfg
```

followed by a second, 31-dword packet of hard-coded DDA constants (`10.0f`,
`11.0f`). Its twins are `mtx2423` (`.text+0x3dfc0`, DWGCTL `0x001000f4`,
constants `0.0f`/`0.9f`) and `mtx465` (`.text+0x3e950`). Callers:
`mtx2550`/`mtx455`/`mtx759`, all from the flush chain `mtx2644`
(`.text+0x3e390`). Grepping both binaries for `5000f5|1000f4|5000f4|1000f5`
returns only these three sites.

**These are pipeline-priming / flush dummy draws with hard-coded 1-pixel
geometry, not clears.** They are still useful as a second witness that a fill
binds slot 0, and as a third witness that cfg `[31:30]` is carried but never
authored.

### 4.2 The real clear  [PROVEN]

`DeviceBitmap::ClearSurface` (`mtx_drv.so` `.text+0x7c8f0`), a whole-surface
solid fill:

```
    7c97b:  movzwl %cx,%r12d ; shl $0x10,%edx ; or %edx,%r12d   ; (h<<16)|w
    7c99a:  movl $0xc5c08f8e,-0x20(%rdx)   ; -> 0x1238, 0x123c, 0x1300, 0x1314
    7c9a1:  mov  0xe0(%rbp),%r9d           ; 0x1238 = slot 3 base
    7c9b3:  mov  0xc4(%rbp),%edi           ; 0x123c = slot 3 cfg (type 3, verbatim)
    7c9b9:  movl $0xcc0,-0x14(%rdx)        ; 0x1300 DWGCTL = 0x00000cc0
    7c9c0:  movl $0x0,-0x10(%rdx)          ; 0x1314 = 0     <-- CLEAR COLOUR
    7c9c7:  movl $0x2121e3c2,-0xc(%rdx)    ; -> 0x1308, 0x138c
    7c9ce:  movl $0x0,-0x8(%rdx)           ; 0x1308 = 0     <-- dst origin
    7c9d8:  mov  %r12d,-0x4(%rdx)          ; 0x138c         <-- extent, FIRES
```

`DWGCTL 0x00000cc0` = opcode 0 (solid fill) | BOP `0xC` GXcopy | destination
slot 3 (`[11:10] = 0b11`, §3.0).

XAA confirms the register roles from the other side.
`ParhlXAASetupForSolidFill` (`.text+0x536e0`) stores the bpp-replicated
foreground colour and the plane mask into the shadow — `+0x2c -> 0x1314` (FCOL),
`+0x24 -> 0x1324` (plane mask) — and `ParhlXAASubsequentSolidFillRect`
(`.text+0x539c0`) emits DWGSYNC `0x707` then `0x1308 = (y<<16)|x`,
`0x138c = (h<<16)|w`.

The 2D register group, cross-checked against the kernel shadow flush
`mtx1619` (`mtx_parhelia.o` `.text+0x412c0`) and against our own working
instrument:

| reg | idx | role |
|---|---|---|
| `0x1300` | `0xc0` | DWGCTL — `[3:0]` opcode, `[7:4]` BOP, `[9:8]` src slot, `[11:10]` dst slot, bit 23 clip enable |
| `0x1304` | `0xc1` | source origin `(y<<16) \| x` |
| `0x1308` | `0xc2` | destination origin `(y<<16) \| x` |
| `0x1314` | `0xc5` | **FCOL — the fill / clear colour** |
| `0x1318` | `0xc6` | BCOL / colour-key mask |
| `0x1324` | `0xc9` | plane write mask |
| `0x138c` | `0xe3` | **extent `(h<<16) \| w` — writing it fires the op** |

`mtx1619`'s dirty-bit map, the most compact statement of the whole group:
bit0 -> `0x1300`; bit1 -> `0x1324`; bit2 -> slot 0 + slot 2 (`0x8d8c8988`);
bit3 -> slot 1; bit4 -> slot 3; bit5 -> `0x1318`,`0x1314`; bit6 -> `0x1308`,
`0x1304`, `0x138c` or `0x130c`; bit7 -> the DWGSYNC packet `0xacafafc0`
(`0x1300=0x707`, `0x12bc=tag|1`, `0x12bc=tag`, `0x12b0`).

There is **no separate colour / depth / stencil clear selector** in these
routines — the choice is the DWGCTL opcode plus which slot the destination is
bound in. The option strings `Use2DClearForceZUncompSundog`, `Use3DClearSundog`
and `ZUncompAnd2DClearSceneInProgSundog` are registered in both binaries and read
by neither, so both a 2D-engine clear and a 3D clear exist but the selection
logic is on the client side.

### 4.3 What we should use

Our own `--dmafill` instrument is exactly `ClearSurface` with the destination in
slot 0 instead of slot 3, and it is hardware-proven at 120000 pixels:

```
  0x1220 = base      0x1224 = cfg          (slot 0 = destination)
  0x1228 = base      0x122c = cfg          (slot 1)
  0x12c0 = 0xffffffff
  0x1300 = 0x000000c0                      (opcode 0, BOP GXcopy, dst slot 0)
  0x1314 = colour                          (FCOL)
  0x1304 = 0                               (SRCXY)
  0x1308 = (y << 16) | x                   (DSTXY)
  0x138c = (h << 16) | w                   (LEN -- fires)
```

`0x000000c0` and the vendor's `0x00000cc0` differ only in `[11:10]`, the
destination slot — which is the cleanest possible confirmation of §3.0.

---

## 5. Is a LINEAR 3D colour target possible?

**Revised answer: the question is probably moot, because the evidence now says
the 3D destination is not tiled relative to the 2D destination at all.** The type
field is *not* a tiling selector either. And there is, separately, a
vendor-exercised blit that lands any surface into a linear system-memory buffer
in one packet — so a linear read-back is guaranteed either way.

### 5.1 Types 2 and 3 address the same pixel at the same byte  [PROVEN]

`SurfDesc2dList::AssignCurrentSurfDescTo(DeviceBitmap*, bool is3D)`
(`mtx_drv.so` `.text+0x7ba20`) binds **one** `DeviceBitmap` as two descriptor
views of the identical pixels:

```
; is3D != 0
  7bab8:  mov  0xc4(%r12),%r13d      ; the bitmap's stored cfg
  7bac0:  mov  0x130(%r12),%edx      ; the bitmap's 3D format code
  7bacf:  orl  $0x20,0x120(%r12)     ; "currently programmed as 3D"
  7bad8:  or   $0x20000,%r13d        ; TYPE = 2
  7bae5:  and  $0xc0faffff,%r13d     ; clear format and type bits 18,16
  7baec:  or   %edx,%r13d            ; format = fmt3d
  7bb4b:  mov  0xe0(%r12),%eax       ; base -> 0x1220
  7bb53:  mov  %r13d,-0x4(%rcx)      ; cfg  -> 0x1224

; is3D == 0
  7bb95:  orl  $0x10,0x120(%r12)     ; "currently programmed as 2D"
  7bb9e:  mov  0xc4(%r12),%r13d      ; cfg verbatim -- TYPE stays 3
```

`DeviceBitmap::Create` (`.text+0x7c430`) sets the stored cfg to type 3 and the
3D format to `0x32`/`0x28`/`0x11` by bytes-per-pixel; `InitPrimarySurface`
(`.text+0x7f1a0`) does the same for the **screen**. `mtx2660` (`.text+0x800b0`)
is the "make this X pixmap the 3D destination" helper and calls
`AssignCurrentSurfDescTo(bitmap, 1)`; `ParhlXAASetupForSolidFill`
(`.text+0x536e0`) calls it with 0 to switch back.

**No pixel conversion happens on the switch.** The whole difference between the
2D and the 3D view of a live surface — the screen pixmap included — is one
re-emitted descriptor preceded by a `DWGCTL = 0x707` DWGSYNC. The vendor renders
3D into ordinary X pixmaps and then draws into them with the 2D engine, with no
reformat step anywhere.

The allocator agrees: types 2 and 3 are **byte-for-byte identical** in every
allocator function — same footprint, same alignment, same aperture pitch. Nothing
distinguishes them as a memory layout.

The captures corroborate the split exactly — over all 110 surface-descriptor
writes in `captures/live-ring-0528/decoded.txt` the correlation is 100 %:

```
0x1224=30030780  0x122c=30030780/30030020   fmt 0x30, type 3, 2D  (DWGCTL 0x20c0/0x24c0)
0x1224=32020780  0x122c=36020780            fmt 0x32/0x36, type 2, 3D (DWGCTL 0x00a60ec4)
0x1224=32020000  0x1234/0x123c=32020000     width 0 = slot unused
```

**Consequence: the measured 64x32 swizzle cannot be a 3D-versus-2D layout
difference.** If it were, the vendor's own screen would be corrupt the moment a
GL client drew into it.

### 5.2 Neither is the type field, and bits [31:30] are dead  [PROVEN]

Complete type enumeration, from the allocator (`GetWidthGranularityREVB`
`.text+0x74350`, `GetHeightGranularityREVB` `.text+0x74360`,
`GetLocalAlignmentAndGranularityREVB` `.text+0x74170`,
`GetSystemAlignmentAndGranularityREVB` `.text+0x74070`,
`GetPhysicalMemSizeREVB` `.text+0x74490`, `GetHostAperturePitch`
`.text+0x745c0`):

| type | height gran | pitch rule | aperture pitch | what the vendor uses it for |
|---|---|---|---|---|
| 0 | 1 row | raw byte size at desc `+0xc` | `roundup(w,32)*bpp/8` | opaque blob; **no usage site found** |
| 1 | 2 rows | **arbitrary pixel width, no rounding** | `roundup(w,32)*bpp/8` | genuinely linear: host-aperture staging bitmap (`TempBit::Enable` `.text+0x7e365`), and the system-memory readback destination (`mtx3388` `.text+0x500f2`) |
| 2 | 32 (16 with AA) | rounded to 32 px | `next_pow2(max(w,32))*bpp/8` | the **3D view** of a surface |
| 3 | 32 (16 with AA) | rounded to 32 px | `next_pow2(max(w,32))*bpp/8` | the **2D / display view**: `DeviceBitmap::Create`, `InitPrimarySurface`, `CDPSSurface::Init`, kernel context default |
| 4 | 32/64 by width | rounded to 32 px | `next_pow2(max(w,32))*bpp/8` | mip / volume / cube texture layouts |
| 5 | 32/64 by width | rounded to 32 px | 0 | 4 and 8 bpp only -> DXTn compressed textures [INFERRED] |
| 6, 7 | -1 | | | invalid everywhere |

Note also: an AGP/PCI base (`base & 3 != 0`) forces height granularity 16
unconditionally, and multisample (`desc[0x10] & 0x7fc00`) halves both the
granularity and the alignment.

`GetHostAperturePitch` — the routine an earlier reading took for a tiling
selector — is what its name says: **the pitch of the CPU's view through the host
aperture**, and it is a stride, not a swizzle. It cannot be the memory layout,
because the allocator sizes surfaces with `roundup(w,32)`, not with
`next_pow2(w)`; the two would disagree for any non-power-of-two width.

Bits `[31:30]`: **no code in either binary ever sets them.** Every writer
preserves them, in one of two idioms:

```
  acc76:  and  $0xffffffc0,%eax     ; keep [31:30], clear format [29:24]
  3dcee:  and  $0xc0ffffff,%r12d    ; keep [31:30] and [23:0]
```

Exhaustive searches for `or $0x40000000 / $0x80000000 / $0xc0000000`,
`orb $0x40/$0x80/$0xc0` and `shl $0x1e/$0x1f` land on nothing that is a surface
cfg. The one `shl $0x1e` that looked promising (`.text+0xb4897`) is inside
`HSLDisableSyncOnOutput`, building MMIO `0x1c20`/`0x1c24` — the DVO crossbar,
which our own `p690-dvo-lx-dvi-output` work already characterises. Same story
for `[15:14]` (`and $0xc000,%ax`). Bits `[20:19]` are a real field
(`ModifySurfMode` arg 4) but are 0 on every path traced.

Bit `[23]` is **YUV component-order swap**, not a layout flag:
`Surface2d::SwapSurfMode` (`.text+0x7c2d0`, `orl $0x800000` / `andl
$0xff7fffff`) has exactly one caller, `mtx1568` `.text+0x527a1`, which tests
FourCC `'YUY2'` (`0x32595559`) against `'UYVY'`.

Register `0x121c` (index `0x87`), the one adjacent to the slot file, is written
only in the state reset and always as **zero** (`.text+0x3c4e5`, `0x3f775`,
`0x400a2`). There is no "detile" or "linear" register.

### 5.3 The stride caveat: our XAA runs at a power-of-two width

Our XAA/EXA program slot 0 / slot 1 with `displayWidth | 0x00020000 |
(fmt << 24)` — type 2 — and blit into the linear scanout correctly on hardware
(`src/parhelia_xaa.c:73`, `src/parhelia_exa.c:93`). If the type-2 aperture pitch
`next_pow2(w)*4` were also the engine's stride, that should have sheared.

It does not, and `randr-native-displaywidth-pow2-stride.patch` is why: it sets
`pScrn->displayWidth = next_pow2(width)` (1280 -> 2048), and `displayWidth` is
exactly what both XAA cfg words carry as their width field. At a power-of-two
width `next_pow2(w) == roundup(w,32)`, so the two stride formulas coincide and
our 2D path has been running in the one configuration where the distinction is
invisible.

This may **retroactively explain that patch**. It was written against a
display-controller constraint ("the DC fetches at `displayWidth`, so the picture
shears"), and it worked — but if the drawing engine also wants the power-of-two
aperture stride for a type-2/3 surface, the real requirement was that the engine
and the scanout agree, and rounding `displayWidth` up satisfied both at once.

Falsifiable, and worth testing independently of the 3D work: **at a
non-power-of-two `displayWidth`, does XAA with cfg type 2 shear, and does cfg
type 1 (arbitrary unrounded pitch) fix it without the pow2 rounding?** If so we
get back the wasted VRAM — 2048 vs 1280 px of stride is 37 % of the framebuffer.
Step 2 of §6 measures the row spacing directly and answers it in one run.

### 5.3b `UseTextiled` — the one live lead, and it is not a cfg bit

Three tiling-named registry switches exist in both binaries
(`RegistrySwitchManager` `mtx_drv.so` `.text+0x60d70`):
`Force3DTextureTextiledToABSundog` (default 0), `Force3DTextureTextiledToCDSundog`
(default 0), `UseTextiled` (**default 1**).

`mtx_drv.so` never reads any of them. The kernel reads `UseTextiled` in exactly
one place, `mtx263` (`.text+0x519a0`), and it flips a **surface-kind enum** at
`params+0x10` from `0xb` to `0xa`:

```
   519e4:  movl $0xb,0x10(%rdi)         ; default kind
   51ae0:  mov  0x4f0(%rax),%rdi        ; DriverSwitchesCommon -> UseTextiled
   51aea:  call *0x20(%rax)             ; GetValue()
   51aef:  je   51aa5                   ; 0 -> keep kind 0xb
   51af5:  cmpl $0x20,0x14(%rax) ; je   ; class 0x20 excluded
   51afd:  cmp  $0x29,%eax ; je         ; format enum 0x29 excluded
   51b02:  cmp  $0x2a,%eax ; je         ; format enum 0x2a excluded
   51b07:  movl $0xa,0x10(%rbx)         ; -> kind 0xa, "textiled"
```

Path `mtx372` (`.text+0x53100`) -> `mtx263` -> `mtx892` (`.text+0x52c40`,
allocator) -> `mtx3627` (`.text+0x29e70`) -> `mtx434`. **Where kind `0xa`/`0xb`
turns into a hardware bit is NOT FOUND** — no dispatch on 5..0xb near the surface
code, no `.rodata` attribute table indexed by it. This is the single concrete
remaining lead, and note it is about **textures**, not the render target.

The two `Force3D...Sundog` switches only select which of four texture state-block
pointers goes into a per-context slot table (`mtx3285` `.text+0x5d160`,
`mtx3708` `.text+0x5d6d0`); they never touch a register word.

### 5.4 So where does the measured swizzle come from?

It is **not** the type field (§5.1, §5.2), **not** cfg `[31:30]` (never
written), **not** cfg `[23]` (YUV swap), and **not** `0x121c` (always 0).

Two possibilities remain, and they are testable:

1. **The measurement conflated a swizzle with a wrong base.** The triangle was
   measured at BAR `0x2fa1020..0x3095f7f` while the bound slot-0 base in the same
   capture was `0x02000000` — a delta of `0xfa1020`, which is not a whole number
   of 1920x4 scanlines. §1.6 explains that as a stale capture (eight `gltri` runs
   deduped with `sort -u`, so the section set spans eight drawables), and our own
   2D instrument shows base -> BAR offset is 1:1 at seven different offsets. But
   the tile model was fitted against an *assumed* base, so a base error and a
   swizzle are not separable from that data.
2. **The framebuffer BAR presents a de-swizzled view that a raw VRAM read does
   not**, which is exactly what a "host aperture pitch" is for — and it would
   explain why `GetHostAperturePitch` returns a power-of-two pitch for types
   2/3/4 and a linear one for types 0/1.

The way to separate them is in §6: draw into a base **we** set and measure.
Until then the 64x32/8192-byte model stands as an empirical fit
(`tools/parhelia-3dtri.c --detile`, 0.38 px residual) with an unverified base.

### 5.5 The guaranteed linear read-back — one packet, vendor-exercised  [PROVEN]

Whatever the answer, there is no need to de-swizzle in software. Kernel
`mtx3388` (`mtx_parhelia.o` `.text+0x4fff0`, reached from the ioctl-shaped
wrapper `mtx3223` `.text+0x50650`) BITBLTs an **arbitrary** surface into a
**type-1, arbitrary-pitch, system-memory** destination:

```
   500dd:  sub  (%rdx),%ecx           ; dst pitch = raw pixel width, NOT rounded
   500e2:  mov  0xd8(%rbx),%edi       ; source surface base
   500e8:  mov  0xdc(%rbx),%r8d       ; source surface cfg  (whatever type it is)
   500f2:  or   $0x10000,%ecx         ; dst TYPE = 1  (linear)
   505c2:  or   $0x10000000,%ecx      ; bpp  8 -> fmt 0x10
   505cd:  or   $0x20000000,%ecx      ; bpp 16 -> fmt 0x20
   5010e:  or   $0x34000000,%edx      ; bpp 30 -> fmt 0x34
   50114:  or   $0x30000000,%eax      ; else   -> fmt 0x30
   50139:  movl $0x2c2,0xb08(%rax)    ; DWGCTL: opcode 2 BITBLT, BOP GXcopy,
                                      ;         src slot 2, dst slot 0
   50148:  movl $0xffffffff,0xb0c(%rax)  ; plane mask -> 0x1324
   5015d:  mov  %ecx,0xb38(%rdx)      ; slot0 cfg  = linear destination
   50163:  mov  %r8d,0xb40(%rdx)      ; slot2 cfg  = the source surface
   5016a:  mov  %edi,0xb30(%rdx)      ; slot2 base
   502ab:  or   $0x3,%eax             ; dst base |= class 3  (PCI/system memory)
   502b6:  mov  %eax,0xb28(%rcx)      ; slot0 base
   502cf:  call mtx1331               ; emit
```

`0x2c2` decodes as `[3:0]=2` BITBLT, `[7:4]=0xC` GXcopy, `[9:8]=2` source slot 2,
`[11:10]=0` destination slot 0 — consistent with the slot map in §3.4.

**This is the drop-in de-swizzler.** Bind our 3D target as the *source* in slot 2,
a type-1 linear buffer as the *destination* in slot 0, `DWGCTL = 0x2c2`, and read
the destination with a plain `y*pitch + x*4`. No layout knowledge required, and
it is a path the vendor itself runs.

### 5.6 What to do

1. Do the base calibration in §6 first. It may dissolve the whole tiling
   question.
2. Bind the colour target the way the vendor does — **type 2, format 0x32** —
   because that is the one configuration proven to render.
3. Read results back with the `DWGCTL 0x2c2` blit of §5.5 into a type-1 linear
   buffer, or with `--detile` if the swizzle turns out to be real.
4. Only then, if a directly-linear target is still wanted, try **type 1** (not
   type 0 — type 0 has no pitch field at all, its size comes from a raw byte
   count) and see whether an opcode-4 draw honours it. Nothing in either binary
   ever issues a 3D draw into a type-0/1 surface, so this is untested territory.
5. Do **not** touch bits `[31:30]`, `[23]` or `[15:14]`, and do **not** put a
   byte pitch in `[13:0]` — it is a pixel width (§2).

### 5.7 Still OPEN

* Where the measured 64x32 swizzle actually comes from, and whether it survives
  a self-set base (§6).
* Where surface kind `0xa`/`0xb` from `UseTextiled` reaches hardware
  (`mtx892` -> `mtx3627` -> `mtx434`).
* Whether a type-1 colour target is honoured by the 3D destination path.
* The depth format code (§3.3) and whether depth shares the colour geometry.

---

## 6. Test recipe

Run with **X stopped** — with the vendor X driver alive our submissions execute
but stop drawing (`parhelia-x-owns-the-engine`). Command ring in `GFP_DMA32`
system memory, never in the framebuffer BAR.

### Step 0 — gate

```
./3dtri --dmafill --nowriteback
```
Must report `green px: at base 0x0 = 120000`. If not, stop: the engine is not in
a state where anything below means anything.

### Step 1 — pick the target

```
  base   = 0x04000000        (64 MB into VRAM; 8 KB aligned, clear of the
                              1920x1200 desktop at 0 and of the vendor's
                              0x01000000/0x02000000 surfaces)
  width  = 1920 px           (multiple of 64, so both stride formulas are exact)
  height = 256 px            (multiple of 32, the type-2/3 granularity)
  size   = 1920 * 256 * 4    = 0x1E0000
  cfg    = 0x32020780        type 2, exactly what the vendor binds for 3D
```

`0x32020780` is exactly `rt_ctrl(32, 1920)` in `tools/parhelia-3dtri.c` — use it
for every step up to step 7. The type-1 variant tried in step 7 is
`0x32010780`. Check `base + size` is inside the aperture the tool prints at
startup.

### Step 2 — measure the stride, and calibrate the base

```
./3dtri --dmafill --nowriteback --filloff 0x04000000
./3dtri --nzdump /root/tmp/fill2d.txt --scanbase 0x04000000 --vramscan 0x200000
```

The instrument fills a 400x300 rectangle at `(200,150)` with cfg `0x32020780`
(width 1920, **type 2**). Expect 120000 non-black dwords in **300 runs of 400
consecutive dwords**. The gap between consecutive runs is the answer to §5.1 for
the 2D engine, at zero risk:

| row spacing | first run at | meaning |
|---|---|---|
| **7680 B** | `0x04000000 + (150*1920 + 200)*4` = `0x0411A0E0` | stride = `roundup(w,32)*4`; the type field does **not** change the 2D stride |
| **8192 B** | `0x04000000 + (150*2048 + 200)*4` = `0x0412BFE0` | stride = `next_pow2(w)*4`; the drawing engine wants the aperture pitch too, and our pow2 `displayWidth` is load-bearing |
| neither | — | the layout is tiled even for 2D; compare against §5.4 |

The control: re-run the same fill against a **1024**-wide target, where
`next_pow2(1024) == roundup(1024,32)` and the two predictions collapse.

This also pins the base -> BAR-offset relation for the 3D measurement that
follows, which is exactly what §5.4 says is missing. Either way it confirms base,
cfg and aperture in one shot.

Note `--dmafill` zeroes and scans `pitch * 768` dwords from `--filloff`, i.e.
5.6 MB at pitch 1920 — keep `0x04000000 .. 0x04600000` free, or lower `--pitch`.

### Step 3 — clear it

Same fill with `0x1314 = 0` and `0x1308 = 0`, `0x138c = (256<<16)|1920`, or just
memset through the aperture before the draw (the tool already zeroes
`pitch*768` dwords at `--filloff`).

### Step 4 — emit the binding and one triangle

Prepend to the state we submit, in this order:

```
  1  00000004 8d8c8988 04000000 32020780 00000000 32020000   colour + null depth
  2  00000002 21218b8a 04000000 32020780                      slot 1 = same
  2b 00000002 21218f8e 04000000 32020780                      slot 3 = same (§3.0)
  3  00000001 212121c9 ffffffff                               PLNWT
  4  00000004 bebfcbca 00000000 01000780 0bb80024 007fe000    scissor (0,0)-(1920,256)
                                                              + 0x12fc / 0x12f8
  5  4104000b 00000002 00000160 00000001 00000000
        44700000 c3000000 3f000000 3f800000     c[0x160] = ( 960, -128, .5, 1)
        44700000 43000000 3f000000 43800000     c[0x161] = ( 960,  128, .5, 256)
     (for a full 1920x256 viewport at origin (0,0))
  6  00000001 212121c0 00a60ec4                               3D DWGCTL last
```

then the vertex section and the draw packet `97080001 <start> <end>`, unchanged
from the working same-boot replay.

Note step 4's tag `0xbebfcbca` = indices `0xca, 0xcb, 0xbf, 0xbe` = `0x1328`,
`0x132c`, `0x12fc`, `0x12f8` — the vendor's own packet, values substituted.

With the existing tool this is:

```
./3dtri --ringreplay ring7.hex sec7.hex reloc7.txt \
        --rtslot 0 --rtbase 0x04000000 --rtcfg 0x32020780 \
        --scissor 0x01000780 \
        --replayrt 0x04000000 --vramdiffbase 0x04000000 --vramdiff 0x200000 \
        --pitch 1920 --bpp 32
```

`--rtslot 0` is **required**: the tool defaults `g_rt_reg` to `0x1238` (slot 3),
which is wrong (§7).

### Step 4b — the same thing as a plain ordered register list

If you are building the stream with `warp_add()` rather than splicing hex, this
is the whole of it. Order matters only in that the surface pair must precede the
3D `DWGCTL`, and the `DWGCTL` must precede the draw packet:

```c
  /* --- target --- */
  warp_add(rg, vl, &n, 0x1220, base);          /* colour base, |class in [1:0] */
  warp_add(rg, vl, &n, 0x1224, cfg);           /* rt_ctrl(32, width)           */
  warp_add(rg, vl, &n, 0x1230, 0);             /* depth base  = null           */
  warp_add(rg, vl, &n, 0x1234, 0x32020000);    /* depth cfg   = null           */
  warp_add(rg, vl, &n, 0x1228, base);          /* slot 1: harmless to mirror   */
  warp_add(rg, vl, &n, 0x122c, cfg);
  warp_add(rg, vl, &n, 0x1238, base);          /* slot 3: DWGCTL[11:10] says 3  */
  warp_add(rg, vl, &n, 0x123c, cfg);           /*   -- see 3.0, costs nothing   */

  /* --- raster extent, in TARGET coordinates --- */
  warp_add(rg, vl, &n, 0x1328, (y0 << 16) | x0);          /* scissor min       */
  warp_add(rg, vl, &n, 0x132c, (y1 << 16) | x1);          /* scissor max, excl */
  warp_add(rg, vl, &n, 0x1324, 0xffffffff);               /* plane write mask  */
  warp_add(rg, vl, &n, 0x12f4, 0x42000000);               /* guardband, 32.0f  */
  warp_add(rg, vl, &n, 0x12f8, 0x007fe000);               /* per-surface       */
  warp_add(rg, vl, &n, 0x12fc, 0x0bb80024);               /* per-surface       */
  warp_add(rg, vl, &n, 0x12c8, 0x001000e4);               /* per-surface       */
  warp_add(rg, vl, &n, 0x12cc, 0x000000ff);               /* per-surface       */
  warp_add(rg, vl, &n, 0x12d0, 0x000000ff);               /* per-surface       */

  /* --- viewport, as block-file constants c[0x160]/c[0x161] --- */
  /*  c[0x160] = ( w/2, -h/2, (f-n)/2, 1.0 )                                   */
  /*  c[0x161] = ( x0 + w/2, y0 + h/2, (n+f)/2, (float)h )                     */

  /* --- last --- */
  warp_add(rg, vl, &n, 0x1300, 0x00a60ec4);               /* 3D DWGCTL         */
```

then the vertex stream and the `0x97080001 <start> <end>` draw packet.

`0x1300 = 0x00a60ec4` decomposes as `0x860e00 | 0x200000 | 0xc4`: opcode 4 in
`[3:0]`, BOP GXcopy in `[7:4]`, and `0x860e00` is an unconditional constant on
the 3D path, so nothing per-draw varies bits `[11:9]`. Those bits nominally name
destination slot 3 (§3.0) while `mtx2954` binds slot 0 — which is why the list
above writes slots 0, 1 and 3 to the same base and leaves `DWGCTL` alone.

### Step 5 — read it back

```
./3dtri --detile 0x04000000 0x04400000 1920 1920 256 7680
./3dtri --nzdump /root/tmp/tri3d.txt --scanbase 0x04400000 --vramscan 0x200000
```

`--detile` writes a linear copy at `0x04400000`; the nzdump of that copy should
show contiguous scanline runs whose x/y match the triangle's NDC coordinates
through the §3.2 viewport transform. A dump of the *tiled* original instead
shows 8192-byte-periodic clusters — that difference is itself the tiling check.

### Step 6 — the guaranteed linear read-back (do this before de-swizzling)

Instead of trusting the swizzle model, use the vendor's own path (§5.5): a
one-packet BITBLT from our 3D target into a linear destination. Everything is a
register write we already know how to emit:

```
  0x1230 = rt_base           0x1234 = rt_cfg        (slot 2 = SOURCE, the 3D target)
  0x1220 = lin_base          0x1224 = lin_cfg       (slot 0 = DESTINATION, linear)
  0x1324 = 0xffffffff                               (plane mask)
  0x1300 = 0x000002c2                               (BITBLT, GXcopy, src slot 2,
                                                     dst slot 0)
  0x1304 = 0                                        (SRCXY)
  0x1308 = 0                                        (DSTXY)
  0x138c = (h << 16) | w                            (LEN -- fires)
```

with `lin_cfg = w | (1 << 16) | (0x32 << 24)` — **type 1**, whose pitch field is
an arbitrary pixel width with no 32-rounding. Put `lin_base` in VRAM
(`| 0` = local) for the first try; the vendor uses system memory (`| 3`), which
also works and needs no aperture read.

Then dump `lin_base` and read it with a plain `y*w*4 + x*4`. If that produces a
clean triangle while a raw dump of `rt_base` does not, the surface really is
swizzled and this blit is the read-back path from here on. If a raw dump of
`rt_base` is *already* clean, there was never a swizzle — §5.4 possibility 1 —
and `--detile` should be retired.

### Step 7 — only then, a directly linear target

Repeat steps 3–5 with `--rtcfg` = `w | (1 << 16) | (0x32 << 24)`
(**type 1**, not type 0 — type 0 has no pitch field at all, its size comes from a
raw byte count). Outcomes: pixels at `y*w*4 + x*4` means a linear 3D target
works; the same footprint as type 2 means the type field is not a layout switch
for the destination either (consistent with §5.1); nothing drawn means type 1 is
rejected for a 3D destination. All three are useful answers.

One variable per run, persist the result before the next
(`parhelia-bench-one-at-a-time`).

---

## 7. Bugs this uncovers in our own tooling

1. **`tools/parhelia-3dtri.c:135`** — `g_rt_reg = 0x1238`, i.e. `--rtbase`
   patches **slot 3** only. The reasoning in the comment ("the destination slot
   the 3D DWGCTL selects, bits [11:10]") is not silly — DWGCTL `[11:10]` *is* the
   destination slot at every 2D site (§3.0), and `0x00a60ec4` does say slot 3.
   But those bits come from the unconditional `or $0x860e00`, `mtx2954` binds the
   live target into **slot 0** and never emits slot 3, and slot 3 is null in
   every capture. The two readings are not separated (§3.0).

   **Fix: patch slots 0, 1 and 3, not slot 3 alone.** That is correct under
   either reading. The original worry — "rewriting slots 0/1 breaks the 2D blits
   in the same stream, which use slot 1 as a SOURCE" — is real for a full-stream
   replay, and is handled by patching only writes whose accompanying cfg is the
   3D one (`0x32020780`), leaving the type-3 2D descriptors alone.

2. **`tools/parhelia-3dtri.c:3090`** (the `--warpring` path) —

   ```c
   warp_add(rg,vl,&n, 0x1328, 0x0fff0fff); warp_add(rg,vl,&n, 0x132c, 0);
   ```

   The scissor is **inverted**: min = (4095,4095), max = (0,0) is an empty box,
   so nothing can rasterise. It must be `0x1328 = 0`,
   `0x132c = (height << 16) | width`.

3. **`tools/parhelia-3dtri.c:4355` and `:4594`** (the `--tri920` / `--warpstream`
   paths) — bind `0x1220 = 0` and put the render target in slot 1
   (`0x1228 = g_rtoff`). The 3D destination is slot 0, so these draw into
   whatever is at VRAM offset 0 — the visible framebuffer — while the buffer the
   code then scans at `g_rtoff` stays empty.

4. **`src/parhelia_reg.h:549`** — `PARHELIA_REG_SURF_SCRATCH 0x1238 /* scratch /
   2D-fill dst */` and line 514's `SURF_COLOR 0x1228 /* slot 1: colour render
   target */` carry a stale slot attribution. Slot 0 is the destination for both
   our 2D fill (DWGCTL `0xc0`, `[11:10] = 0`) and the 3D draw;
   `src/parhelia_xaa.c:76` already gets this right ("slot 0 = dst"). Rename them
   to plain slot numbers and let the DWGCTL `[11:10]` field name the role.

5. **`src/parhelia_exa.c` / `src/parhelia_xaa.c` — the 2D/3D cfg views.** Our
   2D code writes cfg type **2** (`PARHELIA_SURF_CFG_CTRL 0x00020000`). The
   vendor's 2D view is type **3**, and it switches the *same* bitmap to type 2
   only when it becomes the 3D destination
   (`SurfDesc2dList::AssignCurrentSurfDescTo`, §5.1). The two are byte-for-byte
   equivalent in the allocator, so this is not a bug today — but when the driver
   grows a 3D path it should adopt the same two-view discipline (type 3 +
   screen format for 2D, type 2 + `fmt3d` for 3D) rather than one cfg for both.

---

## 8. PROVEN / INFERRED / OPEN

**PROVEN**

* The binding packet, its framing, its tag decode, its position in the frame, and
  that it is an ordinary callable-section register write we can emit ourselves —
  from 40 hardware captures *and* from `mtx2954` `.text+0xf8e0`. Every section
  carrying it is called with the header-ful (`|2`) framing we already use.
* Colour = slot 0, depth = slot 2, slot 1 = the drawable's other buffer; the 3D
  binder never emits slot 3. Second witness from the fill path
  (`mtx1192` `mtx_parhelia.o .text+0x3dcb0`).
* `cfg = width_px | (type<<16) | (fmt<<24)`; `[13:0]` is a **pixel width with
  granularity 32**, not a byte pitch; `[20:19]` is a 2-bit sub-mode (always 0);
  `[23]` is the **YUV component-order swap** (`Surface2d::SwapSurfMode`, sole
  caller tests FourCC `'YUY2'`); `[29:24]` format; `[31:30]` and `[15:14]` are
  **never written by any code path in either binary**.
* The complete type table: 0 raw blob (1-row, size from desc `+0xc`), 1 linear
  with arbitrary unrounded pitch (2-row), 2 the 3D view and 3 the 2D/display view
  (32-row, and byte-for-byte identical throughout the allocator), 4 texture
  layouts, 5 4/8 bpp only, 6/7 invalid. AGP/PCI bases force 16-row granularity;
  multisample halves granularity and alignment.
* **Types 2 and 3 address the same pixel at the same byte.**
  `SurfDesc2dList::AssignCurrentSurfDescTo` flips one live bitmap — the screen
  pixmap included — between them with no data conversion, and the vendor draws 2D
  into surfaces it just rendered 3D into.
* Type 1 is a genuinely linear surface class, and `mtx3388` BITBLTs any surface
  into one with `DWGCTL = 0x2c2` (src slot 2, dst slot 0, dst base `|3`).
* DWGCTL `[11:10]` = destination slot, `[9:8]` = source slot
  (`SurfDesc2d::Init` stores `slot << 10`); obeyed by the priming draw (`0xf5`,
  slot 0), `ClearSurface` (`0xcc0`, slot 3), the readback blit (`0x2c2`) and XAA.
* The real clear is a solid fill: `DWGCTL 0x0cc0`, `0x1314` FCOL, `0x1308`
  origin, `0x138c` extent (writing it fires), `0x1324` plane mask.
* Allocation `size = roundup(w,32) * roundup(h,heightGran) * bpp/8 * slices`,
  rounded to the alignment; 0x2000 for a 32 bpp type-2/3 local surface.
* `base = byte offset | class tag [1:0]`, and it is the framebuffer-BAR offset
  1:1 — verified by our own 2D instrument at seven different offsets.
* The surface "dims" word never reaches a register; there is no surface-height
  register to program.
* Scissor `0x1328` = min, `0x132c` = max, both `(y<<16)|x`, full range
  `0x0fff0fff` — from the captures *and* from `mtx2187` `.text+0x3ccb0`. Same
  packing for 2D `0x1308` DSTXY and `0x138c` LEN.
* The viewport constants `c[0x160]`/`c[0x161]` and their upload framing.
* A null depth binding is accepted; no capture ever binds a real Z surface.
* `0x12f4 = 0x42000000` is the GUARDBAND float (32.0f), not a cfg.

**INFERRED**

* The 3D rasteriser ignores DWGCTL `[11:10]` and always takes colour from slot 0
  and depth from slot 2. Strongly supported (`mtx2954` emits exactly that pair
  and never slot 3; slot 3 is null in every capture) but not separated from the
  alternative — bind slots 0/1/3 and the question is moot.
* Type 5 is DXTn (only 4 and 8 bpp are legal for it).
* A type-1 colour target would be honoured by the 3D destination path: the 3D
  binder copies the type straight from the surface object, the opcode-4/5
  emitters explicitly preserve it, and the kernel's context default for slot 0 is
  type 3. No vendor path demonstrably draws 3D into a type-0/1 surface.
* Format bit assignment within the `0x3x` family (bit 1 = alpha, bit 2 = 10:10:10).
* Depth format is one of `0x21 0x22 0x24..0x27 0x2a..0x2d` (16-bit) or
  `0x33 0x38 0x3a` (32-bit); `ForceDepthToS8Z24Sundog` says the 32-bit one is
  S8Z24.

**OPEN**

* **Where the measured 64x32 / 8192-byte swizzle comes from.** It is not the type
  field (types 2 and 3 alias), not `[31:30]`, not `[23]`, not `0x121c`. Either it
  is a physical layout both engines share, or the measurement's assumed base was
  wrong (§5.4). §6 steps 2 and 6 separate the two.
* Where the `UseTextiled` surface-kind enum `0xa`/`0xb` (kernel `mtx263`
  `.text+0x519a0`) reaches hardware — `mtx892` -> `mtx3627` -> `mtx434`. Note it
  concerns *textures*, not the render target.
* The depth format code, and whether depth shares the colour geometry. The table
  is on the libGL/Sundog side; `ForceDepthToS8Z24Sundog`, `EnableZCompSundog`,
  `Use2DClearForceZUncompSundog`, `Use3DClearSundog` and
  `UseZCompInMultContextSundog` are all registered in both binaries and read by
  neither.
* cfg bits `[15:14]`; never non-zero in any capture and never written.
* Whether a second draw in the same frame may re-bind mid-frame (the vendor never
  does — the binding is emitted once per frame).
