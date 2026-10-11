# Parhelia-512 — what gates fragment production

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Clean-room reverse engineering from binaries only (`objdump`/`nm`/`objcopy`):

* `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` — vendor kernel core,
  relocations intact, symbols obfuscated to `mtxNNNN`.
* `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` — the ICD;
  emits a *record stream*, never a register.
* `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` — 2D.
* the win32 `MTXPARD.dll` (cached disassembly `scratchpad/pard.asm`).

Ground truth for every value quoted here: `captures/live-ring-0528/sec7.hex` +
`reloc7.txt` (the inter-draw span of a frozen `gltri`), decoded with the packet
grammar in `docs/parhelia-3d-submission-audit.md` §A.3, and
`captures/live-ring-0528/decoded.txt` (eight complete frames).

Tags: **[PROVEN]** = read out of a disassembly or out of the capture itself.
**[INFERRED]** = the best self-consistent reading, not proven.

---

## 0. Executive answer

The captured register state contains **nothing that kills fragments**. Depth
test, stencil test, alpha test and blending are all *disabled* at the draw,
`PLNWT` (`0x1324`) is `0xffffffff`, the scissor is the client's full 320×240
window, the guard band is 32.0 and the z-clip range is full. A byte-exact
replay of that state cannot be suppressed by a raster-state bit. §4 enumerates
every candidate and shows each one open.

So the draw is being *aimed* somewhere other than where it is being looked for.
Three things now matter, in this order:

1. **The base register holds an address in the *engine's* space, and that is
   not the framebuffer BAR** (§2.6, §2.6b). The encoding itself is settled and
   simple: bits[1:0] are a memory class (0 = local, 2 = AGP, 3 = PCI/system),
   the rest is a plain byte address, and there is no MMU, page table or
   relocation — `Surface2d::Enable` @0x7bf00 writes the field straight through
   with only `| class`. But that space is **banked** (bank A at 0, bank B at
   `0x08000000`, host aperture at `0x10000000`), and
   `MmSurfaceManager::AllocateSurfaceBlock` @0x76590 receives the *engine
   address* and the *framebuffer-aperture offset* from the kernel as **two
   separate values** and keeps them apart. Reading the BAR at the engine
   address is only correct when the two coincide. This is the most economical
   explanation for "VRAM `0x02000000` reads as zeros".

2. **The destination slot is not settled.** DWGCTL `0x00a60ec4` contains
   `0xe00`, which in the 2D field map means "dst = slot 3, src = slot 2" — and
   `0xe00` is part of a constant the vendor ORs in unconditionally on every 3D
   draw. In the capture slot 3 is the *null* descriptor `{0, 0x32020000}` while
   slot 0 holds the live target `{0x02000000, 0x32020780}`. §3.3 lays out both
   readings and the four-run experiment that decides it.

3. **A surface has two pitches** (§2.5). The physical footprint uses
   `roundUp(width, 32)`, but the **host-aperture pitch** — the pitch of the
   CPU-visible, de-tiled window — is `pow2ceil(width) * bpp/8` for types 2–5
   (`MmSurfDesc::GetHostAperturePitch` @0x745c0). The driver maintains a
   separate de-tiling aperture precisely because the raw layout is not the one
   the pitch field advertises, so `base + y*1920*4` is not a safe way to find a
   pixel in a type ≥ 2 surface.

Before spending another hardware run on the 3D path, run **Test 0** and
**Test 0b** (§6.1): fill VRAM `0x02000000` with the *already-proven 2D engine*
and read it back through the same path that produced the null result, and check
that BAR offset 0 shows the desktop (the screen surface's base register value is
provably **0**, §2.7). Together they separate "the 3D draw went elsewhere" from
"the engine address is not a BAR offset", and they cost one run each. If they
show the spaces do not coincide, stop hunting for the vendor's buffer and
retarget the draw to an address you allocated yourself.

## 1. The full register state at the draw

Decoded from `sec7.hex`/`reloc7.txt` in execution order; "last write" is the
ring-entry index of the final write before the `0x97080001` draw packet at
ring[749]. 123 sections, 112 register-list packets (opcode 0x00), 10 block
loads (opcode 0x41), 1 draw (opcode 0x17). [PROVEN — capture]

| reg | name | value at draw | last write |
|---|---|---|---|
| 0x1220 | SURF0_BASE | `02000000` | ring[691] |
| 0x1224 | SURF0_CFG | `32020780` | ring[691] |
| 0x1228 | SURF1_BASE | `01000000` | ring[696] |
| 0x122c | SURF1_CFG | `36020780` | ring[696] |
| 0x1230 | SURF2_BASE | `00000000` | ring[691] |
| 0x1234 | SURF2_CFG | `32020000` | ring[691] |
| 0x1238 | SURF3_BASE | `00000000` | ring[428] |
| 0x123c | SURF3_CFG | `32020000` | ring[428] |
| 0x1300 | DWGCTL | `00a60ec4` | ring[711] |
| 0x1324 | PLNWT | `ffffffff` | ring[701] |
| 0x1328 | SCISSOR_MIN | `00000000` | ring[541] |
| 0x132c | SCISSOR_MAX | `00f00140` | ring[541] |
| 0x12c8 | DEPTH_STENCIL | `001000e4` | ring[551] |
| 0x12cc | STENCIL_MASKS | `000000ff` | ring[551] |
| 0x12d0 | STENCIL_WRITE | `000000ff` | ring[551] |
| 0x12e4 | ALPHATEST | `0000000e` | ring[546] |
| 0x12e8 | BLENDCTL | `00800202` | ring[546] |
| 0x12f4 | GUARDBAND | `42000000` (32.0f) | ring[716] |
| 0x12f8 | ThresholdAA | `007fe000` | ring[541] |
| 0x12fc | RASTERMODE | `0bb80024` | ring[541] |
| 0x12b0 | MemLevels | `3fff8842` | ring[706] |
| 0x12c0/0x12c4 | ZCLIP_MIN/MAX | `80000000`/`7fffffff` | ring[551] |
| 0x1130 | WARP_PIPECTL | `0200200c` | ring[661] |
| 0x10b0 | PRIMWORD | `0006305c` | ring[666] |
| 0x1000/0x1040 | VTXBASE0/STRIDE | `00000003`/`00000004` | ring[744] |
| 0x1060/0x1064 | VTXCOUNT/FIRST | `000280cc`/`0` | ring[676] |
| 0x1100 | WARP_ENTRY | `076acd03` (boot-specific) | ring[651] |

**Two captures, taken on different boots, were compared dword for dword.**
Every *sysmem* address changed (fence page `0x179e7000` → `0x18a2a000`, WARP
entry `0x07d57783` → `0x076acd03`, the vertex-buffer operands, every DWGSYNC
tag), while **every surface base and cfg is bit-identical**: slot 0
`{0x02000000, 0x32020780}`, slot 1 `{0x01000000, 0x36020780}`, slots 2 and 3
`{0, 0x32020000}`. So the surface base lives in a *deterministic* address
space with a fixed driver-side layout — it is not a dynamically allocated
GART/sysmem address. Every raster-state value listed above is also identical
across the two boots. [PROVEN]

Everything else in the span is X's 2D work (`DWGCTL = 0x000005c0` fills through
surface slot 1, `PAT0/PAT1 = 0x77ddbbee`, `LEN = 0x04b00780` = 1920×1200 and
two `0x00200020` = 32×32 spots) and the two pipeline-priming pseudo-draws
(`DWGCTL = 0x001000f4` / `0x005000f5`).

---

## 2. The surface descriptor file (Q1)

### 2.1 Pair layout — base first, cfg second [PROVEN]

Four slots, `{base, cfg}` at `0x1220 + 8*slot` and `0x1224 + 8*slot`:

| slot | base | cfg | tag dwords seen in the capture |
|---|---|---|---|
| 0 | 0x1220 (idx 0x88) | 0x1224 (idx 0x89) | `0x8d8c8988`, `0x8988bfb9` |
| 1 | 0x1228 (idx 0x8a) | 0x122c (idx 0x8b) | `0x21218b8a`, `0x21c58b8a` |
| 2 | 0x1230 (idx 0x8c) | 0x1234 (idx 0x8d) | `0x8d8c8988` |
| 3 | 0x1238 (idx 0x8e) | 0x123c (idx 0x8f) | `0x21218f8e` |

The *order within the descriptor object* is the direct proof: `mtx969`
@0x5b2e0 is the "bits per pixel of this descriptor" helper and its first
instruction is

```
5b2e0:  mov  edx,DWORD PTR [rdi+0x4]
5b2e3:  and  edx,0x3f000000
```

so the **config word is at descriptor offset +4** (base is +0), and the format
field is exactly **6 bits, [29:24]**. [PROVEN]

The same word appears in the *aperture descriptor bank* at MMIO
0x0000–0x013c, third dword of each 4-dword entry — the live trace
`captures/mmio-draw-0528-decoded.txt` writes

```
W 0x0010 = 0x10040000   W 0x0014 = 0x0   W 0x0018 = 0x30030780   W 0x001c = 0x10708
W 0x0020 = 0x10ec0000   W 0x0024 = 0x7f60000  W 0x0028 = 0x5000020  W 0x002c = 0x10001
```

`0x30030780` there is bit-identical to the `0x122c` value in the ring, which is
independent confirmation that this word is a surface descriptor and that
`{gpuAddr, ?, cfg, dims}` is the entry layout — `0x001c = 0x10708` decodes as
height 0x708, depth 1, and `0x0028 = 0x05000020` / `0x002c = 0x00010001` as a
1-bpp 32-px-wide, 1-row staging buffer. `EmulMmSurfaceMapper::AllocateApertureBlock`
@0x74c40 builds this table (§2.6b). [PROVEN]

### 2.2 Config word bitfields

```
bits [13:0]   pitch in PIXELS, forced to a multiple of 32
bits [15:14]  preserved by every RMW, unidentified
bits [18:16]  surface type / layout class   (§2.5)
bits [22:19]  preserved by every RMW, unidentified
bit  23       linear / tiled ("swap surf mode")   [PROVEN, §2.5]
bits [29:24]  pixel format code (6 bits)
bits [31:30]  driver-programmed flag pair       (§2.6)
```

`bits[29:24]` is [PROVEN] three ways. First `mtx969` masks `& 0x3f000000`.
Second, `mtx1192` @0x3dcb0 — the pipeline-priming emitter — rewrites the colour
target's format with a mask that clears **exactly** those six bits:

```
   3dcce:  mov  r12d,DWORD PTR [rax+0xb38]   ; cfg[0] shadow
   3dcd5:  mov  r13d,DWORD PTR [rax+0xb28]   ; base[0] shadow
   3dcee:  and  r12d,0xc0ffffff              ; clear bits [29:24]
   3dcf5:  or   r12d,0x32000000              ; format = 0x32
```

Third `mtx3388`
@0x50000 which ORs `0x30000000` / `0x34000000` into a word carried in `ecx`
from `[rdx+0xc]`, i.e. leaving the low bits untouched:

```
500f8:  cmp  esi,0x8          ; source bit depth
50101:  cmp  esi,0x10
5010e:  or   edx,0x34000000   ; 30 bpp GigaColor
50114:  or   eax,0x30000000   ; 24/32 bpp
50119:  cmp  esi,0x1e         ; 30
```

`bits[13:0] = pitch` is [PROVEN] by the values themselves: every full-screen
descriptor in the capture ends in `0x780` = 1920, matching the X server's
1920-pixel screen, and the two 32×32 blit surfaces carry `0x020` = 32 together
with `LEN = 0x00200020` (32×32). It is also the mask used all over the kernel
for surface geometry (`and dx,0x3fff`, e.g. `mtx…` @0xc724/0xc744/0xc74a).

