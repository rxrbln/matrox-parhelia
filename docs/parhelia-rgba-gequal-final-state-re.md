# A8R8G8B8 `GL_GEQUAL(.5)` final-state audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is an independent, local-only pass over the archived binary drivers and
captures.  It answers a narrow question: whether the archive contains a
complete, replayable final hardware state for an A8R8G8B8 texture whose sample
alpha is tested with `GL_GEQUAL, 0.5`.

It does not alter the target or any Mesa/kernel source.  Addresses below are
the loaded PE VMAs for `MTXPARD.dll` (its image base is `0x00010000`), except
where an `MTXOGL.dll` address is explicitly shown as an RVA.

## Result

There is **no complete final-state packet in the archive** for this case.  Two
parts are exact and independently corroborated:

* `0x12e4 = 0x0000800b` is the open-driver form of enabled
  `GEQUAL, ref=0x80`.
* The RGBA `GL_MODULATE` combiner consumes `t0` and `v0` for both RGB and
  alpha.

They were not captured together with an A=0/A=255 texture comparison.  The
P690 probe already shows that combining these known pieces admits an A=0 texel
through the alpha test while vertex alpha zero rejects it.  Therefore the
unknown is the P690 sampler/context route that delivers the texture alpha, not
the comparator word or vertex-alpha path.  This rules out exposing the state
as a normal Mesa alpha-cutout path.

## Comparator: exact `GEQUAL(.5)` word

`MTXPARD.dll:0x9c3e8..0x9c46a` selects this state when dirty selector
`DL & 0x40` is present.  The directly disassembled instructions establish:

| VMA | instruction effect | consequence |
|---:|---|---|
| `0x9c3e8` | `test dl, 0x40` | alpha-test state is a separate dirty class |
| `0x9c40e..0x9c414` | clear/set shadow `+0x18` bit 0 | alpha-test enable |
| `0x9c437..0x9c44e` | translate `device+0x38`, `add eax,eax`, masked XOR with `0x0e` | compare occupies shadow bits `[3:1]` |
| `0x9c451..0x9c457` | `device+0x34` to shadow byte `+0x19` | reference occupies bits `[15:8]` |
| `0x9c457` | clear shadow `+0x1a` | bits `[23:16]` are zeroed |
| `0x9c45b` | OR `0xff000000` | Windows-private high mask field |
| `0x9c462..0x9c465` | OR owner dirty word with `0x40000000` | schedule the fragment-state flush |

The translator at `MTXPARD.dll:0x9a980..0x9a9cb` maps the D3D comparisons to a
three-bit predicate mask:

```
NEVER 0  EQUAL 1  LESS 2  LEQUAL 3
GREATER 4  GEQUAL 5  NOTEQUAL 6  ALWAYS 7
```

So the low, portable word is:

```
alpha = enable | (predicate << 1) | (reference8 << 8)
      = 1      | (5 << 1)          | (0x80 << 8)
      = 0x0000800b                 /* GL_GEQUAL, 0.5 */
```

The Windows shadow adds `0xff000000`, producing `0xff00800b`.  The saved
Linux kernel/open paths leave that high field zero, and proven `GREATER,.5`
captures use `0x00008009`; the high field is consequently not a candidate
sampler-alpha control.

The flush at `MTXPARD.dll:0xc92f0..0xc9315` writes the four-word packet with
header `0xbbbcb9ba`, in this order:

| packet word | register | source shadow offset |
|---:|---:|---:|
| 0 | `0x12e8` blend | `+0x0c` |
| 1 | `0x12e4` alpha test | `+0x18` |
| 2 | `0x12f0` blend constant BA | `+0x14` |
| 3 | `0x12ec` blend constant RG | `+0x10` |

**Confidence: confirmed** for the register formula and flush order.  The
standalone archived alpha capture only proves `GREATER,.5 = 0x00008009`; the
`GEQUAL` value follows from the binary's explicit predicate table.

## RGBA MODULATE packet piece

The archived RGBA textured packet contains the following one-unit
`GL_MODULATE` combiner words:

| register | value | decoded operation |
|---:|---:|---|
| `0x1264` | `0x08000002` | RGB source: `MUL t0, v0` in ALU slot 2 |
| `0x126c` | `0x01400000` | RGB destination: saturated `r0` |
| `0x1260` | `0x08000000` | alpha source: same `MUL t0, v0` route |
| `0x1268` | `0x01400000` | alpha destination: saturated `r0.a` |

