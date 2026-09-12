# Where a rendered pixel lands: surface base, tiling, and how the 3D target is bound

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Clean-room reverse engineering from binaries only (`objdump`/`nm` on
`kernel/mtx_parhelia.o` and `xserver/7_4_0/mtx_drv.so`), cross-checked against
hardware measurements taken on the 0x0528. No proprietary source was read.

> **SUPERSEDED IN PART — see `docs/parhelia-own-render-target.md`.**
> Three claims below are corrected there:
> * §3.2 "the binding is emitted by the kernel into the primary stream, never by
>   the client into its secondary sections" is **wrong** — the `0x8d8c8988`
>   packet is an ordinary opcode-0 register write inside the called sections, and
>   it occurs 2 486 times as a well-formed packet across the gl-matrix captures.
> * §2 calls cfg `[13:0]` a *pitch*. It is a **width in pixels** with granularity
>   32; the hardware derives the byte stride from width, type and format
>   (`MmSurfDesc::GetHostAperturePitch`).
> * §5.3 guesses the tiling selector is cfg `[31:30]`. It is **not** — no code in
>   either vendor binary ever sets those bits, and every writer preserves them.
>   Nor is it the type field: the vendor flips one live surface between type 3
>   (2D view) and type 2 (3D view) with no data conversion, and types 2 and 3 are
>   byte-for-byte identical throughout the allocator. Where the tiling actually
>   comes from — or whether the 64x32 model in §5 is an artefact of the base that
>   measurement assumed — is now OPEN.
> Everything else here still stands.

## Summary

1. The surface base register is a **plain byte address** in the GPU address
   space, low two bits = memory-space tag. No shift, no page table, no MMU.
   [PROVEN, §1]
2. The 3D colour target is **surface slot 0** (`0x1220`/`0x1224`) and depth is
   **slot 2** (`0x1230`/`0x1234`) — but the packet that programs them is
   **emitted by the kernel into the primary stream**, never by the client into
   its secondary sections. That is why the addresses are absent from the
   capture and why a replay only rasterises while the vendor's context is
   alive. [PROVEN, §3]
3. DWGCTL bits `[11:9]` are **not** a destination-slot selector on the 3D path:
   the 3D DWGCTL builder ORs the constant `0x860e00` unconditionally, and
   `0x00a60ec4 == 0x860e00 | 0x200000 | 0xc4` exactly. Slot 3 is a decoding
   artefact. [PROVEN, §4]
4. The 3D render target is **tiled: 64 x 32 pixels per tile, 8192 bytes, tiles
   row-major**. This is derived exactly — to the byte — from the measured
   16128-pixel footprint, and it is corroborated by the allocator's 32-pixel
   height granularity and its 8192-byte alignment. [DERIVED from measurement +
   binary, §5]

---

## 0. Correction to the first revision of this document

The first revision claimed the capture's descriptor-to-slot attribution was
byte-order-reversed. **That claim is withdrawn — it was wrong, and the evidence
this document itself cites refutes it.** `SurfDesc2d::Init` pairs `slot << 10`
with header `0x21218988` for slot 0; read LSB-first that header is register
indices `0x88`, `0x89`, i.e. `0x1220`, `0x1224`. So slot N *is*
`0x1220 + N*8`, the existing decoders were already correct, and there is no
reversal. The real explanation for "DWGCTL says slot 3, slot 3 is empty" is §4:
on the 3D path those bits are not a slot field at all.

The first revision also claimed the drawing engine's row stride is
`next_pow2(pitch) * bpp/8`. That is the **host aperture** (CPU) pitch, not the
engine's layout — §5 replaces it with the tiled layout, which is what the
hardware footprint actually shows.

---

## 1. The base value is a byte address + memory-class tag  [PROVEN]

`MmSurface::AttachToBlock(MmBlock*)`, `mtx_drv.so .text+0x73710`:

```
73720:  mov   0x20(%rsi),%r8d        ; r8 = block->offset
73772:  movslq %edx,%rcx             ; edx = MemoryType (0..7)
7377c:  shl   %cl,%rdi               ; mask = 1 << MemoryType
7377f:  test  $0xc3,%dil ; jne  ..   ; types 0,1,6,7 -> tag 0 (local)
73785:  test  $0x4,%dil  ; mov $2,%al ; type 2 -> tag 2 (AGP)
7378d:  and   $0x8,%edi  ; mov $3,%al ; type 3 -> tag 3 (PCI/system)
73736:  mov   %eax,0x90(%rbx)        ; surf->0x90 = block->offset | tag
```

`Surface2d::Enable` (`+0x7bf00`) copies `surf->0x90` to `bitmap->0xe0` (and
forces `|3` for non-local); `DeviceBitmap::ProgSurfDesc` (`+0x7c350`) emits
`bitmap->0xe0` to the base register and `bitmap->0xc4` to the cfg register.
Kernel `mtx1132` (`+0x4c730`) does the identical arithmetic
(`addr = offset + region->0x18`, `|2` / `|3` by region type).
`HSLPAR_Register_PriSurfAddr_Set` (`+0xc5810`) masks the scan-out address
`& 0x7fff800` — in place, unshifted. Same address space throughout.

**`0x02000000` means VRAM byte 0x02000000, memory class 0 (local).**

### 1.1 Independent confirmation from the observed base set  [PROVEN]

The complete set of bases seen in the capture is
`0x00000000, 0x01000000, 0x02000000, 0x018e8000, 0x018ea000, 0x09000000,
0x09002000`. Note:

```
0x01000000 + 1920 * roundup(1200,32) * 4  =  0x01000000 + 0x8E8000  =  0x018E8000
```

`0x018e8000` is *exactly* the end of a 1920x1200 32 bpp surface allocated at
`0x01000000` under the allocator's own size formula (§5.3), and `0x018ea000` is
one 8192-byte tile past it. `0x09000000`/`0x09002000` show the same 8 KB
relation. Three facts fall out at once: the bases are literal byte addresses,
the height granularity is 32 pixels, and the allocation unit is 8192 bytes.

---

## 2. The cfg word  [PROVEN]

`MmSurfDesc+0x04` **is** the value written to `0x1224 + 8*slot`; the four
`MmSurfDesc` helpers mask it with `0x3fff` (pitch), `0x70000` (type) and
`0x3f000000` (format).

```
 31 30 29        24 23 22 19 18   16 15         0
 +-----+-----------+--+-----+-------+------------+
 |tile | format    |sw| ... | type  | pitch (px) |
 +-----+-----------+--+-----+-------+------------+
```

| type `[18:16]` | width gran | height gran | host-aperture pitch | local align (32bpp) |
|---|---|---|---|---|
| 0 | 32 | 1 | `roundup32(pitch)*bpp/8` | 0x200 / 0x40 |
| 1 | 32 | 2 | `roundup32(pitch)*bpp/8` | per-bpp table |
| **2, 3** | 32 | **32** (16 if `desc->0x10 & 0x7fc00`) | `next_pow2(pitch)*bpp/8` | **0x2000** |
| 4, 5 | 32 | 0x20 / 0x40 | `next_pow2(pitch)*bpp/8` | 0x2000 / 0x800 |

Sources: `GetWidthGranularityREVB` `.text+0x74350` (a constant `mov $0x20`),
`GetHeightGranularityREVB` `.text+0x74360` (the type switch),
`GetHostAperturePitch` `.text+0x745c0`, `GetLocalAlignmentAndGranularityREVB`
`.text+0x74170`, `GetPhysicalMemSizeREVB` `.text+0x74490`.

> **Note on the power-of-two pitch.** `GetHostAperturePitch` is the pitch of the
> **CPU's view through a host aperture**, not the engine's layout — the
> allocator sizes surfaces with granularity 32, not with a power-of-two pitch,
> so the two cannot both be the memory stride. The engine layout is §5. The
> separate, hardware-measured fact that the *display controller* wants a
> power-of-two fetch stride (`randr-native-displaywidth-pow2-stride.patch`,
> 1280 -> 2048) is a DC constraint and stands on its own.