**bpp table** (from `mtx969`'s complete comparison tree, [PROVEN]):

| bpp | format codes |
|---|---|
| 1 | 0x00, 0x02, 0x04, 0x05 |
| 4 | 0x08 |
| 8 | 0x09, 0x0a, 0x0b, 0x0c, 0x10, 0x11, 0x12 |
| 16 | 0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e |
| 32 | 0x30, 0x32, 0x33, 0x34, 0x36, 0x38, 0x3a, 0x3b |

Observed codes and their roles in this capture:

| cfg | format | type | pitch | role |
|---|---|---|---|---|
| `0x32020780` | 0x32 | 2 | 1920 | **GL colour render target** (slot 0) |
| `0x30030780` | 0x30 | 3 | 1920 | 2D/DPS full-screen surface (slot 1) |
| `0x36020780` | 0x36 | 2 | 1920 | slot 1 while the GL draw is live |
| `0x30030020` | 0x30 | 3 | 32 | 32×32 2D spot surface |
| `0x32020000` | 0x32 | 2 | **0** | **the null descriptor** — see below |
| `0x05000020` | 0x05 | 0 | 32 | 1-bpp aperture entry |

`0x30` vs `0x32` vs `0x36` are all 32-bpp codes. `mtx3388` picks `0x30` for a
plain 24/32-bit source; a separate path force-rewrites the colour target to
`0x32` (`cfg = (cfg & 0xc0ffffff) | 0x32000000`). [PROVEN that both are 32 bpp;
[INFERRED] that `0x30` = X8R8G8B8 and `0x32` = A8R8G8B8, i.e. they differ only
in whether the alpha byte is meaningful.]

### 2.3 Why `0x32020000` has pitch 0 — it is the *disabled-slot* encoding [PROVEN]

`0x32020000` never appears with a non-zero base. In the capture it is always
written as the pair `{base = 0, cfg = 0x32020000}`, and it appears on exactly
the slots that are **not in use for the current operation**:

* slot 2 (`0x1230`/`0x1234`) at the draw — and depth testing is **off**
  (`0x12c8` bit 0 = 0), so `gltri` has no depth buffer to bind;
* slot 3 (`0x1238`/`0x123c`) throughout;
* slot 0 between frames, when the GL target is not bound.

So `{base = 0, cfg = format<<24 | type<<16 | pitch 0}` is "slot unused". A
pitch of 0 is not a bug and not a puzzle: it is the reset value written by the
same emitter that writes a live descriptor, with the geometry zeroed. The
format nibble `0x32` and type `2` survive because they are the object's
defaults, not because anything reads them. [PROVEN — every occurrence in
`decoded.txt`: `0x1220=00000000` 57×, always with `0x1224=32020000`;
`0x1220=02000000` 8× (once per frame), always with `0x1224=32020780`.]

**Consequence for a replay: nothing needs fixing here.** The pitch-0
descriptors belong to slots the draw does not use.

### 2.4 One packet that pins the whole index→register mapping [PROVEN]

`mtx1192` @0x3dcb0 builds the priming pseudo-draw as a single 10-dword payload,
and it is reproduced byte for byte in the capture (the two priming pseudo-draws).
Because every value is a literal or a named shadow field, this one block
independently confirms eight register indices at once:

```
   3dd2e:  mov  DWORD PTR [rcx-0x2c],0x9          ; payload dwords - 1 = 9
   3dd35:  mov  DWORD PTR [rcx-0x28],0xb2cbcac0   ; idx c0, ca, cb, b2
   3dd3c:  mov  DWORD PTR [rcx-0x24],0x5000f5     ;   0x1300 DWGCTL   = 0x005000f5
   3dd51:  mov  DWORD PTR [rcx-0x20],0x0          ;   0x1328 SCISSOR_MIN = 0
   3dd7c:  mov  DWORD PTR [rcx-0x1c],eax          ;   0x132c SCISSOR_MAX = 0x00010001
   3dd5c:  mov  DWORD PTR [rcx-0x18],0x0          ;   0x12c8 DEPTH_STENCIL = 0
   3dd63:  mov  DWORD PTR [rcx-0x14],0x8988bfb9   ; idx b9, bf, 88, 89
   3dd6a:  mov  DWORD PTR [rcx-0x10],0x1          ;   0x12e4 ALPHATEST = 1
   3dd71:  mov  DWORD PTR [rcx-0xc],0x0           ;   0x12fc RASTERMODE = 0
   3dd78:  mov  DWORD PTR [rcx-0x8],r13d          ;   0x1220 SURF0_BASE = base[0]
   3dd7f:  mov  DWORD PTR [rcx-0x4],r12d          ;   0x1224 SURF0_CFG  = fmt-forced cfg[0]
```

(`eax` is the two 16-bit halves `[rsp+0xc] = 1`, `[rsp+0xe] = 1` set at
0x3dcdc/0x3dce3 — a **1×1 scissor**, i.e. the priming draw is deliberately
scissored to nothing.)

Capture, priming packet: `0x1300=005000f5 0x1328=00000000 0x132c=00010001
0x12c8=00000000 0x12e4=00000001 0x12fc=00000000 0x1220=00000000
0x1224=32020000` — identical. This also shows the *null* descriptor arriving
straight from a zeroed shadow: `base[0] = 0` and
`cfg[0] = (0x00020000 & 0xc0ffffff) | 0x32000000 = 0x32020000`, pitch 0 because
the shadow's geometry is zero. [PROVEN]

The 2D twin, `mtx1331` @0x3e585, flushes the whole shared engine state in one
17-dword block and fixes the shadow layout exactly:

```
   3e59f: [rax]      = 0xf                        ; 16 payload dwords
   3e5a5: [rax+0x04] = 0x8d8c8988                 ; 0x1220 <- dev[0xb28]  0x1224 <- dev[0xb38]
                                                  ; 0x1230 <- dev[0xb30]  0x1234 <- dev[0xb40]
   3e5cd: [rax+0x18] = 0x21c5c6c9                 ; 0x1324 <- dev[0xb0c]
                                                  ; 0x1318 <- dev[0xb14]  0x1314 <- dev[0xb18]
   3e5ef: [rax+0x28] = 0xe3c1c2c0                 ; 0x1300 <- dev[0xb08]  0x1308 <- dev[0xb1c]
                                                  ; 0x1304 <- dev[0xb20]  0x138c <- dev[0xb24]
   3e62c: [rax+0x3c] = 0x212121dd                 ; 0x1374 STROBE
```

so `base[i] = dev + 0xb28 + 4i`, `cfg[i] = dev + 0xb38 + 4i`, DWGCTL at
`dev+0xb08`, PLNWT at `dev+0xb0c`. [PROVEN]

### 2.5 The **type** field, and the two different pitches [PROVEN]

`bits[18:16]` is a 3-bit layout enum with six defined values. The X driver's
`MmSurfDesc` accessors name them by use, and MTXPARD's helpers give the
arithmetic:

| type | set by | meaning |
|---|---|---|
| 0 | cursor / staging buffers (`0x7f4a8`, `0x52036` mask `&0xf8`) | plain linear buffer; byte size taken verbatim from `desc[0x0c]` |
| 1 | textures | pow2-pitch 1-D/texture class, height granularity 2 |
| 2 | forced by the 2D bind path (`0x7bad8` `or 0x20000` after `and 0xc0faffff`) | **2-D drawing destination** |
| 3 | `InitPrimarySurface` @0x7f242 (`or r10d,0x3`) and `Surface2d::AllocTmp` | **primary / displayable 2-D** |
| 4, 5 | — | further tiled variants; 5 is the 4-bpp compressed class |

**There are two distinct pitches, and conflating them is a trap.**

* the **physical footprint** uses `roundUp(width, 32)`:
  `GetPhysicalMemSizeREVB` @0x74490 =
  `roundUp(w,32) * roundUp(h,hgran) * bpp/8 * depth`, rounded to the alignment.
  `GetWidthGranularityREVB` @0x74350 returns a constant **32**.
* the **host-aperture pitch** — the pitch of the *CPU-visible, de-tiled* window
  onto the surface — uses a **power of two** for types 2..5:
  `GetHostAperturePitch` @0x745c0 = `pow2ceil(max(width,32)) * bpp/8` for types
  2–5, `roundUp(width,32) * bpp/8` for types 0/1. MTXPARD's `0xa40f0` computes
  the same split (`a412f` round-32 for types 0/1/2, `a4152`
  `pow2(max(pitch,32))` for types 3/4/5 — the two binaries disagree on which
  side type 2 falls, so treat that one boundary as [INFERRED]).

So for the captured `0x30030780` (type 3, 1920 px, 32 bpp) the allocation is
1920 px wide but the **CPU window onto it is 2048 px = 8192 bytes per row**.
A type ≥ 2 surface is therefore **not** guaranteed to be plainly linear in
VRAM: the driver goes to the trouble of maintaining a separate de-tiling
aperture with a different pitch precisely because the raw layout is not the
one the pitch field advertises. Reading such a surface directly out of the
framebuffer BAR at `base + y*width*bpp/8` can legitimately produce garbage —
or, if the read lands outside the populated part of the tile pattern, zeros.

Read-modify-write masks in MTXPARD pin the field boundaries exactly [PROVEN]:

| site | mask / or | what it replaces |
|---|---|---|
| `0x8d6d9` | `and 0xc0faffff` then `or 0x20000 \| (fmt&0x3f)<<24` | type ← 2, format ← var; bits 30/31 preserved |
| `0x9761d` | `and 0xd0f8ffff` / `or 0x10000000` | type ← 0, format ← 0x10; pitch preserved |
| `0x975b9` | `and 0xd0f9c040` / `or 0x10010040` | type ← 1, format ← 0x10, pitch ← 64 |
| `0x9892a` | `and 0xf2faffff` / `or 0x32020000` | type ← 2, format ← 0x32 |
| `0xa2b7a` | `edx = (cfg ^ width) & 0x3fff ; cfg ^= edx` | **pitch[13:0] ← width in pixels** |
| `[XDRV] 0x7bad8`–`0x7bae5` | `or 0x20000` ; `and 0xc0faffff` ; `or (fmt&0x3f)<<24` | the 2D bind forces type 2 and re-stamps the format |

Bits [31:30] and [23:19] are preserved by every one of those masks. Bits
[31:30] *are* written by `Surface2d::Enable` (§2.6), and **bit 23 is proven**:
`Surface2d::SwapSurfMode(bool)` @0x7c2d0 toggles exactly `0x00800000` in both
the descriptor and the alloc-option copy, and its caller is
`TempBit::SetSurfModeTiled` @0x7e520 — so **cfg bit 23 = linear / tiled
("block") mode**, not byte swap. Every cfg in our capture has it clear.
Bits [15:14] remain preserved and unidentified.

**Two captured cfg words are now positively identified**: `0x05000020`
(1 bpp, type 0, pitch 32) is the **hardware cursor mask** — `HardPointerBuffer`
@0x7f3b0 builds exactly that (`purpose = 0x12`, memClass 1, format code 4/5,
pitch `(1+31)&0x3fe0 = 0x20`); and `0x30030780` is the **primary/displayable**
surface, matching the DC FORMAT register value `0x32030780` recorded in
`parhelia-visible-framebuffer` (same encoding, same type 3).

### 2.6 The base register: `{engine byte address | memory class}` [PROVEN]

MTXPARD's 2D blit builds the destination base as a **sum of two fields of the
surface object** (`0x10dd42`):

```
10dd42:  mov edx,[ecx+0x37c]        ; a per-surface base / aperture origin
10dd4e:  add edx,[ecx+0xa0]         ; + the offset of this surface within it
10dd5a:  mov [ebp+0xaa4],edx        ;   -> 0x1220
10dd54:  mov eax,[ecx+0xa4]
10dd60:  mov [ebp+0xab4],eax        ;   -> 0x1224
```

and the Linux X driver writes a base with the low bits **set**:

```
59098:  mov r15d,DWORD PTR [rbx+0xa8]
5909f:  or  r15d,0x3               ; <-- memory-space selector in bits[1:0]
5912d:  mov esi,0x1220             ; then memWriteDword
```

for a surface it subsequently blits *from local VRAM into*, i.e. that surface
is **non-local**. So the register value carries a space selector in bits[1:0]
(0 = local, 3 = non-local/AGP), and the rest is `base + offset` in whichever
space that selects. **The captured GL colour target `0x02000000` has
bits[1:0] = 0 → local VRAM**, and the addresses the same stream uses for
system memory (ring `0x14600000`, fence page `0x18a2a000`, WARP heap
`0x076acd03 → bus 0x1663…`, vertex buffer `0x16a5…`) all sit in
`0x14000000..0x18000000` — a cleanly separated region. Two spaces, split high
up in the address; `0x02000000` is in the local one.

A third, independent corroboration of the class field comes from a *different*
register that uses the same encoding: at the draw `0x1000 VTXBASE0 = 0x00000003`
— base 0 with **class 3 = PCI/system memory**, which is exactly where the
vertex buffer lives (the draw operands point at `0x16a2…` in the sysmem band).
So `{address | class}` in the low bits is the general form for engine address
registers, and the colour target's class 0 means local VRAM. See also
`docs/parhelia-span-decoded-complete.md`, which reaches the same reading from a
full dword-by-dword decode of the same span.

**The two spaces do not conflict with each other.** The ring pointer registers
`0x400`/`0x404`/`0x408` take a *bus* address — the vendor's ring sits at
`0x14600000`, the vertex buffer at `0x16a2…`, the fence page at `0x18a2a000`,
all in a `0x14000000..0x18000000` band — while the surface base register takes
a VRAM offset in a separate, low space. They are different registers with
different address spaces; a value in one must never be compared with a value in
the other. The decisive calibration point is our own 2D fill instrument: it
draws **on screen** with base 0, and the screen is VRAM offset 0
(`parhelia-visible-framebuffer`), so base 0 = VRAM byte 0 and the surface base
really is a VRAM byte offset for a local surface. [PROVEN, one binary + one
hardware result]

Two small corroborations that these values are *allocator* addresses in a
byte-addressed space: every base observed in the capture
(`0x02000000`, `0x01000000`, `0x018e8000`, `0x018ea000`, `0x09000000`,
`0x09002000`) is **2048-byte aligned**, which is exactly the type-2/type-3
32-bpp alignment requirement from `0xa3c90` (§2.5); and the two colour buffers
are 16 MB apart, the power-of-two round-up of a 1920×1200×4 = 9.2 MB surface.

**There is no MMU, page table or aperture that could relocate a surface base.**
An independent pass over the same kernel object
(`docs/parhelia-engine-context-bringup.md` §2.1–§2.4) establishes the complete
translation and finds nothing else: `buf->[0x34]` is the 2-bit class OR'd into
the low bits (**0 = local/VRAM, 2 = AGP, 3 = PCI/system**), `buf->[0x58]` is a
`busAddress − hostVirtualAddress` delta applied only to classes 2/3, and the
`0x0000..0x013f` descriptor block is programmed once at HAL init and *never
consulted when a surface address is computed*. The MTXPARD expression above is
software-side bookkeeping — `[ecx+0x37c]` is the allocation's base within VRAM
and `[ecx+0xa0]` the offset inside it; their sum is still a VRAM byte offset.

And the X driver's `Surface2d::Enable(MmSurfaceManager*)` — found at 0x7bf00
via its PLT thunk at 0x37cc0 — computes the register value in two
instructions, with no arithmetic in between:

```
   7bf35:  mov  esi,DWORD PTR [rbx+0x90]     ; the surface's address field
   7bf3b:  mov  DWORD PTR [rbx+0xe0],esi     ; -> the value that goes to 0x1220+8s
   7bf65:  call QWORD PTR [rbp+0x20]         ; a device capability query
   7bf68:  test al,0x8
   7bf6c:  or   DWORD PTR [rbx+0xe0],0x3     ; -> class 3 (PCI/system) if capable
```

**No shift, no aperture base added, no page-table index.** The value is the
address field with the memory class ORed into bits[1:0], exactly as the kernel
pass concluded. [PROVEN, three independent binaries]

The same function is also the only place found that programs the top of the
cfg word:

```
   7bfa8:  movzx ecx,BYTE PTR [rbx+0xc9]
   7bfaf:  and   ecx,0x3f
   7bfb2:  or    ecx,0xffffff80              ; byte3 bits[7:6] <- 0b10
   7bfb5:  mov   BYTE PTR [rbx+0xc9],cl
```

`[rbx+0xc9]` is byte 3 of the cfg word, so this sets **cfg bits[31:30] = 0b10**
when `dev->[0xc9ba] & 1`. That confirms bits[31:30] is a real, driver-programmed
2-bit field (§2.2) — and every cfg in our capture has it **0b00**
(`0x32`/`0x30`/`0x36` all have byte-3 bits[7:6] = 00), i.e. the captured
surfaces are all in the field's default/linear state. [PROVEN]

**So `0x1220 = 0x02000000` is a plain byte address in the engine's own address
space, with class 0 = local.** [PROVEN, three independent binaries]

### 2.6b The engine address space is **banked**, and it is not the BAR [PROVEN]

This is the part that was missing, and it is the most likely reason a hardware
read of "VRAM 0x02000000" came back empty.

**The layout.** `EmulMmSurfaceManager`'s constructor @0x76870 builds the heaps
from literals:

```
   ecx   = *(u32*)commonData[0x2088] >> 1        ; half the VRAM size
   bankA = { start 0,           end ecx            }
   bankB = { start 0x08000000,  end 0x08000000+ecx }
   CreateHeap(bankA, mgr+0x58, 0x100000, fromTop=1)   ; 1 MB at the TOP of bank A
   CreateHeap(bankA, mgr+0x40, rest,     fromTop=0)   ; the rest, from 0 upward
   CreateHeap(bankB, mgr+0x70, 0x400000, fromTop=0)   ; 4 MB at 0x08000000
   CreateHeap(bankB, mgr+0x88, 0x100000, fromTop=1)
   CreateHeap(bankB, mgr+0xa0, rest,     fromTop=0)
```

and the kernel agrees — `mtx1876` @0x56290 builds the same shape from
`commonData[0xb8]`/`[0xbc]`:

```
   56292:  mov  r14d,0x8000000
   562b7:  mov  DWORD PTR [rdi],0x8000000       ; bank B base
   564e8:  cmp  r11d,0x8000000                  ; 128 MB per bank ceiling
```

with `mtx3368` @0x5cea7 special-casing `bankAend == 0x8000000 && total ==
0x10000000` and adding `0x8000000` to a bank-B surface address. So:

```
   engine 0x00000000 .. 0x07ffffff   bank A   (VRAM/2)
   engine 0x08000000 .. 0x0fffffff   bank B   (VRAM/2)
   engine 0x10000000 ..              host aperture window, 256 KB granularity
```

`EmulMmSurfaceMapper::AllocateApertureBlock` @0x74c40 nails the last line:
`add eax,0x7ffff ; and eax,0xfffc0000` (256 KB granularity),
`or ecx,0x10000000`, `shl r14d,0x4` (16-byte descriptors). That reproduces the
live MMIO trace exactly — 16-byte entries at `0x0000`, entry 0 base
`0x10000000`, entry 1 base `0x10040000` = +256 KB. **So the `0x0000..0x013c`
block is a 20-entry host-aperture descriptor table, and `0x10000000` is the
base of the aperture space, not of VRAM.** `MmSurface::CanUseVertexHostAperture`
@0x736c0 range-checks a surface's engine address against that window, which
proves the engine and the aperture share one flat byte-addressed space.
[PROVEN]

**And the engine address is NOT the framebuffer-BAR offset.**
`MmSurfaceManager::AllocateSurfaceBlock` @0x76590 receives *three* values from
the kernel and keeps them apart:

```
   7660f:  [block+0x20] = out0     ; the ENGINE byte address  -> goes to 0x1220
   76612:  [block+0x08] = out1     ; size
   76619:  [block+0x10] = out2     ; the FRAMEBUFFER-APERTURE offset
   class 2|3 :  block[0x18] = OsMapPhysicalMemory(out0)   ; out0 is a CPU phys addr
   class 6|7 :  block[0x18] = mgr[0x10] + out2            ; FB base + FB offset
   class 0|1 :  block[0x18] = 0                           ; not CPU-mapped at all
```

(the ioctl is `0xc0804618`, kernel handler `mtx1545` @0x12a0, which returns
`out[0x00] = kblock[0x40]` and `out[0x0c] = kblock[0x30]`.)

**`out0` and `out2` are different numbers.** Reading the framebuffer BAR at the
*engine* address is only correct when the two happen to coincide. For a 256 MB
part with `bankAend == 0x8000000` they plausibly do for bank A; for any other
split, or for a bank-B surface, they do not. This — together with the
host-aperture pitch of §2.5 — is the most economical explanation of "VRAM
`0x02000000` reads as zeros": **the number in `0x1220` was never a BAR
offset.** [PROVEN that the two are distinct; [INFERRED] that this is what the
measurement hit.]

That makes the hardware null result a statement about *which register* is the
colour destination, not about the encoding. Two candidates remain, in the order
the evidence supports them: the destination is a different slot (§3.3), or the
read window was too narrow (§6.1). §6 separates them in a handful of runs.

### 2.7 The calibration point: the primary surface's base register value is 0 [PROVEN]

`InitPrimarySurface` @0x7f1a0 fills the primary's descriptor and its allocation
options:

```
   7f238:  desc.word[0xc4] = (x & 0xc000) | ((width + 31) & 0x3fe0)   ; pitch
   7f23f:  desc.byte[0xc7] = (x & 0xc0) | (pdev[0x114] & 0x3f)        ; format code
   7f242:  or  r10d,0x3        -> desc.byte[0xc6] |= 3                ; type = 3
   7f2c6:  mov DWORD PTR [rbx+0x0c],0x0                               ; memClass = 0 (LOCAL)
   7f2d0:  mov DWORD PTR [rbx+0x08],0x1                               ; purpose  = 1
```

`memClass 0` takes the `test dil,0xc3 -> sel = 0` branch of
`MmSurface::AttachToBlock` @0x7377f, so **no selector bits are ORed in**; and
`purpose 1` selects, through the jump table at 0x110e8c
(`0→mgr+0x88, 1→mgr+0x40, 18→mgr+0x58, 21→mgr+0x70, 22→mgr+0xa0`), the **bank-A
main heap, which starts at 0 and allocates upward** (§2.6b) — the 1 MB reserved
for the cursor comes off the *top* of bank A.

**Therefore the screen surface's base register value is 0**, which matches the
hardware fact that the visible framebuffer is VRAM offset 0 and is scanned out
from CRTC `0x1824` (`parhelia-visible-framebuffer`). That is the anchor an open
driver can calibrate against, and it is the basis of Test 0b in §6.1.

---

## 3. DWGCTL `0x1300` (Q2)

### 3.1 How the vendor builds the 3D form [PROVEN]

`mtx3712` @0x188f0 is the kernel's per-draw state validation function. It
builds DWGCTL twice (two paths through the same code, @0x18bb4 and @0x18f32)
and the assembly is unambiguous:

```
18be5:  mov    rax,QWORD PTR [r14+0x1c]
18be9:  movzx  edx,BYTE PTR [rax+0x3]
18bed:  shr    dl,1
18bef:  and    edx,0xf                  ; -> bits [3:0]
18bf6:  movzx  eax,BYTE PTR [rax+0x3]
18bfa:  shr    al,0x5
18bfd:  and    eax,0x1
18c00:  shl    eax,0x16                 ; -> bit 22
18c09:  mov    rcx,QWORD PTR [r14+0x14]
18c0d:  movzx  edx,BYTE PTR [rcx+0x5c]
18c11:  and    edx,0x20
18c14:  cmp    dl,0x1
18c17:  sbb    edx,edx
18c19:  not    edx
18c1b:  and    edx,0x80000              ; -> bit 19  = (cfgbyte & 0x20)
18c23:  cmp    esi,0x2                  ; esi = virtual call [pDev...+0x4c0]->[0x20]
18c34:  or     edx,0x200000             ; -> bit 21  (esi != 0, != 2)
  ...   (esi == 2 path @0x19468: bit 21 = (cfgbyte & 0x40))
18c3e:  mov    rax,QWORD PTR [r14+0xc]
18c46:  or     edx,DWORD PTR [rax+0x18] ; -> the ROP/BOP contribution
18c49:  or     edx,0x860e00             ; -> the fixed 3D constant
18c57:  cmp    DWORD PTR [rax+0xb08],edx ; DWGCTL shadow, dirty bit 0x01
```

MTXPARD assembles the identical expression (`pard.asm` 0xab910–0xab967), so two
independent binaries agree.

MTXPARD assembles the identical expression at `0xab910`, and the two binaries
agree instruction for instruction:

```
   ab923:  and esi,0x20                ; F & 0x20
   ab926:  and eax,0x40                ; F & 0x40
   ab929:  shl eax,1
   ab92b:  or  eax,esi
   ab92f:  shr esi,0x12
   ab932:  and esi,0x780               ; (W >> 25) & 0xf, pre-shifted
   ab938:  and edx,0x20000000          ; W bit 29
   ab943:  or  esi,0x43070000          ; 0x43070000 >> 7 == 0x00860e00
   ab949:  shr esi,7
   ab94c:  shl eax,0xe                 ; -> bit 19 and bit 21
   ab957:  or  eax,<rop shadow>
```

`(F&0x20)<<14 = 0x80000` = bit 19 and `((F&0x40)<<1)<<14 = 0x200000` = bit 21,
exactly as `mtx3712` computes them; and the low nibble is `(W >> 25) & 0xf` in
both. [PROVEN, two independent binaries]

### 3.2 Decoding `0x00a60ec4`

```
0x00a60ec4 = 0x00860e00        fixed 3D constant  (bits 23, 18, 17, 11, 10, 9)
           | 0x00200000        bit 21
           | 0x000000c0        BOP = 0xC (GXcopy), from the ROP shadow
           | 0x00000004        opcode 4
```

| bits | value | meaning |
|---|---|---|
| [3:0] | 4 | **primitive opcode 4 = 3D/WARP primitive**. The same 4 appears in the GL depth-clear `DWGCTL = 0x000000C4`, whose destination is the *Z* surface — so the nibble is the operation class, not a surface property. [PROVEN by two independent uses] |
| [7:4] | 0xC | BOP = GXcopy, from `[r14+0xc]+0x18` |
| [9:8] | 0b10 | part of the fixed `0x860e00` — **not** a source-slot number in the 3D form |
| [11:10] | 0b11 | part of the fixed `0x860e00` — **not** a destination-slot number in the 3D form |
| [18:17] | 0b11 | part of the fixed `0x860e00` |
| 19 | 0 | `= (ctxcfg & 0x20)` |
| 21 | 1 | `= (ctxcfg & 0x40)` on the `esi == 2` path, else set unconditionally |
| 22 | 0 | `= (obj1c.byte3 >> 5) & 1` |
| 23 | 1 | clip / scissor enable (shared with the 2D map) |

### 3.3 Which surface slot does a 3D draw write? — the one genuinely open question

Two readings survive the evidence, and they predict different addresses. Both
are set out here because the difference is exactly what the hardware
measurement is failing on, and §6 gives the four-run experiment that settles it.

#### Reading A — bits [11:10] ARE the destination slot, so 3D writes **slot 3**

MTXPARD gives strong, quantitative support [PROVEN as a correlation]:

* the surface→slot binder at `0x8d530` stores the per-slot constants
  `0x000 / 0x400 / 0x800 / 0xc00` — literally `slot << 10` — into the
  descriptor object, and the X driver's `SurfDesc2d::Init` @0x7bc80 does the
  same thing independently:

```
   7bca6:  mov DWORD PTR [rdi+0x10],0x0      ; slot 0 ; tag 0x21218988
   7bcb5:  mov DWORD PTR [rdi+0x10],0x400    ; slot 1 ; tag 0x21218b8a
   7bcc4:  mov DWORD PTR [rdi+0x10],0x800    ; slot 2 ; tag 0x21218d8c
   7bc97:  mov DWORD PTR [rdi+0x10],0xc00    ; slot 3 ; tag 0x21218f8e
```

* six independent MTXPARD call sites correlate the DWGCTL slot bits with the
  surface slots programmed in the *same* packet, with no counterexample:

| DWGCTL | site | dst/src by [11:10]/[9:8] | slots programmed together |
|---|---|---|---|
| `0x001000f4` | `0x2e832` | 0 / 0 | slot 0 only (`0x8988bfb9`) |
| `…c2 \| 0x200` | `0x10da10` | 0 / 2 | slots 0 + 2 (`0x8d8c8988`) |
| `0x00000bc2` | `0x976fe` | 2 / 3 | slots 2 + 3 (`0x8f8e8d8c`) |
| `0x00800be2` | `0x475cd` | 2 / 3 | slots 2 + 3 (`0x8f8e8d8c`) |
| `0x0030000c` | `0x2cac2` | 0 / 0 | slot 0 only |
| `0x002000c1` | `0xd5c83` | 0 / 0 | slot 0 only |

  The opposite assignment (bits[9:8] = dst) is *excluded* by row 2, where the
  blit's destination is provably slot 0 while only bit 9 is set.

Under Reading A, `0x00a60ec4` means **dst = slot 3, src = slot 2**, and a 3D
draw is retargeted by reprogramming `0x1238`/`0x123c`, not by editing DWGCTL.
It also immediately explains the hardware null result: in the captured span
slot 3 holds `{base 0, cfg 0x32020000}`, and a zero-stride, zero-base surface
puts every row of the triangle on top of VRAM row 0.

#### Reading B — for the 3D opcode the slot bits are part of a fixed constant


1. `0x00860e00` is ORed in **unconditionally** by `mtx3712` (`or edx,0x860e00`)
   and by MTXPARD. Bits 9, 10 and 11 are therefore constant across every 3D
   draw, whatever surface is bound. A field that never varies cannot select
   anything.
2. Read as a 2D slot pair, `0xe00` says "dst = slot 3". In the capture slot 3 is
   `{base 0, cfg 0x32020000}` — the *null* descriptor of §2.3. A draw into a
   pitch-0, base-0 surface would not produce the frame that `gltri` visibly
   renders.
3. The GL depth clear uses `DWGCTL = 0x000000C4` — bits [11:10] = 0, i.e.
   "slot 0" — yet it clears the surface bound in **slot 2**
   (`docs/parhelia-rendertarget.md` §2.3, `mtx2187` @0x3ccb0). Again the slot
   bits do not name the surface that gets written.

Under Reading B the 3D slot roles are *fixed by the pipe*, not selected by
DWGCTL:

* **slot 0 = colour render target**, `0x1220` / `0x1224`;
* **slot 2 = depth/stencil**, `0x1230` / `0x1234`;
* slot 1 = the drawable's other colour buffer (used by the 2D swap/compose);
* slot 3 = unused by 3D.

Independent support — **the GL surface-bind emitter, found and read in full**.
`mtx2954` @0xf8e0 emits exactly the two packets the capture shows at ring[691]
and ring[696], in that order:

```
    f96c:  mov  DWORD PTR [rcx],0x4              ; payload dwords - 1
    f972:  mov  DWORD PTR [rcx+0x4],0x8d8c8988   ; idx 0x88,0x89,0x8c,0x8d
    f97c:  mov  eax,DWORD PTR [r12+0x144]        ; base[0]
    f988:  mov  DWORD PTR [rcx+0x8],eax          ;   -> 0x1220
    f98b:  mov  eax,DWORD PTR [r12+0x154]        ; cfg[0]
    f993:  mov  DWORD PTR [rcx+0xc],eax          ;   -> 0x1224
    f996:  mov  eax,DWORD PTR [r12+0x14c]        ; base[2]
    f99e:  mov  DWORD PTR [rcx+0x10],eax         ;   -> 0x1230
    f9a1:  mov  eax,DWORD PTR [r12+0x15c]        ; cfg[2]
    f9a9:  mov  DWORD PTR [rcx+0x14],eax         ;   -> 0x1234
    f9bd:  mov  DWORD PTR [rcx],0x2
    f9c3:  mov  DWORD PTR [rcx+0x4],0x21218b8a   ; idx 0x8a,0x8b
    f9ce:  mov  eax,DWORD PTR [r12+0x148]        ; base[1] -> 0x1228
    f9df:  mov  eax,DWORD PTR [r12+0x158]        ; cfg[1]  -> 0x122c
```

(the per-operation record is filled by `mtx2372` @0xcc30 as
`rec[0x144 + 4i] = base[i]`, `rec[0x154 + 4i] = cfg[i]`.)

**`mtx2954` never emits slot 3.** The Linux 3D/GL path binds slots 0, 2 and 1
and nothing else, which is why slot 3 keeps the null descriptor of §2.3 across
the whole capture. [PROVEN]

#### Weighing them

Reading A's six witnesses are **all 2D opcodes** (0 fill, 2 blit, 0xc textured
rect). The only 3D witness is the value in dispute. Reading B's witnesses are
the two 3D paths themselves. Against Reading B: the hardware says the pixels
are not at slot 0's base either. Against Reading A: the vendor's own frame
renders correctly while slot 3 is null, which a zero-stride destination cannot
explain.

