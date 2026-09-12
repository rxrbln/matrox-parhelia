# Vendor alpha-test and RGBA-texture emission audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a binary-only audit of the saved Parhelia vendor stack.  It separates
facts that are safe to use in an open driver from the still-missing P690
sampler-alpha route.

## Exact alpha-test emitter

`MTXPARD.dll` VMA `0x9c3e8..0x9c462` is the hardware-shadow packer.  The
disassembly establishes all of the following.

* The `DL & 0x40` dirty selector reaches the alpha-test branch.
* The enable bit is set or cleared in shadow-object `+0x18`.
* The comparison enum at device `+0x38` is translated by
  `MTXPARD!0x9a980`, then shifted left by one into bits `[3:1]`.
* The reference byte at device `+0x34` is stored at shadow `+0x19`, which is
  `0x12e4[15:8]`.  The next two bytes are cleared.
* Windows ORs `0xff000000` into its shadow image.  This mask differs from the
  Linux/open-driver image, which writes zero in `[31:16]`; it is not required
  for the hardware-proven target result.
* It sets dirty bit `0x40000000` in the owning fragment object.

`MTXPARD!0xc9330..0xc948d` flushes that object with the fixed header
`0xbbbcb9ba`.  In packet order, the header maps its four data words to:

| word | register | shadow offset |
|---|---:|---:|
| 0 | `0x12e8` blend control | `+0x0c` |
| 1 | `0x12e4` alpha test | `+0x18` |
| 2 | `0x12f0` blend constant BA | `+0x14` |
| 3 | `0x12ec` blend constant RG | `+0x10` |

The comparison translator at `0x9a980..0x9a9cb` produces the predicate-mask
encoding `NEVER=0`, `EQUAL=1`, `LESS=2`, `LEQUAL=3`, `GREATER=4`,
`NOTEQUAL=6`, `GEQUAL=5`, and `ALWAYS=7`.  Therefore the portable register
formula is:

```
0x12e4 = enabled | (predicate << 1) | (reference8 << 8)
```

The saved `gl-matrix` alpha operation has the exact final fragment packet:

```
header  0xbbbcb9ba
0x12e8  0x00800202
0x12e4  0x00008009   /* enabled, GREATER, 0x80 */
0x12f0  0x00000000
0x12ec  0x00000000
```

`captures/live-ring-0528/t-alpha-sec.hex` independently records `0x00008009`
for `GL_GREATER, 0.5` and `t-alpha_lt-sec.hex` records `0x00008005` for
`GL_LESS, 0.5`.  This proves the reference scaling, enabled bit, and compare
mapping independently of the Windows code.

## Measured RGBA MODULATE packet

The archived one-unit RGBA `GL_MODULATE` packet has a co-issued alpha half;
the packet does not expose a separate texture-alpha control register.

| stage | RGB (`0x1264`, `0x126c`) | Alpha (`0x1260`, `0x1268`) | meaning |
|---|---|---|---|
| texture-address slot 0 | `0x00004000`, `0x00000c00` | `0x00004000`, `0x00000c00` | plain unit-0 fetch |
| ALU slot 2 | `0x08000002`, `0x01400000` | `0x08000000`, `0x01400000` | `MUL_sat r0, t0, v0` |

Slot 0 is the texture-address stage, so the ALU is slot 2.  The alpha source
word clears only the RGB slot-address low bits; both halves multiply sampled
texture and primary vertex colour.  The alpha result is consequently
`textureA * vertexA` when the sampler supplies texture alpha.

For comparison, an RGB texture in `GL_REPLACE` has a distinct alpha
instruction: RGB is `MOV r0,t0` while alpha is `MOV r0,v0`.  The packet
comparison distinguishes the resulting programs; it does not by itself prove
the compiler branch that chose them.  There is no safe reason to turn an
A8R8G8B8 descriptor into XRGB.

The sampler state around the captured RGBA path is also known:

```
0x1200 = 0x00040101       /* select/latch unit 0, enabled 2D */
0x11e0 = 0x32020040       /* 64-pixel A8R8G8B8, vendor layout 2 */
0x11e8 = 0x00500000       /* one-stage cache image */
```

`0x1200` must be emitted before its shared `0x11c0..0x11e4` sampler window.
The same order is present in both `MTXPARD.dll` and the saved Linux vendor
kernel object.

## Negative result and implementation boundary

The archive has alpha-only `GL_ALPHA_TEST` captures and textured RGBA
`GL_MODULATE` captures, but no raw vendor draw that combines those two state
classes.  The component reconstruction above is nevertheless exact: the
current open Mesa path emits the same alpha-test word and the same co-issued
MODULATE pair.

