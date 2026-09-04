# Matrox Parhelia — complete 2D acceleration map for EXA

Provenance: reverse-engineered from the on-disk proprietary driver
`matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` (not stripped,
demangled C++/`ParhlXAA*` symbols) and the DPS scaler classes in the same
object. All addresses below are **virtual addresses inside that `mtx_drv.so`**
(objdump `-d -C`); cross-checked against `7_4_99/mtx_drv.so`. Tags:
**[CONF]** = read directly from the disassembled emit path, **[PROB]** =
inferred from surrounding code. This fills the gaps left open by
`docs/parhelia-accel.md`; it does not re-derive what that doc already nailed
(DMA ring, DWGCTL 0x5c0 solid, 0x4c2 textured copy, surface descriptors).

Engine register model (unchanged): MMIO offset = `0x1000 + (index<<2)`; DWGCTL =
index 0xC0 = 0x1300. Direct-MMIO replay writes `0x1000+idx*4` in packet order;
the final geometry write (LEN/LINE_END) fires the op with no doorbell.

## The DrawContext shadow struct (all XAA ops)

Every `ParhlXAASetupFor*` writes a shadow at `surfpriv + 0xa68` that the flush
(`HwThread2d::SetupHardware`) streams as tag+data packets. Layout recovered
[CONF, from Setup{SolidFill@0x536e0, ScreenToScreenCopy@0x551e0, Mono8x8@0x53be0}]:

| Shadow offset | Meaning | Emitted to |
|---|---|---|
| +0x08 (byte) | emit-flags: which reg groups to flush (`\|0x30` fg+mask, `\|0x80` fg/bg pair, `\|0x50`/`\|0x20`/`\|0xa0` pattern, `\|0x10` scissor) | — |
| +0x09 (byte) | emit-flags bit0=colour, bit2=scissor-armed | — |
| +0x18 | **DWGCTL** value | 0x1300 (idx 0xC0) |
| +0x24 | **PLNWT** planemask (bpp-replicated) | 0x12C0 (idx 0xB0) |
| +0x28 | **BCOL** background (bpp-replicated) | 0x1318 (idx 0xC6) |
| +0x2C | **FCOL** foreground / key colour (bpp-replicated) | 0x1314 (idx 0xC5) |

Colours are replicated across the dword by bpp before storing: 8bpp
`c|c<<8|c<<16|c<<24`, 16bpp `c|c<<16`, 32bpp verbatim (branch on
`surf+0x128 == 8/16/32`). DWGCTL is always preceded by an idle load `0x707`
(packet header `0x2121c0c0` → two writes to idx 0xC0: `0x707` then the value)
[CONF @0x53fda / mtx642].

---

## 1. ROP / raster-op — CONFIRMED complete

- **`g_adwParhlXaaBopTable` @0x333E00** = 16 dwords, dumped verbatim:
  `00 08 04 0c 02 0a 06 0e 01 09 05 0d 03 0b 07 0f`. This is the 4-bit
  **bit-reversal** of the X GX rop index (`bop = table[alu&0xf]`).
- The BOP nibble sits in **DWGCTL bits [7:4]** (`bop<<4 & 0xF0`). [CONF @0x5386e,
  @0x55370, @0x53d06].
- X only defines 16 GC alu ops; there is no 256-entry ROP3 table in the X path
  (Windows D3D's ROP3 codes collapse to this 4-bit BOP plus the pattern/bg
  select). So the 16-entry table **is** the complete ROP support for EXA.
- Per-op DWGCTL assembly (low bits) [CONF]:
  - SolidFill: `dstSlot | (bop<<4) | (clip?0x800000:0)`  (@0x538a4)
  - Copy/BitBlt: `dstSlot | (srcSlot>>2) | 0x2 | (bop<<4) | dir | trans | clip`
    (@0x553a1)

`PrepareSolid`: `dwgctl = parhelia_bop[alu]<<4`; `FCOL=fg`, `PLNWT=planemask`.
`PrepareCopy`: add `0x2 | (srcSlot>>2) | srcSlotBit`. (Current `parhelia_exa.c`
is already correct here.)

## 2. Planemask / write-mask — CONFIRMED

- Register **PLNWT = idx 0xB0 = 0x12C0**. A non-trivial planemask is the
  bpp-replicated mask stored to shadow +0x24 and streamed to 0x12C0; `0xFFFFFFFF`
  = write all planes. [CONF @0x5531b path and shadow flush].
- Solid uses the mask directly; Copy forces **BCOL=0xFFFFFFFF** (@0x5542e) and
  puts the key colour in FCOL (see §4). The engine default should be programmed
  once to `0x12C0 = 0xFFFFFFFF` (already done in `ParheliaExaInit`).