A third possibility neither reading covers is that the low nibble (opcode 4)
switches the meaning of bits [11:10] entirely — note that the GL **depth
clear** uses `DWGCTL = 0x000000C4`, bits [11:10] = 0 = "slot 0", yet it clears
the surface bound in **slot 2** (`mtx2187` @0x3ccb0,
`docs/parhelia-rendertarget.md` §2.3). That is a same-opcode counterexample to
Reading A inside the 3D opcode, and it is the reason this section does not pick
a winner from static analysis alone.

**Settle it with four runs (Test B in §6): move one slot's base to a scratch
address at a time and see which one moves the pixels.**

> **What `W` is.** Both binaries compute the low nibble as `(W >> 25) & 0xf`
> and bit 22 as `W & 0x20000000`, where `W` is a dword read through a state
> pointer. `docs/parhelia-pixel-pipeline-complete.md` §3.8 identifies `W` with
> the render-target *cfg* word. It cannot be: with the captured colour cfg
> `0x32020780` the nibble would be 9, and the capture shows **4**. Running the
> formula backwards, the observed nibble 4 with bit 22 clear forces
> `W` bits[28:25] = `0b0100` and `W` bit 29 = 0, i.e. `W`'s 6-bit field at
> [29:24] is `0x08` or `0x09` — a 4-bpp or 8-bpp format code, not a 32-bpp one.
> So `W` is a *different* descriptor (or a mode word that merely shares the
> shape). Do **not** compute the DWGCTL nibble from the colour target; emit 4,
> which is also what the GL depth clear emits (`0x000000C4`).
> [PROVEN that the published formula does not reproduce the capture]