The texture-address stage precedes it at slot zero:

```
0x1264/0x126c = 0x00004000 / 0x00000c00
0x1260/0x1268 = 0x00004000 / 0x00000c00
```

For a vendor-layout-2 64-pixel A8R8G8B8 texture, the associated safe P690
sampler image is:

```
0x1200 = 0x00040101    /* select/latch unit 0; 2D pipe */
0x11e0 = 0x32020040    /* format 0x32, layout 2, width 64 */
0x11e8 = 0x00500000    /* vendor cache image */
```

The selection write must precede the shared sampler window.  This is a
necessary texture setup, **not sufficient evidence** that sampled alpha is
connected to the comparator.  In particular, `0x00040103` occurs in an older
PCI-X capture and wedges the P690; it is prohibited on the test target.

**Confidence: confirmed** for the archived packet words.  **Confidence:
unproven** that this exact sampler plus combiner image supplies texture alpha
on P690, because the direct P690 experiment disproved that conclusion.  The
former citation to `MTXOGL.dll:0x1010d532` as a GL_RGBA lowering branch was
incorrect; that address is a scalar conversion loop.  The corrected static
audit is in `parhelia-p690-native-alpha-static-recheck.md`.

## The nearby colour-key helper is not final hardware state

`MTXPARD.dll:0x17bbb0..0x17bc4a` looks tempting because it is adjacent to
texture-related state, but it is not a sampler register builder:

* it compares the argument with shadow `+0xd50` and `device+0x424` with
  `+0xd54`;
* when the argument is nonzero, it writes `+0x1578 = 1` if `+0x424 == 0`, or
  `3` otherwise; zero writes `0`;
* it marks only dirty/context fields: `+0xcf0 |= 0x40`,
  `+0x14f4 |= 0x00200800`, `+0xed4 |= 0x00200000`, and `+0xec0 |= 0x09`.

The three static calls (`0x17bfc3`, `0x17c016`, `0x17c072`) are part of the
same higher-level colour-key/texture control flow.  No path from this helper
to a final `0x1200..0x11e8` sampler image, `0x1260..0x126c` combiner image, or
the `0x12e4` packet is present in the archived static material.  Treating
`+0x1578` or its dirty masks as a programmable sampler-alpha bit would be an
unsupported write into a driver-private context.

**Confidence: confirmed** for these memory effects; **not a test candidate**.

## Safe next experiment

First obtain a matched vendor capture on a separate known-good P690 vendor
stack.  Do not load a binary vendor module or driver on the current target.
Use a one-unit `GL_MODULATE` draw with blending and depth disabled, fixed clear
colour, vertex alpha one, and four cases:

| case | upload | alpha state | expected pixel |
|---|---|---|---|
| RGB control | RGB or RGBA A=255 | disabled | painted |
| opaque RGBA | same RGB, A=255 | `GEQUAL,.5` | painted |
| cutout RGBA | same RGB, A=0 | `GEQUAL,.5` | clear colour retained |
| vertex control | A=255, vertex A=0 | `GEQUAL,.5` | clear colour retained |

Retain, for the final draw of every case, the full key-`0x0e` stage record,
the compiler-produced context object, key-`0x46` combiner object, sampler
window, and alpha packet.  Diff A=255 against A=0 before proposing a packet:

1. Confirm the candidate emits `0x12e4=0x0000800b` (or identify a proved
   vendor high-mask requirement).
2. Compare all sampler and combiner words from the *same final draw*.
3. Compare stage fields `ALPHAOP`, `ALPHAARG1`, and `ALPHAARG2`; the static
   hypothesis is `MODULATE(4), TEXTURE(2), DIFFUSE(0)`, but it has not been
   correlated with a cutout result.
4. Only if an observed complete delta exists, replay the entire delta in a
   private diagnostic.  Require all four pixel outcomes and a retired fence.

If the diagnostic needs a target module reload, preserve recoverability: copy
the candidate `.ko` only to `/tmp/parhelia-stage`, run `depmod -b
/tmp/parhelia-stage`, and use `modprobe -d /tmp/parhelia-stage`.  Never write
the experimental module under `/lib/modules`.  The target diagnostic must keep
the P690-safe `0x00040101`, use a timeout, and leave ordinary Mesa alpha-test
selection disabled until the four-case result passes.

No register sweep, `0x1200=0x00040103` test, `0x11e8` insertion, XRGB format
substitution, or shadow-field write is safe or informative here.