## 3. Pattern / stipple (mono 8×8) — CONFIRMED (capture-only for EXA)

`ParhlXAASetupForMono8x8PatternFill @0x53BE0`, Subsequent `@0x53F10`:

- The pattern is a **hardware offscreen surface**, not 8 data dwords. It is bound
  through the surface-descriptor slot table: `SurfDesc2dList::AssignCurrentSurfDescTo`
  gives a slot, the pattern surface VRAM base goes to the slot's base reg
  (0x44+slot*4 emit array), config to (0x54+slot*4). [CONF @0x53ccd-0x53cf7].
- **DWGCTL opcode bits**: opaque `\|0x2000`; transparent (bg == -1) `\|0x3000`
  (adds the transparency bit) [CONF @0x53d2e / @0x53e75].
- **Pattern origin** is OR'd into DWGCTL per-rect at draw time:
  `(patx<<14 & 0x1C000) | (paty<<17 & 0xE0000)` [CONF @0x53ff6-0x5401e / mtx642].
- FCOL = fg, BCOL = bg (both bpp-replicated), PLNWT = mask.
- Geometry: packet `0x21e3c2c0` → DWGCTL, DSTXY(0x1308)=(y<<16)|x, LEN(0x138c)=
  (h<<16)|w.

EXA note: `PrepareSolid` with `Solid` alone cannot express this — it needs an
offscreen VRAM copy of the 8×8 pattern. **Deferred / capture-only**: implement
only after a scratch-VRAM allocator exists; falls back to software fill meanwhile.

## 4. Transparency / colour-key in blits — CONFIRMED

From `ParhlXAASetupForScreenToScreenCopy @0x551E0` (args: xdir, ydir, rop,
planemask, **trans_colour**):

- If `trans_colour != 0xFFFFFFFF` → **DWGCTL `\|= 0x400000`** (transparency /
  colour-key enable) [CONF @0x553e9].
- The key colour is bpp-replicated and written to **FCOL (0x1314)** (shadow
  +0x2C); **BCOL forced to 0xFFFFFFFF** [CONF @0x55429 / @0x5542e].
- Direction bits (from sign of the walk): **`0x1000` = X reverse** (xdir<0,
  @0x55505 `or $0x10,%ah`), **`0x2000` = Y reverse** (ydir<0, @0x554f5
  `or $0x20,%ah`); the Subsequent walk then starts at the far edge.
- Opaque copy = no `0x400000`; key colour path is the only "transparent-blit"
  the 2D engine offers (no separate chroma register — the key lives in FCOL).

EXA: an opaque `Copy` sets DWGCTL as today. A colour-keyed copy (rare in EXA)
would add `0x400000` and load FCOL with the key.

## 5. Render / Composite (alpha blend) — CONFIRMED **NOT** accelerated

Determined by exhaustive read of the DPS path and a negative search:

- `CDPSSurface::Fill @0x6E9E0`, `Blit @0x6F020`, `Stretch @0x6EC00`, and the
  pixel-shader/texture-engine `UpdateHardware` helpers **never emit the blend
  equation register 0x12AC nor a blend-factor bank** (`grep 12ac` in the whole
  object yields only code addresses, no `movl ...,0x12ac`).
- `CDPSSurface::Blit` is a straight **opaque textured rectangle** (DWGCTL=0x4C2,
  src+dst descriptors, SRCXY 0x1304, DSTXY 0x1308, LEN 0x138C, then PostBuffer)
  [CONF @0x6F020]. No source-alpha, no Porter-Duff.
- The DPS pixel shader (`BuildColorConvertShader`) does **colour-space
  conversion (YCbCr→RGB) only** — used by Xv, not alpha compositing.
- Conclusion: the vendor X driver does **software** RENDER/Composite. Hardware
  Porter-Duff would require the WARP/3D blend unit (blend-eq 0x12AC + the
  factor bank keyed `0x5`, base still undetermined — see `parhelia-accel.md`
  §Blend). That is a separate 3D effort, **capture-only** for now.
- Practical EXA guidance: **do not** advertise `PrepareComposite`; let EXA fall
  back to software. A future component-alpha path must go through 3D, not DPS.

## 6. Scaled blit / stretch (Xv, and any HW composite-scale) — CONFIRMED regs

`CDPSSurface::Stretch @0x6EC00` (source→dest with independent X/Y scale). It
builds one command buffer, then PostBuffer. Emit order [CONF]:

| Reg | Value | Note |
|---|---|---|
| 0x1300 | `0x1400CC` or `0x1C00CC`, `\| (activePipeMask<<8)` | stretch/textured opcode; base chosen by `hwrev+0x180` (>5 ⇒ 0x1C00CC) |
| 0x12F4 | `0x41700000` (=15.0f) | filter/LOD constant, only if filtering flag `this+0x10`≠0 |
| 0x1220 / 0x1224 | src base (`this+0x80`) / src cfg (`this+0x84`) | texture source surface |
| — | `CDPSTextureEngine::UpdateSrcSurfDesc` | binds src as texture |
| 0x12C0 | 0xFFFFFFFF | PLNWT |
| 0x12C4 | 0x7FFFFFFF | |
| 0x12E8, 0x12C8, 0x12FC, 0x12E4 | 0 | aux combiner/coord clears |
| 0x1324 | 0xFFFFFFFF | default/const colour (also seen in WritePixmap) |
| — | `CDPSPixelShader::UpdateHardware` | |
| — | `CDPSTextureEngine::UpdateHardware` | |
| — | `CDPSTextureSetup::UpdateHardware` | **emits the scale** |
| 0x1300 | `0x307` | launch/draw trigger |
| 0x12BC | sync tag (×2, from `hwbuf+0x40+4`) | DWGSYNC |

- **The X/Y scale factors are NOT a dedicated integer register.** They are the
  per-scanline **S/T coordinate DDA increments** = `srcW/dstW`, `srcH/dstH`
  (IEEE-754 floats), computed by `CDPSTextureSetup::ComputeIncrements` and
  emitted by its `UpdateHardware` to the texture-setup coordinate block
  **0x1160–0x1198** (S/T/W origins + increments; see `parhelia-accel.md` DPS
  section). This is why an integer "scale register" was never found.
- `SetSrc(LRECT&,LRECT&)`/`SetDst(LRECT&)` on `CDPSTextureSetup` define the src
  and dst rectangles that drive those increments.

EXA/Xv: a scaled blit = bind source as texture, set src/dst rects, let the
texture-setup DDA carry the ratio, opcode 0x1400CC/0x1C00CC, launch 0x307. This
is the same machinery Xv uses; a `Composite` with only scaling (no blend) could
ride it, but plain `Copy`/`UploadToScreen` should stay on the cheap 0x4C2 path.

## 7. Upload / Download (host ↔ VRAM) — CONFIRMED: no ILOAD port

- **There is no host-data / ILOAD FIFO for pixel upload.** `ParhlXAAWritePixmap
  @0x55780` calls the helper `XferNativeToVid`; `ParhlXAASyncCPUToScreenColorExpandFill
  @0x54570` allocates a scratch surface (`TempBitBuffer::AllocSurfaceGen`), maps
  it with **`MmHostAperture::Lock`** (returns a CPU pointer), CPU-copies the host
  bits in, then submits a normal engine blit. [CONF: the Lock/Unlock + blit
  sequence @0x548e6…0x54dea].
- The `0x384` write at the tail of that function (`memWriteDword esi=0x384`) is
  the **pushbuffer FIFO kick**, not a pixel port. `0x1300` appears only as the
  DWGCTL index in the replayed packet.
- Therefore EXA `UploadToScreen`/`DownloadFromScreen` as a **plain `memcpy` to /
  from the mapped framebuffer aperture is faithful** to the vendor design — the
  current `parhelia_exa.c` implementation is correct and needs no engine work.
- The only thing that genuinely needs a scratch VRAM surface is **mono→colour
  expansion** (`CPUToScreenColorExpandFill`, DWGCTL opcode `0x800303`, transparent
  adds `0x1000` @0x54333/@0x54487): copy the mono bitmap into an aperture-mapped
  scratch surface, then colour-expand-blit it. Deferred like §3.

## 8. Clipping — CONFIRMED

`ParhlXAASetClippingRectangle @0x535E0` / `DisableClipping @0x53660`:

- 2D scissor registers: **MIN = 0x1328** `(y1<<16)|x1`, **MAX = 0x132C**
  `(y2<<16)|x2` (distinct from the 3D NDC scissor 0x1280/0x1284).
- Arming sets shadow **DWGCTL `\|= 0x800000`** (bit23 clip-enable) and the
  emit-flag bits (+0x09 `\|0x4`, +0x08 `\|0x10`) so the flush streams the scissor
  [CONF @0x53618/@0x53635]. `priv+0x10` = clip-armed flag.
- Disable resets scissor to `(0,0)`…`(0xFFF<<16)|0xFFF` and clears bit23 + flags
  [CONF @0x53662-0x536b2].
