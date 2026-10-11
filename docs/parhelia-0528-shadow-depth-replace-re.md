# Original Parhelia: shadow comparison and per-pixel depth replace (vendor RE)
Copyright 2026 René Rebe <rene@exactco.de>

Offline static RE of the Windows original-family stack (xddm32 MtxPar: MTXOGL.dll, MTXPARD.dll;
cross-checked against the Linux kernel object mtx_parhelia.o). PARD addresses use the disassembly base
0x10000. Labels: PROVEN = read in code, INFERRED = interpretation, OPEN = not settled.

## 1. GL_ARB_shadow (GL_TEXTURE_COMPARE_MODE / _FUNC, GL_DEPTH_TEXTURE_MODE)

- MTXOGL glTexParameter (0x103b7216): texture object +0x154 depth mode, +0x158 compare mode (0 or
  0x884E), +0x15c compare func (LEQUAL/GEQUAL, all eight with EXT_shadow_funcs). PROVEN.
- Sent as sampler records (opcode 6, 0x1006ea40, emitted 0x1039e3c0..0x1039e4a1): sub 0x0a image is
  depth, 0x0b depth mode (LUMINANCE 0, INTENSITY 1, ALPHA 2), 0x0c compare mode (0/1), 0x0d compare
  func as D3DCMP 1..8. PROVEN. The Linux kernel handler (mtx3081) dispatches only subs 0..9, which
  matches the Linux libGL not advertising ARB_shadow.
- MTXPARD evaluates the comparison in the **combiner**, not in the texture unit (PROVEN encodings,
  ALU meaning INFERRED):
  - cost estimator 0x1755a0: depth stage with compare and a free helper slot = flag 0x4000, four
    combiner slots; compare without helper = 0x8000; depth without compare = 0x2000;
  - 0x4000 inserts four instructions (0x1810d0, types 0x16..0x19): difference of depth and R, its
    sign scaled x8 into the helper register, T = 1.0, conditional move of the fail colour;
  - a helper texture stage (virtual stage 0x0b, op 0x40 -> address code 2 = TEXCOORD, 0x17704c) on
    hardware slot 0 carries the clamped coordinate (R) - INFERRED from the code value;
  - 0x2000 (no compare) applies the depth mode only: LUMINANCE (D,D,D,1), INTENSITY (D,D,D,D),
    ALPHA (0,0,0,D);
  - 0x8000 (no helper slot): the result is always "lit" (1.0, or (0,0,0,1) for ALPHA).
- Depth images are stored as D24X8 by default (format resolution 0x103fbba6). The sampler format code
  is OPEN (INFERRED 0x36, the S8Z24 depth-buffer code).
- Open driver: Mesa parhelia fce6856 implements the same structure with established forms (comparison
  lowered onto the combiner's CMP, R on a coordinate unit whose clamp saturates it); measured on the
  0528, depth-tex-compare 88/96 probes (LUMINANCE EQUAL/NOTEQUAL needs a fourth slot and is dropped).

## 2. TEXM3x2DEPTH: the per-pixel depth replace (ps.1.3, D3DSIO 0x54)

The vendor's ps.1.3 front end accepts TEXM3x2DEPTH (handler 0x188800) together with TEXM3x2PAD
(0x187a80); dead-code elimination (0x184c30) keeps it as side-effecting. All PROVEN unless noted.

- Operand rules: destination a t-register above the last written, full mask, no modifiers; source
  register < 4, no relative addressing, identity swizzle, modifier none or _bx2; the source must come
  from TEX or TEXCOORD. Exactly one TEXM3x2PAD must be pending.
- Texture-address opcode table (data 0x1d4488): PAD 0x47 -> 0x08, DEPTH 0x54 -> **0x0E**,
  TEXM3x2TEX 0x48 -> 0x0C. The kernel's GL table (mtx1411) maps its code 13 (TEXM3X2_DEPTH) to 0x0E.
- Stage setup: DEPTH is "must execute" (+0xa4), takes the TEXCOORD path (no texel fetch), forces a
  re-emit (+0x128), and sets a sticky "depth stage exists" flag (compiler+0x44 bit 10; kernel ctx+8
  bit 10 in mtx2255).
- Register words (validator 0x179ac0..0x17a18c), unit u = m+1 (DEPTH):
  - `0x1200 + 4u = 0x00040100 | pipes | mode << 16 | 0x0E << 19`, mode 6 (three coordinates) or 7
    (projected);
  - **original family (caps & 0x3c0): 0x1200 |= 0x10000000** (bit 28; MTXPARD 0x17a172, kernel
    0x72869);
  - LX family instead sets 0x11e8 |= 0x100 (0x17a161) - never on the original family;
  - `0x11e0 = 0x800` (sizes 1.0, Q.11 - INFERRED), `0x11e4 = 0x08000800`; no texture bound.
- Unit m (PAD): `0x1200 + 4m = 0x00040100 | pipes | mode << 16 | 0x08 << 19`, same 0x11e0/0x11e4.
- No write to the depth control (0x12c8), the Z surface (0x1228/0x122c) or DWGCTL depends on op 0x0E.
- Fallback 0x179b52: without a bound depth surface the vendor demotes DEPTH to TEXM3x2TEX (0x0C) and
  samples the stage's texture (INFERRED reading of C+0x8dc).
- OPEN: how the unit picks the dot operand tn (INFERRED: implicit by unit order), w = 0 handling, the
  pipe mask/parking of the PAD+DEPTH pair on the 0528, the consumer of the "depth stage" flag.
- Use: gl_FragDepth (piglit glsl-1.10-fragdepth, fbo-depth-sample-compare) could map to it; the form is
  unmeasured and needs an exclusive probe slot.