Format field, from `GetBitsPerPixel` `.text+0x73e70` (32 bpp class:
`0x30 0x32 0x33 0x34 0x36 0x38 0x3a 0x3b`) and the vendor's selector
`mtx3388` `.text+0x4fff0` (8 bpp -> 0x10, 16 bpp -> 0x20, 30 bpp -> 0x34,
24/32 bpp -> 0x30), with `mtx1192` `.text+0x3dcb0` forcing `0x32` for a
driver-owned colour target. Reading the low bits of the `0x3x` family as
alpha (bit 1) and 10:10:10 (bit 2) gives `0x30` X8R8G8B8, `0x32` A8R8G8B8,
`0x34` X2R10G10B10 (proven — the GigaColor code).  `0x36` is no longer an
open alpha-colour candidate: later vendor-draw evidence proves it is the active
S8Z24 depth/stencil format.  See `parhelia-gigacolor-faa-hardware-status.md`.
[`0x38/0x3a/0x3b` remain OPEN.]

---

## 3. Where the 3D destination surface is actually bound  [PROVEN]

**It is surface slot 0. There is no separate 3D-specific target register.** What
is different is *who writes it*: the kernel, into the primary stream, on the
context's behalf — not the client, into a secondary section.

### 3.1 The chain

`mtx2372` `.text+0xcc30` snapshots the four descriptors into the draw context:

```c
/* rdi = draw context, rdx = ctx->[0xe0] (the drawable), rcx = rdx->[0x40] */
for (i = 0; i < 4; i++) {
    ctx[0x144 + 4*i] = rcx[0xb28 + 4*i];   /* base */
    ctx[0x154 + 4*i] = rcx[0xb38 + 4*i];   /* cfg  */
    ctx[0x164 + 4*i] = rcx[0xb48 + 4*i];   /* dims */
}
```

`mtx2954` `.text+0xf8e0` emits them into the context's command buffer
(`ctx->[0xe0]->[0x68]`, cursor at `+0x60`, limit at `+0x70`) and posts it with
`mtx1937`:

```
 f96c:  movl $0x4,(%rcx)                ; register count
 f972:  movl $0x8d8c8988,0x4(%rcx)      ; indices 0x88,0x89,0x8c,0x8d
 f97c:  mov  0x144(%r12),%eax ; ->  0x1220   colour base   (slot 0)
 f98b:  mov  0x154(%r12),%eax ; ->  0x1224   colour cfg
 f996:  mov  0x14c(%r12),%eax ; ->  0x1230   depth  base   (slot 2)
 f9a1:  mov  0x15c(%r12),%eax ; ->  0x1234   depth  cfg
 f9bd:  movl $0x2,(%rcx)
 f9c3:  movl $0x21218b8a,0x4(%rcx)      ; indices 0x8a,0x8b (+2 pad 0x21)
 f9ce:  mov  0x148(%r12),%eax ; ->  0x1228   other colour buffer (slot 1)
 f9df:  mov  0x158(%r12),%eax ; ->  0x122c
```

So **colour = slot 0, depth = slot 2, the drawable's other buffer = slot 1**,
and slot 3 is genuinely unused by the 3D path.

### 3.2 Why the address is absent from the captured secondary sections

`mtx2954` runs kernel-side at bind/validate time and writes into the *context's*
command buffer, which is posted through the primary path
(`OsPostBusmasteringBuffer`, `type = 0`). The client's secondary sections
contain only what the ICD builds. Walking every secondary section therefore
finds only the X server's 2D descriptors — `0x00000000`, `0x01000000`,
`0x02000000` (`0x30030780`, type 3 = the desktop), `0x018e8000`, `0x09000000` —
and never the GL back buffer.

**This is the whole reason a standalone replay does not rasterise.** The
registers still hold whatever the vendor's kernel last programmed; kill the
context and its surface goes away, and nothing in the replayed stream ever
re-binds one.

The confirming instrument: **capture the primary buffer, not just the secondary
sections, and scan it for the dword `0x8d8c8988`; the next four dwords are
colour base, colour cfg, depth base, depth cfg.** Also scan for `0x21218b8a`
(slot 1). This is the direct, unambiguous read of the render-target address
that has been missing.