- **Rule:** never set `0x800000` without programming a scissor, or the op clips
  against stale limits. `ParheliaExaInit` already pre-programs a full-range
  scissor; per-op clipping (EXA sets one clip rect) = write 0x1328/0x132C then
  OR 0x800000 into DWGCTL.

## 9. Engine idle / sync — CONFIRMED, fastest path

- **`0x8F0` bit17 (0x20000) = busmaster idle/complete** — poll for fast sync
  (current `parhelia_wait_idle`).
- `0x41C` = DMA GET pointer (`ParhlXAASync` polls it, masked `&~7`); `0x450` =
  drawing-progress fence (DMA path). `0x384` = FIFO data-port / kick.
- DWGCTL idle value = `0x707` (soft-trap prewrite before every DWGCTL). For a
  direct-MMIO driver the geometry write already serializes; poll `0x8F0` bit17
  before reusing surface descriptors.

---

## EXA hook → register sequence (implementable summary)

All by direct MMIO to `0x1000+idx*4`; colours pre-replicated per bpp.

| Hook | Sequence (in order) | Tag |
|---|---|---|
| **PrepareSolid**(alu,pm,fg) | shadow: DWGCTL=`bop[alu]<<4`, FCOL=fg, PLNWT=pm | CONF |
| **Solid**(x1,y1,x2,y2) | wait 0x8F0.17; SURF0 base+cfg; PLNWT; FCOL; DWGCTL=0x707; DWGCTL=shadow; DSTXY=(y1<<16)\|x1; LEN=((y2-y1)<<16)\|(x2-x1) | CONF |
| **PrepareCopy**(alu,pm,dir) | DWGCTL=`bop[alu]<<4 \| 0x2 \| (srcSlot>>2) \| srcSlotBit`; +`0x1000` if xdir<0, +`0x2000` if ydir<0; PLNWT=pm | CONF |
| **Copy**(sx,sy,dx,dy,w,h) | wait; dst SURF0, src SURF1 base+cfg; PLNWT; DWGCTL=0x707; DWGCTL=shadow; LINE_START(0x1304)=(sy<<16)\|sx; DSTXY(0x1308)=(dy<<16)\|dx; LEN=(h<<16)\|w. (reverse dirs start at far edge) | CONF |
| **Copy (colour-keyed)** | as Copy + DWGCTL\|0x400000, FCOL=key, BCOL=0xFFFFFFFF | CONF |
| **PrepareComposite / Composite** | **do not implement** — DPS has no blend; use EXA software (HW needs 3D WARP blend, capture-only) | CONF (neg) |
| **UploadToScreen** | `memcpy` rows into fbBase+offset (aperture); no engine | CONF |
| **DownloadFromScreen** | `memcpy` rows out of fbBase+offset | CONF |
| **(Xv/scaled blit)** | bind src as texture (0x1220/0x1224 + UpdateSrcSurfDesc); SetSrc/SetDst rects; TextureSetup DDA → 0x1160-0x1198; DWGCTL=0x1400CC\|(pipes<<8); PLNWT=0xFFFFFFFF; launch DWGCTL=0x307; DWGSYNC 0x12BC | CONF regs / DDA=float |
| **Clip (optional)** | SCISSOR_MIN 0x1328=(y1<<16)\|x1; SCISSOR_MAX 0x132C=(y2<<16)\|x2; DWGCTL\|0x800000 | CONF |
| **Mono8×8 pattern** | needs offscreen pattern surface; DWGCTL\|0x2000 (opaque)/0x3000 (transp) \| patOrigin; FCOL=fg,BCOL=bg — **deferred** | CONF |
| **ColorExpand** | copy mono bits to aperture scratch surface; DWGCTL opcode 0x800303 (+0x1000 transp); blit — **deferred** | CONF |

## Residual capture-only items

1. **Mono 8×8 pattern fill** and **CPU→screen colour-expand**: register encodings
   fully recovered, but both need a scratch/offscreen VRAM surface + on-HW
   validation of the pattern-origin / reverse-walk. Software fallback until then.
2. **Hardware Composite (Porter-Duff, component-alpha)**: absent from the 2D/DPS
   path by design; requires the 3D WARP blend unit — blend-eq 0x12AC is known,
   but the **blend src/dst factor register bank (WriteBuffer key 0x5) base is
   still undetermined** and only resolvable from libGL's runtime dispatch, not
   statically. Large separate effort.
3. **Stretch DDA float values**: the scale ratios are computed in
   `CDPSTextureSetup::ComputeIncrements` and land in 0x1160-0x1198 as floats; the
   exact float layout per coordinate was taken as already-documented (DPS section
   of `parhelia-accel.md`) rather than re-derived here.
</content>
</invoke>
