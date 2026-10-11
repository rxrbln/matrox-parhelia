# Slot to hardware: what the kernel does with GL state

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

A region-2 state slot never names a register. It writes a field in a software
shadow, and a validator later turns groups of those fields into register writes,
block loads, or a compiled shader. This is that mapping, from
`mtx_parhelia.o`.

## Dispatcher — `mtx2617` @0x19950

Records are `{u32 slot, u32 value}`.

    slot value cache : HWSTATE + 0x10 + id*4     (ids 0..0x100)
    lock bitmap      : HWSTATE + 0x414           (256 bits, immediately after)

* Sending id `0x100 + S` sets or clears the **lock** for slot S; a locked slot's
  writes are dropped.
* Unchanged values are dropped (`cmp [cache], value; je`), and each validator
  drops again per register.
* **Dispatch bound is `0xab`** -- the jump table at `.rodata+0xba0` is 172
  entries covering `0x00..0xAB`. Earlier notes here said `0x00..0xAC`.
* **Never emit slot 0**: it indexes the lock bitmap at word `0x07ffffff`.
* 54 ids are pure no-ops; ids `0xAC..0x100` are cached for `glGet` only.

## Three delivery mechanisms

| | destination | used by |
|---|---|---|
| **A** | register packet, `reg = 0x1000 + index*4` | depth/stencil, blend, alpha, colour mask, scissor, cull, fog |
| **B** | opcode-0x41 block load | raster, viewport, clip planes, polygon offset |
| **C** | **no register at all** -- a shader-program key | texture env, texture enables, lighting, fog-in-shader |

**Mechanism C is the important surprise.** Texture environment, texture
enables, the packed `[0xdc8]` field, texture-env colour and fog-in-shader are
**compiled into a WARP program**, not written anywhere. There is no register
path to copy: an open driver must generate the program itself.

## Register map (mechanism A)

### Depth / stencil -- validator `mtx360`

    0x12c8  bit0 depth test | bits1-3 depth func | bit4 stencil enable
            bits5-7 stencil func | bits8-10 fail | bits11-13 zfail | bits14-16 zpass
    0x12cc  bits0-7 stencil mask | bits8-15 stencil mask (second)
    0x12d0  bits0-7 stencil ref | bit8 depth write mask

Slots: 0x07 depth test, 0x17 depth func, 0x0e depth write, 0x34 stencil enable,
0x38 stencil func, 0x35/0x36/0x37 the three stencil ops, 0x39/0x3a masks,
0x3b ref. (Which of 0x35/0x36/0x37 is fail vs zfail vs zpass is inference from
field order; that all three are the `mtx1850` stencil-op inputs is confirmed.)

### Blend / alpha / mask / scissor -- validator `mtx2480`

    0x12e8  blend control (below)      0x12e4  alpha test
    0x12ec  blend constant, 10-bit     0x1324  colour write mask
    0x12f0  blend constant, 10-bit     0x1328 / 0x132c  scissor min / max
    0x12fc  cull/mode word

    0x12e8: bit0 enable | 1-4 srcRGB | 5-8 dstRGB | 9-12 srcA | 13-16 dstA
            17-19 alpha equation | 20-22 RGB equation (SAME value: no separate
            equation support) | 23,24 format-gated

    0x12e4: bit0 enable | bits1-3 func | bits8-15 reference, TRUNCATED to 8 bits

The scissor result independently reproduces `parhelia-raster-clip.md`'s
0x1328/0x132c -- two unrelated derivations agreeing.

## Value encodings

**Compare function** (`mtx1401` depth/stencil, `mtx1344` alpha -- byte-identical):
the hw code is a 3-bit predicate mask, **bit0 equal, bit1 less, bit2 greater**.

    NEVER 0  LESS 2  EQUAL 1  LEQUAL 3  GREATER 4  NOTEQUAL 6  GEQUAL 5  ALWAYS 7

**Stencil op**: `KEEP 0, REPLACE 1, INVERT 2, ZERO 3, INCR_WRAP 4, DECR_WRAP 5,
INCR_SAT 6, DECR_SAT 7`.

**Blend equation**: `ADD 0, SUBTRACT 1, REVERSE_SUBTRACT 2, MIN 5, MAX 6`.

**Blend factor**:

    0 ZERO             4 SRC_COLOR        8 DST_COLOR         c SRC_ALPHA_SAT
    1 ONE              5 INV_SRC_COLOR    9 INV_DST_COLOR     e CONST_ALPHA
    2 CONST_COLOR      6 SRC_ALPHA        a DST_ALPHA         f INV_CONST_ALPHA
    3 INV_CONST_COLOR  7 INV_SRC_ALPHA    b INV_DST_ALPHA

Colour factors degrade to their alpha equivalents on the alpha pipe. With no
destination alpha channel, `DST_ALPHA` becomes ONE and `INV_DST_ALPHA` ZERO.

**Logic op**: raw 4-bit truth table indexed by `(src<<1)|dst` --
`CLEAR 0, COPY_INVERTED 3, XOR 6, COPY 0xc, SET 0xf`.

## Validation order -- `mtx3712` @0x188f0

Fixed order, and it is validation order rather than call order:

    1  [0xc87]  mtx360   depth/stencil/surface/viewport
    2  [0xc08]  mtx2480  blend/alpha/logic-op/write-mask/scissor
    2b          mtx2215  (only if step 2 returned true) -- this is where CULL lands
    3  [0xd54]  mtx1906  fog
    4  [0x1b44] mtx3024
    5  [0xdbb]  mtx297   shader select/compile
    6           mtx3654  raster -> block file AND registers
    7           primitive / vertex format
    8           mtx412   clears every dirty word

**Steps 1, 2, 2b and 5 can abort the rest of validation** by returning false.
`mtx412` clears only dirty words; the slot cache and lock bitmap persist.

Note cull reaches hardware only through step 2b, which runs only if step 2
returned true.

## Behaviours worth knowing before copying this design

* **Slot 0x3e calls out of the module synchronously**, inside the record loop,
  through an indirect vtable call -- no shadow, no dirty flag, no register.
* **`mtx2319` re-enters the dispatcher and rewrites the caller's own state**,
  re-submitting slot 0xa8 or 0x0d so the write mask is re-expressed for a newly
  bound render target -- and it does so before the bits it flips are tested.
* **Blend is silently dropped whenever a non-COPY logic op is active.**
* **Slot 0x88 sets no dirty flag at all** -- a latent bug; its write only reaches
  hardware if another slot happens to dirty the same group.
* **`mtx698`, the first validator called, is an empty `repz ret`** -- the cull
  value passed to it is discarded there.
* Slots 0x0d and 0xa8 are mutually exclusive, each clearing the other's bit.
* Blend constant colour is 10-bit while the alpha reference is truncated to 8 --
  a precision mismatch to reproduce deliberately or not at all.

## Still open

* Slots 0x40..0x5f are **not** the user clip planes (those arrive at
  `HWSTATE+0x1da0` and reach block indices 0x16c..0x171 via slot 0x98). Their
  destination is unresolved.
* Slot 0x97's 3-bit field has a resolved encoding path but no known GL meaning.
* Fog's per-slot attribution within `devregs+0xa8` is mapped as a group but not
  per field.