---

## 4. Everything that can suppress all fragments (Q3)

For each entry: what the register does, and what the **captured value** means.
All captured values are the state that was live when the vendor's own frame
rendered correctly, so "captured = harmless" is itself evidence.

| # | register | field | kill condition | captured value | verdict |
|---|---|---|---|---|---|
| 1 | `0x1324` PLNWT | 32-bit bit-plane write mask | any zero plane masks that bit; `0x00000000` kills every pixel | `0xffffffff` | **harmless** |
| 1b | `0x1318` | see §4.1 — write mask or background colour | if it is the mask, `0` kills every pixel | `0x00000000` | **resolved harmless, see §4.1** |
| 2 | `0x1328`/`0x132c` | scissor min/max, `(y<<16)\|x` | primitive entirely outside the rectangle; `0x132c = 0` kills everything | min `0`, max `0x00f00140` = 320×240 | **live and tight** — see §6.2 |
| 3 | `0x1300` bit 23 | clip/scissor enable | set ⇒ the scissor above is enforced | set (part of `0x860e00`) | scissor is ON |
| 4 | `0x12c8` bit 0 | depth-test enable | enabled with a wrong/absent Z surface ⇒ all fragments fail | `0x001000e4`, bit 0 = **0** | depth test OFF |
| 5 | `0x12c8` bits [3:1] | depth compare func | func = NEVER (0) kills all | 2 (`0xe4>>1 & 7`) | irrelevant, test off |
| 6 | `0x12c8` bit 4 | stencil-test enable | enabled + failing func/masks ⇒ all killed | 0 | stencil OFF |
| 7 | `0x12c8` bits [7:5] | stencil func | NEVER kills all | 7 | irrelevant |
| 8 | `0x12cc`/`0x12d0` | stencil compare / write masks | `0x12d0 = 0` = no stencil writes | `0xff` / `0xff` | harmless |
| 9 | `0x12e4` bit 0 | alpha-test enable | enabled with func NEVER, or ref above every alpha | `0x0000000e`, bit 0 = **0** | alpha test OFF |
| 10 | `0x12e4` bits [3:1] | alpha func | 0 = NEVER | 7 = ALWAYS | harmless |
| 11 | `0x12e4` bits [15:8] | alpha ref | — | 0 | harmless |
| 12 | `0x12e8` bit 0 | blend enable | src=ZERO,dst=ONE ⇒ dest unchanged (looks like "no fragments") | `0x00800202`, bit 0 = **0** | blend OFF |
| 13 | `0x1130` bit 0 | cull enable | cull-all with the wrong winding ⇒ every triangle discarded | `0x0200200c`, bit 0 = 0 | cull OFF |
| 14 | `0x12fc` bits [1:0] | cull select (`and eax,0xfffffffc` clears it) | selects the culled face | `0x0bb80024`, bits[1:0] = 0 | cull OFF |
| 15 | `0x12f4` guard band | clip volume | a small guard band clips large primitives | `0x42000000` = 32.0f | generous |
| 16 | `0x12c0`/`0x12c4` | z-clip min/max | an inverted or empty range clips all | `0x80000000` / `0x7fffffff` = full range | harmless |
| 17 | `0x1224` bits [13:0] | RT pitch | pitch 0 ⇒ every pixel of every row aliases row 0 | `0x780` = 1920 | correct |
| 18 | `0x10b0` PRIMWORD | primitive type + topology | a wrong primitive class emits nothing | `0x0006305c` = triangles | correct |
| 19 | `0x1060` VTXCOUNT | vertex count | 0 ⇒ nothing | `0x000280cc` | non-zero |
| 20 | `0x1100` / `0x10c0..0x10d8` | WARP program entries | an entry pointing at a stale/foreign heap ⇒ garbage or no output | `0x07d57783` etc. — the vendor's **live** heap | boot- and context-specific, see §6.3 |

