# P690 sampled-RGBA alpha: cross-binary context audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This is an offline, bounded re-check of the saved P690 Q3 state, the Linux
vendor kernel object, and the Windows Parhelia D3D and OpenGL binaries. It
asks whether any saved context record or final packet gives a P690-safe way to
carry sampled texture alpha into the already-known `GL_SRC_ALPHA` blender.

No source, target, driver, module, or hardware state was changed.

Inputs checked:

| artifact | SHA-256 |
|---|---|
| `captures/ioq3-p690-working-draw.txt` | `4ade3269b3b6d482dd38c32778ef26dd5759cbaf9e34a05faef85168b2864768` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll` | `41b2317309801a917506c29c2b5998713ddb9bdfa3d26c623ab56617d0c1d848` |

## Result

There is no replayable P690 sampled-alpha control in the retained binaries or
captures. The evidence does establish all three necessary *logical* stages:

```
RGBA upload -> texture-stage alpha expression -> alpha fragment result
          -> BLENDCTL source-alpha factors
```

It does not join the middle two stages to a final P3D packet for a P690 draw.
The only complete final blend value is `0x12e8 = 0x0080eced`, which consumes a
source alpha that has already reached the fragment result. It is not a
sampler-alpha selector.

## 1. The retained working Q3 state is a context input

At state-submit 5, `ioq3-p690-working-draw.txt` has key `0x0e` at offset
`0x378`. Its stage-zero entries at `0x388..0x3b4` are:

| field | value | meaning |
|---:|---:|---|
| 1 | 4 | `COLOROP = MODULATE` |
| 2 | 2 | `COLORARG1 = TEXTURE` |
| 3 | 0 | `COLORARG2 = DIFFUSE` |
| 4 | 2 | `ALPHAOP = SELECTARG1` |
| 5 | 2 | `ALPHAARG1 = TEXTURE` |
| 6 | 0 | `ALPHAARG2 = DIFFUSE` |

Thus the vendor did accept the Q3 request `A = texture.A`; it was not using
vertex alpha as a substitute in this state image. The same state object has a
zero-valued key `0x46` at `0x170`, and the retained state submission contains
no final MMIO register list. In particular it carries no correlated writes for
`0x1200`, `0x11c0..0x11e8`, `0x1260..0x126c`, `0x12e4`, or `0x12e8`.
Key `0x0e` must therefore be treated as vendor context input, not as a P3D
packet that Mesa can submit.

## 2. Linux follows the input through an opaque sparse-map boundary

The Linux object stores the alpha triplet in a 0xa0-byte compiled stage record:

| raw descriptor offset | compiled-record offset | field |
|---:|---:|---|
| `+0x3c` | `+0x10` | `ALPHAOP` |
| `+0x44` | `+0x14` | `ALPHAARG1` |
| `+0x48` | `+0x18` | `ALPHAARG2` |

`mtx580` calculates the selected stage record as
`device + 0x434 + 0xa0 * stage`. `mtx3136` passes stage zero to `mtx950`.
The relocation-resolved code shows that `mtx950` invokes `mtx713` eight times:
the source advances by `0x74` per call and the destination map container by
`0x60` per call. `mtx713` copies one dword from a dynamically selected source
offset to a heap-backed node destination. Neither function contains a P3D
register address or identifies the destination used for fields 4--6.

The relevant instruction-level boundary is:

```
mtx580(stage) -> device + 0x434 + 0xa0 * stage
mtx3136       -> mtx950(context, stage_record)
mtx950        -> 8 * mtx713(context + 0x78 + 0x60*i,
                            stage_record + 0x74*i)