On the P690, an isolated hardware test with those values draws the RGB of an
`A=0` A8R8G8B8 texture but lets it pass `0x12e4`; `vertexA=0` rejects the same
draw.  Keeping the vendor cache word and its disabled unit-1 reservation does
not change that result.  Thus storage, alpha test, vertex-alpha interpolation,
and the combiner are proven.  The unsolved component is specifically
sampler-alpha delivery on P690, likely a texture-address/context route not
contained in the captured per-draw sampler or combiner image.

Do not expose texture-driven alpha test or rely on this packet for alpha
cutouts until a controlled capture/probe identifies that route.  Vertex-alpha
blend remains supported because its source is independently hardware-proven.

## Archive cross-check: no composable alpha-texture packet

This additional local-only pass checked whether a previously overlooked saved
packet joins the separate alpha-test and texture captures.  It does not.  In
particular, an alpha-test word and a sampler-looking word merely being present
somewhere in the same trace file is insufficient: the saved captures contain
several setup and draw sections, and the relevant state must occur in the same
final draw section.

The most compact pair is under `captures/live-ring-0528`:

| artifact | final alpha state | texture state | conclusion |
|---|---|---|---|
| `t-alpha-sec.hex` | `0x12e4 = 0x00008009` | unit 0 remains the 1x1 default (`0x11e0 = 0x32020001`, `0x11e4 = 0x00010001`); combiner is `MOV r0,v0` | alpha-test-only draw |
| `t-alpha_lt-sec.hex` | `0x12e4 = 0x00008005` | the same default sampler and untextured combiner | alpha-test-only draw |
| `t-texture-sec.hex` | `0x12e4 = 0x0000000e` (disabled) | a 2x2 texture (`0x11e0 = 0x32020002`, `0x11e4 = 0x00010002`) and texture-address state | texture-only draw |

The raw dword offsets make the separation reproducible without trusting an
inferred state label.  In the first and third files, respectively:

```
                t-alpha-sec.hex          t-texture-sec.hex
0x144..0x149    default sampler           09006000 32020002 00010002
                                           01c01000 f0000000
0x20e           00008009                  0000000e
0x357..0x358    0000008a 00400400         00004000 00000c00
```

`0x00040103` occurs only in the latter 0528 texture path.  That is the
already-known PCI-X paired-pipe form, not a P690 candidate, and it never
co-occurs with an enabled alpha-test word in this controlled pair.  The ring
files for these cases have the same SHA-256 because they are an indirect
submission trampoline; their secondary sections, above, contain the state and
differ.  Thus their matching ring files cannot be used to fabricate a combined
packet.

The broader `captures/gl-matrix/alpha-secs.txt` gives the same result.  Its
400-dword alpha draw sections contain `0x12e4 = 0x00008009` with
`0x1200 = 0x0001000f` and `MOV r0,v0`; the nearby 36-dword texture-unit setup
sections only program the 1x1 default sampler.  The archive contains no
otherwise identical RGBA A=255/A=0 pair, no sample result, and no alpha-test
draw whose final combiner fetches texture alpha.  The P690-specific
`captures/ioq3-p690-working-draw.txt` does provide a key-`0x0e` stage image,
but neither the texture format/alpha-test state for that image nor an A=0
counterpart is captured.  It therefore cannot supply the missing delta.

### Static Windows cross-check

The only nearby Windows `MTXPARD.dll` helper that merited rechecking is at
VMA `0x17bbb0`.  It compares its call argument with shadow `+0xd50` and the
value at `+0x424` with `+0xd54`; if a change is needed it updates those shadow
members, `+0x1578`, and dirty masks `+0xcf0`, `+0xed4`, `+0xec0`, and `+0x14f4`.
The three static callers are `0x17bfc3`, `0x17c016`, and `0x17c072`.

This helper neither builds a register-list packet nor writes a sampler or
combiner word.  The actual alpha-test packet builder is separately at
`0x9c3e8..0x9c462`, and it serializes only the fragment shadow to the known
`0x12e4` word.  No saved capture connects a value reaching `0x17bbb0` to an
A8R8G8B8 A=0 result, a final sampler word, or a texture-alpha combiner input.
It is consequently only a context/dirty-state observation, **not** a
programmable sampler-alpha bit or a basis for a replay.

The missing evidence remains a controlled vendor P690 comparison with one
unit, `GL_MODULATE`, `GL_ALPHA_TEST(GREATER, .5)`, and matched RGBA A=255/A=0
uploads.  The capture procedure in `parhelia-vendor-capture-tooling.md`
exists for exactly that comparison.  Until it produces a same-draw final
packet or context delta, there is no verified additional P690 alpha route to
implement.