Registers that are *not* fragment gates, despite looking like candidates:

* **`0x12b0` = `0x3fff8842`** is the vendor tunable **`MemLevels`** (group
  `3D`). [PROVEN] `mtx722` @0x7653f constructs a parameter object with
  `mov r9d,0x3fff8842` and the name string `.rodata.str1.1+0x161` = `MemLevels`
  (a second table site @0x781df carries the same default). It is a memory/FIFO
  watermark word, not a raster enable.
* **`0x12f8` = `0x007fe000`** is the tunable **`ThresholdAA`** (default
  `0x7fffff`). [PROVEN — same parameter-table mechanism.]
* **`0x0bc0` = `0x4087`** (direct MMIO in the arm sequence) is the tunable
  **`TEDLFCounters`**. [PROVEN — `mtx722` @0x764aa, name string `+0x28c`.]
* **`0x1374`** is a strobe with an ignored payload; **`0x121c`** is a latch the
  kernel writes 0 to before the texture-unit block.

### 4.1 `0x1318` versus `0x1324` — which is the colour write mask [RESOLVED by the capture]

Two binaries appear to disagree, and the capture settles it.

MTXPARD's 2D fill builder writes what is unmistakably a *mask* into the shadow
field that maps to `0x1318`:

```
   10dc98:  mov DWORD PTR [ebp+0xa90],0xffffffff   ; 8 bpp
   10dcd3:  mov DWORD PTR [ebp+0xa90],0xffffffff   ; 16 bpp
   10dcf5:  mov DWORD PTR [ebp+0xa90],0x3fffffff   ; 32 bpp, format 0x10 (2:10:10:10)
   10dd07:  mov DWORD PTR [ebp+0xa90],0x00ffffff   ; otherwise (24-bit RGB)
   10dd17:  mov DWORD PTR [ebp+0xa94],edx          ; the colour, ANDed with the same value
```

and `+0xa90` → `0x1318`, `+0xa94` → `0x1314` (the Linux kernel's `mtx1331`
@0x3e5cd emits the same three from `dev[0xb0c]`/`dev[0xb14]`/`dev[0xb18]` under
tag `0x21c5c6c9`). That makes `0x1318` look like a per-format write mask.

**The capture refutes that for the 3D path.** At the draw, `0x1318 = 0x00000000`
(last written at ring[433]) while `0x1324 = 0xffffffff` (ring[701]) — and the
vendor's own frame renders correctly. A zero colour write mask cannot be live
while pixels are being produced. So:

* `0x1324` is the plane write mask that gates fragments (and the GL clear
  force-overrides it to `0xffffffff`, `mtx2187` @0x3ccb0);
* `0x1318` is the **background/second colour** of the 2D operation, which the
  2D path happens to load with a format-shaped constant so that the fill colour
  and the background agree on the meaningful bits.

[PROVEN by the capture: `0x1318 = 0` across a correctly rendering frame.]

### 4.2 `0x12fc` RASTERMODE — the default value is now proven

`mtx1753` @0x408e0 and `mtx611` @0x40940 build the reset value byte for byte:

```
408e0:  mov  DWORD PTR [rdi+0x20],0x0
408e7:  or   BYTE PTR [rdi+0x20],0x14
408eb:  mov  eax,DWORD PTR [rdi+0x20]
4090a:  and  eax,0x3fff
40916:  or   eax,0x18004000
40929:  mov  DWORD PTR [rdi+0x20],eax        ; = 0x18004014
4092c:  movzx eax,WORD PTR [rdi+0x26]
40930:  and  ax,0x807f
40934:  or   ax,0x280
```

`0x18004014` is exactly the value the capture shows during engine priming, so
shadow offset `+0x20` **is** `0x12fc`. [PROVEN] The word is therefore split as
`bits[13:0]` = raster flags, `bits[31:14]` = a separate field (priming
`0x18004000`, per-draw `0x0bb80000`). The per-draw flags are `0x24` (bits 2 and
5) against the priming `0x14` (bits 2 and 4); bits [1:0] = cull select are 0 in
both. Replay `0x0bb80024` verbatim. [PROVEN for the layout, [INFERRED] for the
individual flag meanings.]

### 4.3 `0x1130` WARP_PIPECTL — the `| 0x200` is conditional [PROVEN]

`mtx3400` @0x439c0 is the sole emitter, and it emits `0x1130` on its own:

```
   43a69:  mov  DWORD PTR [rdx-0xc],0x1          ; payload dwords - 1
   43a70:  mov  DWORD PTR [rdx-0x8],0x2121214c   ; idx 0x4c = 0x1130
   43a77:  or   r12d,DWORD PTR [rbx+0x5f0]       ; the shadow
   43a81:  mov  DWORD PTR [rdx-0x4],r12d
   43a85:  and  BYTE PTR [rax+0x2],0xbf          ; clear dirty bit 0x40
```

`r12d` is 0 unless a device-flag branch is taken, and only that branch sets it:

```
   4433c:  or   edx,0x1
   4433f:  mov  r12w,0x200
```

`docs/parhelia-warp-binding.md` and `parhelia-3d-integrated.md` §8 record the
`| 0x200` as unconditional ("the vendor force-ORs 0x200 on every draw"). It is
**conditional** on `pDev->[0x2190][0xa3114] == 0 && pDev->[0x2190][0xa3121] != 0`,
and on the captured 0528 the branch is *not* taken: the value at the draw is
`0x0200200c`, bits 2, 3, 13 and 25 — **bit 9 is clear**. Emit what the capture
shows, not `| 0x200`. [PROVEN]

### 4.4 `0x1374` (index 0xdd) is a **second pad**, not a strobe [PROVEN]

Proven by omission. `mtx3400` builds several state packets that lead with the
tag `0x212121dd`, and in every one of them the compiler emits stores for every
data slot **except** the one belonging to `0xdd`: the 8-dword packet at
0x43d3e writes `-0x20, -0x1c, -0x14, -0x10, -0xc, -0x8, -0x4` and never
`-0x18`; the 13-dword packet at 0x43ee6 never writes `-0x2c`; the 11-dword one
at 0x43fbb never writes `-0x24`. A compiler does not leave a slot uninitialised
unless the value is discarded. So index `0xdd` behaves like index `0x20`
(`0x1080` DMAPAD): it consumes one data dword which the hardware ignores. Its
practical role is to align the following four-data tag groups.

This matters for a generated stream: **do not treat `0x1374 = 0` as a
meaningful write**, and do not omit its data dword either.

### 4.5 The end-of-batch flush, matched to the capture [PROVEN]

`mtx1619` @0x413ce builds the flush + fence packet:

```
   413e0:  ebx = [chan+0x40+4] + 8          ; monotonic tag, step 8
   41402:  [ring]    = 4
   41408:  [ring+4]  = 0xacafafc0           ; 0x1300, 0x12bc, 0x12bc, 0x12b0
   41412:  [ring+8]  = 0x00000707           ; DWGCTL = flush/sync opcode 7
   4141c:  [ring+c]  = ebx | 1              ; 0x12bc = tag|1  (arm block-until-retire)
   41419:  [ring+10] = ebx                  ; 0x12bc = tag
   41422:  [ring+14] = [rbp+0x58]           ; 0x12b0
```

Capture, the flush packet before the draw: `0x1300=00000707  0x12bc=000001a9  0x12bc=000001a8
0x12b0=3fff8842` — `0x1a8` is the tag, `0x1a9 = tag|1`. Exact match, and it
confirms `parhelia-dwgsync-family`'s "`0x12bc` with `tag|1` arms
block-until-retire" from the emitter side. [PROVEN]

`mtx398` @0x32d19 builds the same shape for the *engine idle* flush and
computes its `0x12b0` value as `((cap ? 0 : 0x1f) << 15) | 0x3ff00000` =
`0x3fff8000` — the same top bits as the `MemLevels` default `0x3fff8842`,
whose low `0x842` is the tunable part.

### 4.6 The parameter namespace (new, and useful)

`.rodata.str1.1` of `mtx_parhelia.o` is a table of vendor tunable names
consumed by `mtx2860(obj, group, name, ?, default)`. Extracting the
`(group, name, default)` triples gives the vendor's own vocabulary for this
hardware — several map straight onto registers we had only as magic numbers:

```
3D  MemLevels        0x3fff8842   -> 0x12b0
3D  ThresholdAA      0x7fffff     -> 0x12f8
3D  TEDLFCounters    0x4087       -> MMIO 0x0bc0
3D  CullNoneMode     0x1
3D  GuardBandClipping 0x2         -> 0x12f4
3D  ClippingType     0x0
3D  ClipReject
3D  TEPipeSelect     0x4          3D TEMultiplePipes 0x7   3D TEEnable
3D  AAMode           0x52         3D FragSize 0x40800000   3D ForcePLNWMASK4FAA
3D  ColRndMode       0x0          3D VECacheMode           3D LightingType 0x1
3D  EnableZinit      3D EnableZCompSundog   3D Use3DClearSundog
3D  ForceDepthToS8Z24Sundog        3D AdjustZ
3D  VertexShader / VertexShaderOptions 0x11   3D VertexBuffers 0x1
3D  TnLZFactor 0x1   3D TrinityEnable   3D PerformCRegCheck
OGL PolygonOffset / WideLines / LineStipple / TextureCompress / FastBitmap
```

`Use2DClearForceZUncompSundog`, `DivByZeroRefRastSundog`, `HqCubemaps`,
`UseTextiled`, `GigaColorRender`, `NonLocalToVideoBlit` are also present.
[PROVEN — strings and defaults read from the object.]

---

## 5. Is there an out-of-span arm? (Q4)

**Short answer: for a same-boot replay through our own ring, no — with two
qualifications.**

### 5.1 The kernel touches almost no MMIO [PROVEN]

Enumerating every `memWriteDword` / `memReadDword` call site in
`mtx_parhelia.o` and recovering the literal offset from the preceding
`mov esi,imm` gives the *complete* set of registers the kernel core writes by
literal MMIO offset:

| offset | R/W | sites | role |
|---|---|---|---|
| `0x0400` | W | `mtx2508` | ring base |
| `0x0404` | W | `mtx2508`; also the doorbell | ring start |
| `0x0408` | W | 25 sites incl. `mtx1937`, `mtx3257` | **the doorbell** (`end \| class`) |
| `0x041c` | R | 14 sites | fence / completion readback |
| `0x0450` | R | 49 sites | engine status |
| `0x0384` | W | 100 sites | **write-posting barrier**, see below |
| `0x0300` | R/W | `mtx3376` | bus/AGP mode (init only) |
| `0x0308` | W | `mtx3376` | bus/AGP mode (init only) |
| `0x08f0`/`0x08f4`/`0x08f8`/`0x08fc` | R/W | ISR `mtx1415`, `mtx1769`… | IRQ status / mask |
| `0x0908`–`0x0924`, `0x0938` | R/W | `mtx1391`, `mtx514` | microcode / DMA engine |
| `0x0bc0` | W | `mtx1475` | `TEDLFCounters` |
| `0x10b0`, `0x12b8` | W | `mtx3376` | init-time engine arm |
| `0x12bc` | W | `mtx2434` | DWGSYNC (direct path, dead in this build) |
| `0x1800`, `0x19a8`, `0x1a00`, `0x1ba8` | R/W | `mtx2073`, `mtx416` | CRTC heads |
| `0x1dc4` | R | `mtx532` | — |

`0x0384` deserves a note because it appears at 100 sites and looks like a
doorbell. It is not. Every MMIO write in the driver is preceded by

```
32417:  mov  eax,DWORD PTR [rbx+0x88]   ; offset of the PREVIOUS register written
3241d:  cmp  eax,0x408                  ; the offset about to be written
32422:  jae  32432
32424:  sub  eax,0xffffff80             ; eax += 0x80
32427:  cmp  eax,0x408
3242c:  jae  32541                      ; -> memWriteDword(handle, 0x384, 0)
32432:  mov  DWORD PTR [rbx+0x88],0x408
```

i.e. if the new register lies within 0x80 bytes *after* the previous one, a
dummy write to `0x384` is inserted first. It is a **write-posting / write-
combining barrier**, and an open driver that uses ordinary `writel()` with
proper barriers does not need it. [PROVEN]

**No 3D register is ever written by literal MMIO offset.** Every `0x10xx`,
`0x11xx`, `0x12xx`, `0x13xx` value reaches the chip through the ring. So there
is no hidden per-draw MMIO poke that our replay omits. [PROVEN]

`mtx3376` @0xa80 — the one function that writes `0x10b0 = 0x10`,
`0x12b8 = 0x70601` and edits `0x300` — is a **chipset workaround at device
init**: it compares the PCI ids `0x0528102b`, `0x0527102b`, `0x03051106`
(VIA), `0x25008086` / `0x25018086` (Intel) and picks AGP/PCI mode bits. It runs
once, long before any draw. The mmiotrace `captures/mmio-draw-0528-decoded.txt`
shows it firing twice at start-up and never again. [PROVEN]

### 5.2 Submission is a pure ring append [PROVEN]

`mtx1937` @0x32360 is the secondary-buffer call emitter:

```
323ce:  mov  DWORD PTR [rax-0x14],0x20292820
323d5:  mov  DWORD PTR [rax-0x10],0x0
323dc:  mov  DWORD PTR [rax-0xc],ebp     ; start | startTag
323df:  mov  DWORD PTR [rax-0x8],r12d    ; end   | endTag
323e3:  mov  DWORD PTR [rax-0x4],0x0
...
32446:  mov  esi,0x408
3244b:  call memWriteDword                ; the doorbell
```

with a wait path that polls `0x31c`/`0x318` until `(status & 0x57) == 0x50`,
and `0x384 = 0` when the 0x408 pointer would wrap below 0x80. Nothing else. Our
replay already reproduces this exactly, which is why the ring health instrument
passes. [PROVEN]

### 5.3 The one real out-of-ring arm: `0x920`, and the WARP microcode path [PROVEN]

This is the answer to "is there something the vendor writes outside the
inter-draw span". There is, and it is a **single dword**.

`mtx2316` @0x9fc70 is the WARP-program binder. It dispatches on
`dev->[0xa3114]`:

```
9fc7b:  mov  edx,DWORD PTR [rdi+0xa3114]
9fc83:  jne  9fca2          ; -> mtx1650, sysmem-heap mode
9fc88:  call mtx1391        ; -> on-chip IMEM mode
```

* **IMEM mode** — `mtx1391` @0x9f6b0 uploads the microcode by *direct MMIO*:
  `memWriteDword(0x918, offset/12)` (the address port, in 96-bit instruction
  units — the divide is the classic `0xaaaaaaab` multiply plus `shr 3`), then
  three `memWriteDword(0x91c, word)` per instruction until `prog->[0x18]`
  bytes are consumed. IMEM is `0x1800` bytes = **512 instructions**
  (`9f6eb`: overflow sets `dev->[0xa3121] = 1`). It returns the entry value
  `4 * (baseInstrIndex + entry[class])`.
* **Heap mode** — `mtx1650` @0x9fbc0 does **no MMIO at all**: `mtx1397`
  @0x9f5b0 is a plain `LocMemCpy(heapVA + slot*0x600, prog->[0x20],
  prog->[0x18])`, and the entry value is
  `((heapBase + slot*0x600)/3 + entry[class]*4) & ~3 | (dev->[0xa0650] & 3)`
  with `dev->[0xa0650] = 3` (`mtx2565` @0x9ff1d) — which is exactly the `| 3`
  of `parhelia-warp-program-heap`. [PROVEN]

**Which mode the chip is in is a register**, and it is written once, by direct
MMIO, by `mtx656` @0xa0e40 immediately after `mtx3806` @0x9fcc0 re-uploads the
eight base programs:

```
a0e8f:  or     edx,0x1
a0e92:  and    ecx,0xfffffffe
a0e95:  mov    eax,DWORD PTR [dev+0xa3114]
a0e9d:  cmove  edx,ecx
a0ee9:  call   memWriteDword          ; esi = 0x920
```