### 3.3 The upstream allocation

`MTXGLXAllocateClientBuffer` (`mtx_drv.so +0x4ddf0`) only allocates a one-page
type-3 (PCI locked) control buffer — it is not the colour buffer. The colour and
depth surfaces are ordinary `MmSurface`s created through
`MmSurfaceManager::AllocateSurfaceBlock` -> `OsAllocSurfaceBlockMemory`
(ioctl `0xc0804618` with `arg[0x10] == 0`, kernel handler `mtx1545` `+0x12a0`,
allocator `mtx3114`), attached with `MmSurface::AttachToBlock` (§1), and landing
in the `0xb28`/`0xb38` shadow that `mtx2372` snapshots. Client-side there are no
relocations at all: a bind record names `(slot, handle, offset)` and the kernel
resolves it (`parhelia-buffer-binding.md`).

---

## 4. DWGCTL on the 3D path: `[11:9]` is a constant, not a slot  [PROVEN]

For 2D, `SurfDesc2d::Init` `.text+0x7bc80` is unambiguous — a four-way switch
storing the DWGCTL bits next to that slot's packet header:

```
slot 0:  0x000   header 0x21218988   ; 0x1220 / 0x1224
slot 1:  0x400   header 0x21218b8a   ; 0x1228 / 0x122c
slot 2:  0x800   header 0x21218d8c   ; 0x1230 / 0x1234
slot 3:  0xc00   header 0x21218f8e   ; 0x1238 / 0x123c
```

`bits = slot << 10`, and slot N is at `0x1220 + N*8`. Confirmed, not corrected.

The **3D** draw validator `mtx3712` builds its DWGCTL differently
(`.text+0x18b8b` onward), and ends with an unconditional constant:

```
 18be5:  edx  = (state->[3] >> 1) & 0xf          ; opcode, bits [3:0]
 18bf6:  edx |= ((state->[3] >> 5) & 1) << 22
 18c0d:  edx |= 0x80000        unless (rcx[0x5c] & 0x20)
 18c34:  edx |= 0x200000       when the mode word is neither 0 nor 2
 18c46:  or   0x18(%rax),%edx                    ; pixel-state bits (BOP etc.)
 18c49:  or   $0x860e00,%edx                     ; <-- UNCONDITIONAL CONSTANT
 18c63:  mov  %edx,0xb08(%rax)                   ; -> DWGCTL shadow (0x1300)
```

`0x860e00` covers bits 23, 18, 17 **and 11, 10, 9**. The captured value decomposes
exactly:

```
0x00a60ec4  ==  0x860e00 | 0x200000 | 0xc4
```

Nothing per-draw ever varies bits `[11:9]` on this path, so they cannot be
selecting a target. The "destination slot 3" reading is an artefact of applying
the 2D field map to a 3D opcode; slot 3 is not involved.

Cross-check: the WARP-assisted clear `mtx1192` `.text+0x3dcb0` writes
`DWGCTL = 0x005000f5` (opcode 5, BOP 0xF = GXset) with bits `[11:9] = 0`, and it
binds **slot 0** (header `0x8988bfb9`). A clear targets the colour buffer — so
colour = slot 0, from a second, independent site.

Decode of `0x00a60ec4`:

| bits | value | meaning |
|---|---|---|
| [3:0] | 4 | 3D primitive opcode (`(state[3] >> 1) & 0xf`) |
| [7:4] | 0xC | BOP, GXcopy (from the pixel state's `+0x18`) |
| [11:9] | 0b111 | part of the fixed `0x860e00` — **not a slot field** |
| 17, 18 | 1, 1 | part of the fixed `0x860e00` |
| 21 | 1 | set when the mode word is neither 0 nor 2 |
| 23 | 1 | part of `0x860e00`; matches the scissor being enabled |

---

## 5. The render target is tiled: 64 x 32 pixels, 8192 bytes  [DERIVED, exact]

### 5.1 The measurement

A GL client rendering a magenta triangle changed exactly 16128 dwords, spanning
`0x2fa1020 .. 0x3095f7c`, for a triangle 224 px wide and 144 rows tall
(224*144/2 = 16128 — the whole footprint is triangle pixels, 32 bpp).

Span = `0x3095f7c - 0x2fa1020` = **1 003 356 bytes**.

No linear raster explains that. With 143 row steps and a column delta bounded by
+-223, a linear 32 bpp surface would need a stride of 7012, 7016 or 7020 bytes
(1753/1754/1755 px) — no plausible pitch, and neither 7680 (1920 px) nor 8192
(2048 px) fits at all.

### 5.2 The layout that fits, to the byte

```
tile          = 64 pixels wide x 32 rows = 8192 bytes at 32 bpp
in-tile order = row-major:  idx = (y & 31) * 64 + (x & 63)
tiles         = row-major:  tile_row_stride = (W / 64) * 8192
```

```
addr(x, y) = base
           + (y >> 5) * (W/64) * 8192
           + (x >> 6) * 8192
           + ((y & 31) * 64 + (x & 63)) * 4
```

Applying it to the two measured endpoints (mask `0x1FFF` = one tile):

```
0x2fa1020 & 0x1FFF = 0x1020 -> idx 1032 -> y0 & 31 = 16,  x0 & 63 =  8
0x3095f7c & 0x1FFF = 0x1F7C -> idx 2015 -> y1 & 31 = 31,  x1 & 63 = 31

y1 - y0 = 143  =>  (16 + 143) & 31 = 31                      consistent
tile delta = (0x3094000 - 0x2FA0000) / 8192 = 122
             = 4 tile rows * 30 tiles/row + 2 tile columns
   =>  W = 30 * 64 = 1920 px,  x1 - x0 = 2*64 + 31 - 8 = 151  (<= 223, fits)

span = 4*245760 + 2*8192 + (2015 - 1032)*4
     = 983040 + 16384 + 3932
     = 1 003 356 bytes                                        EXACT
```

The only other solution is `W = 1984` (`x1-x0 = -105`); 1920 is the desktop
width and the pitch every observed cfg carries, so `W = 1920`.

### 5.3 Why this is the right reading and not a curve fit

* The allocator's **height granularity for types 2/3 is 32 pixels**
  (`GetHeightGranularityREVB` `.text+0x74360`) — exactly the tile height.
* The allocator's **local alignment for a 32 bpp type-2/3 surface is 0x2000**
  (`GetLocalAlignmentAndGranularityREVB` `.text+0x74170`) — exactly one tile.
* `roundup(pitch,32) * roundup(height,32) * bpp/8` for 1920x1200x32 is
  `0x8E8000`, and `0x01000000 + 0x8E8000 = 0x018E8000` is one of the observed
  bases (§1.1).
* The vendor exposes tiling as a driver option: the parameter strings
  `UseTextiled`, `Force3DTextureTextiledToABSundog`,
  `Force3DTextureTextiledToCDSundog` are in `mtx_parhelia.o`.
* 64 px * 4 B = 256 bytes per tile row — a natural burst for the 256-bit bus.

The tiling selector is presumably cfg bits `[31:30]`, which are 0 in every
*captured* descriptor — but every captured descriptor is a 2D/desktop surface,
and the 3D target's cfg has never been seen (§3.2). Reading it out of the
primary buffer settles it. [OPEN]

### 5.4 Base of the measured target

```
base = 0x2FA0000 - (y0 >> 5) * 245760 - (x0 >> 6) * 8192,   with y0 & 31 = 16, x0 & 63 = 8
```

e.g. `base = 0x02F00000` puts the drawable's triangle at `(x0,y0) = (1288, 80)`;
`base = 0x02E00000` puts it at `(1800, 208)`. Only the primary-buffer read (§3.2)
pins it, and it is per-boot anyway.

---

## 6. Pointing the 3D destination at a buffer we choose

Everything needed is a register write we can make ourselves — there is no
context-only channel.

1. **Allocate.** Pick a VRAM region aligned to **0x2000**, of size
   `roundup(W,32) * roundup(H,32) * 4` (1920x1200 -> `0x8E8000`). Do the same for
   a depth surface if the draw has depth enabled (the captured DWGCTL's opcode-4
   path does; a Z surface at the same geometry is the safe choice).
2. **Emit the binding packet first in our own stream**, exactly as `mtx2954`
   does — one header-ful packet, indices LSB-first:

   ```
   header 0x8d8c8988   data:  colour_base   ->  0x1220
                              colour_cfg    ->  0x1224
                              depth_base    ->  0x1230
                              depth_cfg     ->  0x1234
   header 0x21218b8a   data:  other_base    ->  0x1228
                              other_cfg     ->  0x122c
                              (pad, pad)    ->  index 0x21 = 0x1084
   ```

   In our ring framing that is one header dword plus four data dwords per
   packet, unused lanes padded with index `0x21`; the leading count dword seen in
   the kernel builder is its own bookkeeping, not part of the GPU packet.
3. **cfg** = `pitch_px | (2 << 16) | (fmt << 24) | (tiling << 30)`. Start from
   the value read out of the vendor's primary buffer (§3.2). If that is
   unavailable, try `0x32020780` first and check the footprint against §5.2 — if
   it comes out linear rather than 64x32-tiled, the tiling bits are what differ,
   and they are the only unknown in the word.
4. **Leave DWGCTL alone.** `0x00a60ec4` needs no change: bits `[11:9]` are the
   fixed `0x860e00` constant, so there is nothing there that has to be
   re-pointed (§4).
5. **Match the raster state to the new target**: scissor `0x1328`/`0x132c` and
   the viewport constants `c[0x160]`/`c[0x161]` are in target coordinates.
6. **Judge the result with the tiled address formula of §5.2**, not with a
   linear one — a linear scan of the expected rectangle will miss.

### Order of work

1. **Read the primary buffer** for `0x8d8c8988` and take the four dwords. One
   scan, no hardware risk, and it hands over the exact base, cfg (including the
   tiling bits) and the depth descriptor the vendor uses. Do this first.
2. Re-emit that same packet at the head of the replay with the *vendor's* values
   and confirm the replay still rasterises with the client's own surface. That
   isolates the binding as the blocker.
3. Swap in our own base and re-run. Check the footprint against §5.2.
4. Only then vary cfg (tiling bits, type 2 vs 3) to confirm §5.3.

---

## 7. Proven / inferred / open

**PROVEN**

* Base register = allocator byte offset OR memory-class tag; no shift, no table,
  no MMU (`MmSurface::AttachToBlock`, `Surface2d::Enable`,
  `DeviceBitmap::ProgSurfDesc`, kernel `mtx1132`,
  `HSLPAR_Register_PriSurfAddr_Set`).
* Slot N is `0x1220 + N*8`; DWGCTL 2D slot fields are `[11:10]` dst / `[9:8]` src
  (`SurfDesc2d::Init`).
* The 3D DWGCTL builder ORs the constant `0x860e00`;
  `0x00a60ec4 = 0x860e00 | 0x200000 | 0xc4`. Bits `[11:9]` select nothing on the
  3D path.
* The 3D colour target is slot 0 and depth is slot 2, bound by the kernel via
  `mtx2372` -> `mtx2954` into the primary stream (packet header `0x8d8c8988`),
  never by the client.
* cfg field layout, the type table (granularity / alignment / aperture pitch),
  and the format/bpp classes.
* Allocation size `= roundup(pitch,32) * roundup(height,32) * bpp/8 * layers`,
  aligned to 0x2000 for 32 bpp types 2/3 — independently confirmed by
  `0x01000000 + 0x8E8000 = 0x018E8000` appearing in the capture.

**DERIVED (exact fit to hardware, not read out of a binary)**

* The 64 x 32 pixel / 8192-byte tiled layout of the 3D render target, and
  `W = 1920` for the measured surface.

**OPEN**

* Which cfg bits carry the tiling selector (expected `[31:30]`) and its value for
  the 3D target — settled by the primary-buffer read.
* Whether the depth surface uses the same tile geometry.
* Format codes `0x38 / 0x3a / 0x3b`; the meaning of DWGCTL bits 19 and 21.