mtx713         -> copy source[node_id * 4] to dynamic heap node
```

This proves that the stage state is consumed; it does not yield an address or
packet value that can be replayed. The adjacent four-dword map update in
`mtx3136` instead reads `device+0xcc1/+0xcc5/+0xcc9/+0xccd`, so it cannot be
the serialization of the three alpha fields.

## 3. Windows independently lowers colour and alpha arguments, but exposes no sampler mux

`MTXPARD.dll` resets four `0x12c`-byte texture-stage descriptors and ten
`0x154`-byte intermediate instruction records at `0x1764d0`. Its
fixed-function lowering loop begins at `0x176620`; the shown path at
`0x1766d2..0x17685f` reads the stage operation and argument words, validates
the D3D source selector (`arg & 0x0f <= 5`) and modifiers
(`arg & 0xf0 <= 0x30`), then passes the values to the intermediate-instruction
builder. The alpha-side words are processed separately from their RGB
counterparts.

This independently corroborates the Q3 state interpretation: `TEXTURE` is a
valid alpha source and alpha is part of the fixed-function compiler input. No
`0x11c0..0x11e8`, `0x1200`, `0x1260..0x126c`, `0x12e4`, or `0x12e8` literal is
emitted by that lowering block. It constructs IR; a later, unretained
per-draw compilation/submission is required to identify the hardware route.

The Windows OpenGL ICD separately proves that `GL_RGBA` uploads preserve the
fourth byte: `MTXOGL.dll:0x103ab28f` branches on `GL_RGBA (0x1908)` and the
four-byte conversion path writes the rotated 32-bit source value without
forcing alpha to `0xff`. This is upload evidence only, not a sampler or
fragment-state write.

## 4. Known final packets are necessary but insufficient

The archive has a valid texture-address instruction and a separate alpha
instruction for `GL_MODULATE`:

```
0x1260 = 0x00004000;  0x1268 = 0x00000c00   # texture-address slot
0x1260 = 0x08000000;  0x1268 = 0x01400000   # saturated alpha MUL t0.a, v0.a
```

It also has the complete alpha-over blender state:

```
0x12e8 = 0x0080eced   # RGB/A: SRC_ALPHA, ONE_MINUS_SRC_ALPHA; ADD
```

These values establish that the combiner can request `t0.a` and that the
blender can consume fragment alpha. They are not a complete sampled-alpha
route on P690: the direct P690 A8R8G8B8 test with this alpha instruction and a
correct alpha comparator allowed an `A=0` texel to paint RGB, whereas changing
only vertex alpha to zero correctly rejected it. Therefore texture alpha was
lost before the established final fragment result.

The P690-safe sampler selection remains `0x1200 = 0x00040101`. The `03`
pipe-pair image from a PCI-X capture is not an alpha bit and wedges the P690;
it is excluded from all candidate routes.

## Implementation consequence

Do not add a native sampled-alpha Mesa path from this material. In particular,
do not inject key `0x0e`, reinterpret key `0x46`, alter the known alpha ALU
word, change `0x11e8`, or retry `0x00040103` as a texture-alpha experiment.

A native implementation needs one matched vendor P690 capture of otherwise
identical RGBA textures with A=255 and A=0, retaining the complete key `0x0e`
record, post-stage compiled record, `mtx713` node destinations, final key
`0x46` object, final sampler/combiner/alpha-test/blend packets, and a result
pixel. Only a correlated delta that rejects A=0 while preserving A=255 can
define a hardware implementation.

## Reproduction

```sh
objdump -dr -Mintel --start-address=0x14be0 --stop-address=0x14c20 \
  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -dr -Mintel --start-address=0x1b640 --stop-address=0x1b7a7 \
  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -D -b pei-i386 -m i386 --start-address=0x1764d0 --stop-address=0x176860 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
objdump -D -b pei-i386 -m i386 --start-address=0x103ab280 --stop-address=0x103ab3b0 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll
```

Related evidence: `parhelia-p690-sampler-alpha-emission-re.md`,
`parhelia-p690-texture-alpha-context-re.md`,
`parhelia-q3-alpha-stage-packet-route-re.md`, and
`parhelia-q3-textured-alpha-blend-route-re.md`.