so **`0x920` bit 0 = "WARP programs live in the sysmem heap"** (bit 31 is
cleared separately; the X driver's first-start does `0x920 &= 0x7fffffff`).
The live mmiotrace shows `W 0x0920 = 0x1` — heap mode. [PROVEN]

**Consequence for the replay.** The `0x1100` / `0x10c0..0x10d8` entry values in
a captured span are computed *for whichever mode `0x920` selects*. Replaying
them with `0x920` in the other mode makes the WARP fetch from the wrong place:
every packet is consumed, the engine goes idle, and no fragment is produced —
exactly the observed symptom. For a **same-boot replay under a live X + GL
client this is already correct** (X and the client set it), which is why the
existing replay eventually worked; for a **from-scratch open driver it must be
programmed explicitly**, together with the upload path.

Registers involved, none of which any ring capture can contain:

| reg | meaning | writer |
|---|---|---|
| `0x0918` | WARP IMEM write address, in instruction units (`byteOffset / 12`) | `mtx1391` |
| `0x091c` | WARP IMEM data port, 3 dwords per 96-bit instruction | `mtx1391` |
| `0x0920` | bit 0 = heap mode / on-chip IMEM mode; bit 31 cleared | `mtx656`, `mtx676`, `mtx3810` |
| `0x0bc0` | `TEDLFCounters`, from a driver tunable object | `mtx1475` @0x3e835 |

`mtx1475` is called from `mtx3380` @0x38710, the engine/3D (re)init:
`… mtx656 (0x920) → mtx1475 (0xbc0) → mtx2070`. Both are once-per-init.

### 5.3b A free instrument: reading the WARP register file back [PROVEN]

`mtx514` @0x8f7c0 is a register-file readback port and is *not* part of any
draw path (its only caller is the `mtx676` self-test):

```
8f813:  index = ((a << 9 | b) & 0xfff) | ((fileSel & 0xf) << 24)   ; fileSel 0..0xa
8f854:  memWriteDword(0x910, index)        ; index port
8f8a6:  memWriteDword(0x938, 0xfffffffc)   ; strobe
8f8dc:  memReadDword (0x914, &out[0])      ; data, read 4x to settle
        ... repeated with index | 0x10000 / 0x20000 / 0x30000 -> out[1..3]
```

Four dwords per slot = one x/y/z/w register. This lets an experiment ask "did
the WARP actually produce vertex output?" without guessing at a VRAM address —
a much better instrument than a blind diff. `mtx612` @0xa0120 shows the usage:
`fileSel = 3`, index 0..0x17f, four banks.

### 5.4 The two other out-of-span dependencies

1. **The WARP program heap.** `0x1100` and `0x10c0..0x10d8` in the span are
   *entries into the client's live class-3 program heap*
   (`0x07d57783`, `0x07d579bf`, …). Those addresses are valid only on the boot
   and in the context that produced them. They are not "an arm we forgot", but
   they do make a captured image non-portable. (Already recorded in
   `parhelia-warp-program-heap`.)
2. **The block-load frontend.** The ten `0x41` packets are consumed through the
   vertex-fetch frontend, so a one-attribute pipe must be programmed
   immediately before them and the real vertex format restored after. The span
   contains that programming (`0x1000=0`, `0x1040=4`, `0x10b0=0x0002305c`,
   `0x10b4=0`, `0x10b8=0x83c00000` at ring[556]/ring[561], restored at
   ring[661]–ring[681]), so a byte-exact replay is fine; a *reordered* or
   *pruned* replay is not.

### 5.4b The Windows driver's per-draw prolog, for comparison [PROVEN]

MTXPARD has an explicit `ValidateState` at `0xc9ac0` that emits only the dirty
state groups and then arms:

```
   c9ad1:  call 0xc9250    ; -> 0x1328,0x132c,0x12fc,0x12f8 | 0x12e8,0x12e4,0x12f0,0x12ec
   c9add:  call 0xc9330    ; -> 0x12c8,0x12cc,0x12d0 | 0x12c0,0x12c4
   c9ae9:  call 0xc9160    ; cull / face state (not decoded)
   c9afc:  call 0x13b3d0   ; -> 0x1130   (tag 0x2121214c)
   c9b08:  call 0xc95b0
   c9b15:  call 0xc98b0    ; -> surface slots, 0x1314/0x1318/0x1324, 0x12b0, DWGCTL
   c9b22:  call 0xca1c0
   c9b9c:  tag 0x21e3c1c2  ; -> 0x1308, 0x1304, 0x138c
```

and the tail of `0xc98b0` is the arm itself — the same shape the Linux kernel's
`mtx1619` emits and the same shape the capture contains:

```
   c9a37:  count 4, tag 0xacafafc0 ->  0x1300 = 0x307        (drain)
                                       0x12bc = tag|1        (block until retire)
                                       0x12bc = tag
                                       0x12b0 = <MemLevels>
   c9a8d:  count 1, tag 0x212121c0 ->  0x1300 = <the real DWGCTL>
```

The tag counter increments by `0x10` per validate (`0xc9a17`). **The arm is
per-state-validate, not per frame**, and it is entirely inside the command
stream — another confirmation that a byte-exact span replay is not missing it.

`0x12b0` is not a constant: MTXPARD switches it from `0x3fff8842` to
`0x3fff9084` when the operation covers more than `0x3d090` = 250 000 pixels
(`0xa622f`, `0xa624c`, and the same test at `0x10ddaf`) — consistent with a
memory/burst watermark and with the `MemLevels` name (§4.6).

### 5.4c The vendor's own known-good 3D engine init [PROVEN]

Worth transcribing because it is the complete reset state an open driver can
start from (MTXPARD `0x2c100`–`0x2c621`):

```
texture units  0x1200..0x120c = 0x40100 | (1 << unit)
               0x11c0=0  0x11c4=0x249  0x11c8=0
               0x11cc=0xffffffff  0x11d0=0x11d4=0x11d8=0
               0x11e8=0x100000    0x1374=0
               0x1200=0x1000f  0x1204=0x1208=0x120c=0
               0x1300=0x002010cc
combiner x5    0x1264=0x80+2n  0x126c=0x400400  0x1260=0x80  0x1268=0x400400
tests          0x12e4=0  0x12c0=0xffffffff  0x12c4=0x7fffffff  0x12e8=0
               0x12d4=0  0x12c8=0  0x12cc=0xffff  0x12d0=0x1ff
               0x1324=0xffffffff  0x1318=0  0x1314=0  0x12fc=0
               0x1328=0  0x132c=0x0fff0fff  0x1304=0  0x1308=0
               0x12f4=0x41f80000 (31.0f)
fence          0x1300=0x307 ; 0x1300=0x0c ; 0x1300=0x707, 0x1374=0, 0x12bc=tag|1, 0x12bc=tag
               0x12b0=0x3fff8000..0x3fff0000 (by clock)
```

and the inverse, "leave 3D / enter 2D" (`0x2e760`), which is also the shape of
the two priming pseudo-draws in our capture:

```
tag 0x21b5bac9 : 0x1324=0, 0x12e8=0, 0x12d4=0
tag 0x21b4b2b3 : 0x12cc=0, 0x12c8=0, 0x12d0=0
tag 0xb2cbcac0 : 0x1300=0x001000f4, 0x1328=0, 0x132c=0x00010001, 0x12c8=0
tag 0x8988bfb9 : 0x12e4=1, 0x12fc=0, 0x1220=base, 0x1224=cfg|0x32000000
then 31 dwords zeroing the 0x1160..0x1198 DDA plane registers
```

Note `0x12fc = 0` and `0x12e4 = 1` are the *2D* values: **`0x12fc = 0` is the
"3D rasteriser off" state**, and it is what the priming pseudo-draws leave
behind until the per-draw value `0x0bb80024` is written. An open driver that
forgets to write `0x12fc` after a 2D operation draws nothing. [PROVEN]

### 5.5 What `mtx3712` actually is, and why it is not a missing arm [PROVEN]

`mtx3712` @0x188f0 is the per-draw *validate* function — the closest thing in
the driver to a "Setup/Validate/Prolog". It walks the dirty-bit shadows
(`+0xc87` depth block, `+0xd54` fog block, `+0x1b44`, `+0xdbb`, `+0x2978`
raster block, `+0x2aa8` back-end block), calls the sub-emitters
(`mtx698`, `mtx297`, `mtx3654`, `mtx339`, `mtx1153`, `mtx1035`, `mtx3319`,
`mtx3405`, `mtx2218`, `mtx2714`, `mtx2215`, `mtx2480`), assembles DWGCTL (§3.1)
and stores it into the shadow at `dev->[0x40] + 0xb08` with dirty bit 0x01.
**Everything it produces ends up in the command stream** — the function itself
performs no MMIO. So replaying the stream replays its entire effect. [PROVEN]

---

## 6. Ranked causes of "executes clean, zero fragments", and the tests

### 6.1 Test 0 — prove the measurement path before anything else

The 2D fill instrument is already proven on this hardware (120000 px, verified).
Use it as the calibration:

```
tools/parhelia-3dtri --fill --filloff 0x02000000 --pitch 1920 --width 320 --height 240 \
                     --vramdiffbase 0x02000000 --vramdiff 0x900000
```

(`--filloff` sets the fill's destination offset explicitly; `--fillrt` instead
reuses `--rtoff`.)

* pixels appear → the reader reaches VRAM 32 MB, the mapping is fine, and the
  3D draw really is going somewhere else. Proceed to Test B.
* nothing appears → the *reader* (or the BAR window, or the aperture) never
  reached 32 MB, and every "zero fragments" verdict taken through that path is
  void. Fix the reader first.

One run, and it removes the largest single source of false negatives in this
project.

**Test 0b — calibrate BAR offset against engine address, using the screen.**
`InitPrimarySurface` proves the *screen* surface's base register value is **0**
(§2.7), and the screen is at VRAM byte 0. So re-point a surface slot at base 0
with the screen's cfg and confirm you can read the visible desktop through the
same path:

```
tools/parhelia-3dtri --readreg 0x1220        # what the hardware currently holds
# then read the BAR at offset 0 and compare against what is on screen
```

If BAR 0 shows the desktop but BAR `0x02000000` shows zeros, the two spaces do
*not* coincide beyond bank A's start, and §2.6b's `out0` vs `out2` split is the
explanation: the engine address in `0x1220` is not a BAR offset. In that case
the only reliable way to see a 3D draw is to **retarget it to an address you
allocated yourself** (Test B), not to hunt for the vendor's buffer.

**A second, cheaper thing to fix in the same run: widen the read window.** The
null result was taken over `0x0205a0c0..0x02168440`, which is the triangle's
position computed for a drawable origin of (0,0) — the first ~1.4 MB of the
buffer. Two facts say that window is too narrow:

* the previously measured vendor footprint `0x02f654f8..0x030d1b6f` **starts
  inside** `[0x02000000, 0x03000000)`, i.e. 15.4 MB into a region whose next
  known allocation boundary (slot 1 at `0x01000000`) is 16 MB away. It is
  outside the read window but not outside the buffer;
* the vendor re-emits each primitive **once per X clip rectangle at a
  translated drawable origin** (`mtx2254` @0x4da00 writes the origin into the
  state shadow at `+0xcd1`/`+0xcd5` with dirty bit 0x80,
  `docs/parhelia-rendertarget.md` §1.4), so a non-zero origin is normal and the
  window-local scissor `(0,0)-(320,240)` does not imply the pixels land at the
  start of the surface.

So read `0x02000000 .. 0x03200000` (18 MB) rather than 1.4 MB, and judge by a
changed-input diff. Note that a byte offset decomposes into some (row, column)
for *any* pitch, so an "exact" row/column decomposition of a measured address
is never on its own evidence for a pitch — only a changed-input diff is.

### 6.2 Test B — the slot sweep, which settles §3.3 in four runs

The one genuinely open question is which of the four descriptor slots the 3D
pipe writes. Sweep them:

```
for N in 0 1 2 3; do
  tools/parhelia-3dtri --replay … --rtslot $N --rtbase 0x04000000 --rtcfg 0x32020780 \
                       --vramdiffbase 0x04000000 --vramdiff 0x900000
done
```

(`--rtslot N` sets the patched register to `0x1220 + 8*N`; note the tool's
default is already `0x1238`, i.e. slot 3. The patcher rewrites **every**
occurrence of that register and of `reg+4` in the replayed stream, so a slot
whose span also contains a null bind stays valid throughout the run — which is
what makes the sweep clean.) Whichever slot moves
the output to `0x04000000` **is** the 3D destination. Expected outcomes:

| slot that moves the pixels | conclusion |
|---|---|
| 0 | Reading B of §3.3; the RT is `0x1220`, and the earlier null reads were a measurement or stride problem |
| 3 | Reading A of §3.3; DWGCTL bits [11:10] select the slot for 3D too, the captured span leaves slot 3 null, and **the replay must bind slot 3** |
| 2 | the pipe writes the "depth" slot for colour — re-examine §2.7 |
| none | the destination is not in the `0x122x` file at all; next suspects are the block file (opcode 0x41) and `0x10b4` |

**Caveat, and it matters — do not run the sweep naively.** `--rtbase` patches
*every* write of the chosen register in the stream, and the span also contains
X's 2D work: the `DWGCTL = 0x000005c0` fills use **slot 1 as both source and
destination**, and the full-screen compose reads slot 1 at `0x02000000`.
Rewriting slot 0 or slot 1 wholesale corrupts those packets and stalls the
replay on a `0x5c0` packet (the tool's own source comments record exactly this
failure). So:

* **slots 2 and 3 are safe to sweep as-is** — the 2D packets in this span never
  bind them to anything but the null descriptor.
* **for slots 0 and 1, patch only the LAST occurrence** of the register before
  the draw (ring[691] for slot 0, ring[696] for slot 1 in the current capture).
  That is a one-line change to the patch loop: remember the last match and
  rewrite it after the walk instead of rewriting inside it.

Run slot 3 first: it is safe, it is the tool's default, and it is the reading
the DWGCTL constant points at.

If slot 3 wins, the fix for the replay is one packet before the draw:
`0x1238 = <rt base>`, `0x123c = 0x32020780`.

### 6.3 Test C — mind the two pitches when computing where to look

Before declaring a region empty, remember there are two (§2.5): the physical
footprint uses `roundUp(width, 32)`, while the CPU-visible de-tiled window uses
`pow2ceil(width) * bpp/8` for types 2–5. For the captured `0x30030780` the
aperture pitch is **8192** bytes per row, not 7680. If the surface is genuinely
tiled, no single linear stride locates a pixel at all — which is another reason
to **retarget the draw to your own linear allocation** rather than to search the
vendor's buffer.

### 6.4 Test A — full-VRAM diff with a changed input

```
tools/parhelia-3dtri --replay … --vramdiffbase 0 --vramdiff 0x10000000
```

run twice with a different vertex colour or position and judge by the *changed*
dwords. Never judge by a same-input diff, and never by a guessed window. This is
the only measurement that cannot produce a false "nothing drew".

### 6.5 The scissor is the client's window, and the vertices must match it

`0x132c = 0x00f00140` = a 320×240 rectangle, and `0x1300` bit 23 enforces it.
A replayed span carries the captured client's projection *and* its scissor, so
vertices have to be in **that client's window coordinates**. NDC vertices are
clipped away and draw nothing — already measured
(`parhelia-3d-integrated.md` §6b). The vendor's own buffer holds NDC floats
only because its WARP program transforms them; if the replay substitutes raw
vertices without the same program bound, the coordinate space changes.

**Test C:** widen the scissor before the draw — add
`0x1328 = 0x00000000`, `0x132c = 0x0fff0fff` (the value the vendor's own clear
path uses, `mtx2187` @0x3ccb0) — and re-diff the whole 256 MB. If pixels appear
outside the 320×240 box, the geometry was landing outside the window.

### 6.6 `0x920` selects the wrong WARP program space

`0x920` bit 0 says whether the WARP fetches its programs from the **sysmem
heap** or from **on-chip IMEM** (§5.3), and the entry values in `0x1100` /
`0x10c0..0x10d8` are computed differently for the two modes:

* heap: `((heapBase + slot*0x600)/3 + entry[class]*4) & ~3 | 3`
* IMEM: `4 * (baseInstrIndex + entry[class])`

A replay that carries heap-form entries while `0x920` says IMEM (or the
reverse) makes the WARP fetch garbage: **every packet is consumed, the engine
reports idle, and nothing rasterises** — precisely the reported symptom. It is
one dword and no ring capture can ever contain it.

Under a live X plus a live GL client this is already correct (`W 0x0920 = 0x1`
in the mmiotrace = heap mode), which is why a same-boot replay eventually
worked. It becomes mandatory the moment the open driver arms the engine itself.

**Test G:** `--readreg 0x920` before the replay; it must be `1` (bit 31 clear)
for heap-form entries. If it is 0, either write `0x920 = 1` or recompute the
entries in IMEM form.

### 6.7 The WARP program entries are stale

`0x1100 = 0x07d57783` and the six stage entries point into the frozen client's
class-3 heap. On a different boot, or after the client has been resumed and has
reallocated, those entries reference something else and the pipeline can
consume the draw and emit nothing. **Capture and replay must be the same boot**
with the client still stopped.

**Test D:** dump the heap at `entry` → `bus = (v & ~3) * 3` before and after the
replay and confirm the 13 program words are unchanged
(`tools/dump-warp-programs.sh`).

### 6.8 The depth slot is null but depth is off — verify, do not "fix"

`0x1230/0x1234 = {0, 0x32020000}` looks alarming, but `0x12c8` bit 0 = 0 means
depth testing is disabled and no Z surface is needed. **Do not "repair" this by
binding a Z buffer** — that would enable a path the captured state never used.
If you want to rule it out, force `0x12c8 = 0x00000000` (depth and stencil both
off, func NEVER cleared) and re-run.

### 6.9 Write masks and the pipeline flags — already ruled out

`PLNWT = 0xffffffff`, blend off, alpha test off, cull off, guard band 32.0,
z-clip full range. There is no state in the captured span that can suppress a
fragment. If Tests A–D all come back negative, the next thing to suspect is the
*vertex* path (`0x10b8` attribute descriptors, `0x1110/0x1114/0x1118` vertex
format, `0x1060` count), not the fragment back end.

### 6.10 The run order, condensed

| # | do this | if it fires |
|---|---|---|
| 1 | **2D-fill VRAM `0x02000000`** and read it back through the same path that produced the null result (§6.1) | nothing appears ⇒ the reader/BAR never reached 32 MB and every previous "no fragments" verdict is void |
| 2 | Widen the read window to `0x02000000 .. 0x03200000` and re-diff the existing replay with a **changed input** | pixels appear ⇒ the draw always worked; the window was 1.4 MB of an 18 MB buffer |
| 2b | Read BAR offset 0 and check it shows the desktop (§2.7: the screen surface's base register value is 0) | BAR 0 = desktop but BAR `0x02000000` = zeros ⇒ engine address ≠ BAR offset (§2.6b); stop hunting for the vendor's buffer and retarget instead |
| 3 | **`--rtslot 3 --rtbase 0x04000000 --rtcfg 0x32020780`** (safe: no 2D packet in the span binds slot 3) | pixels at `0x04000000` ⇒ §3.3 Reading A: the 3D destination is slot 3 and the replay must bind `0x1238`/`0x123c` |
| 4 | `--rtslot 2` likewise (also safe) | pixels ⇒ the "depth" slot is the colour destination |
| 5 | slot 0 / slot 1, patching **only the last write** before the draw (§6.2 caveat) | pixels ⇒ §3.3 Reading B |
| 6 | read `0x920`; it must be 1 for the captured heap-form WARP entries (§6.6) | 0 ⇒ the WARP is fetching from the wrong program space |
| 7 | widen the scissor: `0x1328 = 0`, `0x132c = 0x0fff0fff` (the value the vendor's own clear uses) | pixels outside 320×240 ⇒ geometry was landing outside the window |
| 8 | belt and braces: `0x1324 = 0xffffffff`, `0x12c8 = 0`, `0x12e4 = 0`, `0x12e8 = 0` | no change expected — this only *proves* the back end is open |

Steps 1 and 2 cost one run each and need no code change. Step 3 is one flag.

---

## 7. Corrections this document makes

| claim | source | verdict |
|---|---|---|
| DWGCTL bits [11:10] select the destination surface slot for a 3D draw | `parhelia-rendertarget.md` §1.3 | **unresolved, not settled either way** — §3.3 sets out both readings and the four-run experiment. The field map is correct for 2D; for the 3D opcode the bits are part of an unconditional constant and the GL depth clear is a same-opcode counterexample. |
| `0x1224 = 0x32020000` (pitch 0) is an anomaly | task framing | it is the **unused-slot** value: base 0 with the geometry zeroed. It never appears with a non-zero base (§2.3), and MTXPARD's stride helper returns 0 for it. |
| pitch is simply "bits[13:0] in pixels" | every previous doc | **incomplete**: the *type* field selects the rounding — multiple of 32 for types 0/1/2, **power of two** for types 3/4/5 (§2.5). A type-3 "pitch 1920" surface is addressed as 2048 wide. |
| `0x12b0 = 0x3fff8842` purpose unknown | `parhelia-pixel-pipeline-complete.md` §3.5 | it is the vendor tunable **`MemLevels`**; MTXPARD switches it to `0x3fff9084` above 250 000 pixels (§4.6, §5.4b) |
| `0x12f8` is "reg3DThresholdAA" by quotation | ibid. | confirmed by name and default in this binary: tunable **`ThresholdAA`**, default `0x7fffff` (§4.6) |
| the low nibble of a 3D DWGCTL is `((surf >> 25) & 0xf)` | ibid. §3.8 | the instruction sequence is right, the operand is not: it gives 9 for `0x32020780` while the capture shows 4 (§3.2) |
| `0x1318` is the colour write mask | implied by MTXPARD's 2D fill | **no** — `0x1324` PLNWT is the mask; `0x1318 = 0` across a correctly rendering captured frame (§4.1) |
| `0x1130 \| 0x200` is force-ORed on every draw | `parhelia-3d-integrated.md` §8 | **conditional**; bit 9 is clear in both 0528 captures (§4.3) |
| the render target "is not `0x02000000`" | `parhelia-render-target-location` | still open, but the frame is now different: `0x02000000` is an **engine** address, and the kernel hands the driver the engine address and the framebuffer-aperture offset as two separate values (§2.6b). "Not at BAR `0x02000000`" does not imply "not at engine `0x02000000`". Two captures on different boots have **identical** surface bases, so the "different boots" explanation is also weaker than it looked. |
| the `0x0000..0x013f` block is "host-visible aperture descriptors, never consulted for a surface address" | `parhelia-engine-context-bringup.md` §2.3 | **confirmed and completed**: `EmulMmSurfaceMapper::AllocateApertureBlock` @0x74c40 builds them (`or 0x10000000`, 256 KB granularity, `shl 4` = 16-byte stride), and `MmSurface::CanUseVertexHostAperture` @0x736c0 range-checks engine addresses against the window — so the aperture and the engine share one flat space (§2.6b) |
| cfg bit 23 = byte/word swap | several docs | **no** — `Surface2d::SwapSurfMode(bool)` @0x7c2d0 toggles exactly `0x00800000`, and its caller is `TempBit::SetSurfModeTiled`. Bit 23 is **linear/tiled** (§2.5) |
| "WARP programs are not DMA'd from system memory; they are written through a two-register window" | `parhelia-engine-context-bringup.md` §3.1 | **half the story**: that is the on-chip IMEM mode. `mtx2316` @0x9fc70 dispatches on `dev->[0xa3114]` and the *other* branch (`mtx1650` @0x9fbc0 → `mtx1397` @0x9f5b0) is a plain `LocMemCpy` into a sysmem heap with **no MMIO at all**. Which mode is live is register `0x920` bit 0, and the live trace has it set — the 0528 under X is in **heap** mode (§5.3) |
| `0x1374` is a strobe register | several docs | it is a **second pad**: its data dword is left uninitialised by the emitter and ignored by the hardware (§4.4) |

## 8. Reproducing the decode

`reloc7.txt` lines are `ringIndex sectionOffset sectionLen drawIndex startTag
endTag`; `startTag == 2` means the section at that offset in `sec7.hex` is
header-ful (opcode packets, `header = (opcode<<24) | (count<<17) |
((totalDwords-2) & 0xffff)`), `startTag == 0` means bare tag dwords. Walking
the sections in `ringIndex` order, expanding opcode-0x00 bodies as
`[tag][data…]` with `reg = 0x1000 + idx*4`, `idx 0x20` = pad and `idx 0x21` =
tag terminator, yields the table of §1. Register names come from
`tools/annotate-stream.py` (`regname()`), which can be imported directly.

In this span: 123 sections, of which one is header-less; 112
opcode-0x00 register lists, 10 opcode-0x41 block loads, one opcode-0x17 draw
(`97080001 16a523d0 16a5240c`).

## 9. Still open

* **Which slot a 3D draw writes** (§3.3). The single blocking unknown; four
  hardware runs settle it.
* Where inside slot 0's allocation the drawable origin puts the primitive —
  the origin is written to a state shadow (`mtx2254` @0x4da00, `+0xcd1`/`+0xcd5`)
  but the register it ends up in has not been identified.
* Whether the framebuffer BAR maps bank A linearly from 0. `AllocateSurfaceBlock`
  @0x76590 proves the engine address and the FB-aperture offset are separate
  values, but not what the mapping is on this board. Test 0b (§6.1) measures it
  in one run against the primary surface, whose base register value is provably 0.
* Whether a type ≥ 2 surface is physically tiled in VRAM. The existence of a
  de-tiling host aperture with a pow2 pitch says the raw layout differs from the
  advertised pitch; the tile pattern itself is undecoded. cfg bit 23 selects
  linear vs tiled and is clear on every surface in the capture, which argues the
  captured surfaces are linear — so this may not bite here.
* The meaning of the `0x12fc` upper field (`0x1800` priming vs `0x0bb8`
  per draw) and of flag bits 2/4/5. Bits [1:0] are the cull/primitive class.
* Which of the 32-bpp format codes `0x30` / `0x32` / `0x36` implies alpha.
  `0x32` is the generic 32-bit colour code; `0x30` is only ever seen with
  type 3 and `0x36` only with types 2/4 — [INFERRED] they are layout variants
  tied to the type, not independent channel orders.
* Cull mode: it is not in any `0x12xx` register examined here; MTXPARD routes
  `D3DRS_CULLMODE` into the `ctx+0xe68` group emitted by `0xc9160`, which is
  undecoded.
* The virtual call `[pDev->[0x8]->[0x2198]->[0x4c0]]->[0x20]` whose result
  gates DWGCTL bits 21/22 — a device-class query, value unknown.
* `0x1324`'s second possible role: MTXPARD zeroes it on entry to 2D and sets it
  to all-ones at 3D init, which is consistent with a write mask but does not
  exclude a constant-Z reading.
