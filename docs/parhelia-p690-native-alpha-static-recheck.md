# P690 native sampled-alpha static re-check

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is an offline re-check of the archived Windows ICD and the saved working
P690 ioquake3 state.  It corrects one misleading binary-location citation in
the older alpha notes.  It does not change Mesa, the kernel, a target, or a
vendor driver.

## Result

The archive still contains no replayable P690-native sampled-alpha route.  It
does, however, narrow the next comparison in two useful ways:

1. `MTXOGL.dll:0x1010d532` is **not** a `GL_RGBA` branch or fragment-combiner
   generator.  It is inside a scalar-to-16-bit pixel conversion loop.  Any
   conclusion that derives the sampled-alpha route from that address is
   invalid.
2. The saved working P690 Q3 stage image uses
   `ALPHAOP=SELECTARG1(2), ALPHAARG1=TEXTURE(2)`, rather than the previously
   hypothesized `ALPHAOP=MODULATE(4)`.  For a draw with vertex alpha one,
   select-texture is exactly the relevant alpha expression.  A future
   opaque/A=0 vendor comparison must measure field 4; it must not assume that
   its required value is 4.

The measured final combiner packets, the A8R8G8B8 upload/storage evidence,
and the P690 negative hardware result remain valid.  What has been removed is
only an unsupported causal link between a specific ICD instruction address and
the final alpha combiner.

## Reproducible binary correction

The archived input is:

```
SHA-256 41b2317309801a917506c29c2b5998713ddb9bdfa3d26c623ab56617d0c1d848
xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll
```

The earlier notes described RVA `0x10d532` (loaded VMA `0x1010d532`) as a
`cmp ..., 0x1908` branch.  The actual bytes and decoded instructions are:

```
1010d52c  83 c6 02              add esi, 2
1010d52f  0f b7 44 24 14        movzx eax, word ptr [esp+0x14]
1010d534  d8 ca                 fmul st, st(2)
1010d536  0d 00 0c 00 00        or eax, 0xc00
1010d53b  89 44 24 0c           mov [esp+0xc], eax
1010d53f  83 c2 04              add edx, 4
1010d542  d8 c1                 fadd st, st(1)
1010d544  d9 6c 24 0c           fldcw [esp+0xc]
1010d548  db 5c 24 0c           fistp dword ptr [esp+0xc]
1010d54c  66 8b 44 24 0c        mov ax, word ptr [esp+0xc]
1010d551  66 89 46 fe           mov word ptr [esi-2], ax
```

There is no `0x1908` immediate, no texture-stage write, and no fragment-state
packet in that basic block.  It belongs to a family of adjacent conversion
loops which quantize float input to 8- or 16-bit destinations.

Use this command to reproduce the result:

```sh
objdump -D -b pei-i386 -m i386 --start-address=0x1010d500 \
  --stop-address=0x1010d570 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXOGL.dll
```

## What the ICD does prove about RGBA input

There is a genuine `GL_RGBA` upload conversion branch at
`MTXOGL.dll:0x103ab28f`, but it is not a fragment-state compiler:

```
103ab28f  cmp ebx, 0x1908              ; GL_RGBA
103ab295  jne 0x103ab4bb
103ab29b  cmp eax, 0x8035              ; GL_UNSIGNED_INT_8_8_8_8
103ab2a0  jne 0x103ab4bb
...
103ab31f  mov edi, 3
103ab324  jne 0x103ab32b
103ab326  mov edi, 4                   ; RGBA has four source bytes
...
103ab3a2  mov eax, [esi]
103ab3a4  ror eax, 8
103ab3a7  mov [edi], eax               ; four-byte path preserves A
```

The other path at `0x103ab383` explicitly ORs `0xff000000` into three-byte
input.  This corroborates the existing conclusion that the vendor preserves
the alpha byte for the RGBA upload, while RGB input is made opaque.  The
routine subsequently invokes an opaque virtual setter and then a texture
transfer helper; the static data here does not identify a sampler-alpha mux,
a `0x1260..0x126c` word, or an alpha-comparator side condition.

A separate dispatcher at `0x101f0c70` selects conversion routines for internal
formats `0x8032..0x8368`.  Its `GL_RGBA` tests at `0x101f0ca2` and following
are reached from `0x10078d89`, which first stores external format/type fields
at object offsets `+0x24/+0x28`.  That is additional evidence for an upload or
pixel-transfer path, not evidence for a draw-time fragment route.

## Working P690 Q3 stage state

The saved trace has SHA-256
`4ade3269b3b6d482dd38c32778ef26dd5759cbaf9e34a05faef85168b2864768`.
At `captures/ioq3-p690-working-draw.txt` offsets `0x378..0x3b4`, its key
`0x0e` stage-zero entries are:

| field | value | conventional D3D stage name |
|---:|---:|---|
| 1 | 4 | `COLOROP = MODULATE` |
| 2 | 2 | `COLORARG1 = TEXTURE` |
| 3 | 0 | `COLORARG2 = DIFFUSE` |
| 4 | 2 | `ALPHAOP = SELECTARG1` |
| 5 | 2 | `ALPHAARG1 = TEXTURE` |
| 6 | 0 | `ALPHAARG2 = DIFFUSE` |

This is direct trace data, not a reconstructed packet.  It means that the
known working vendor path gives alpha the texture value directly, while RGB
modulates texture and diffuse.  It is compatible with a cutout draw whose
vertex alpha is one, but the retained trace does not preserve a matched
alpha-test state, source texture alpha, vertex alpha, or readback.  It
therefore cannot justify replaying `SELECTARG1` on the open driver by itself.

## Consequence for implementation

The correct next evidence target is unchanged in shape but more precise:
capture opaque and A=0 draws on a separate vendor P690 stack and retain the
full stage record, post-`mtx2223` record, dynamic `mtx950` map destinations,
key `0x46`, sampler state, alpha-test state, and a result pixel.  The analysis
must treat both `SELECTARG1` and `MODULATE` as possible observed field-4
values.  Only the complete observed delta may be considered for a private
P690 diagnostic.

Do not use the invalid `0x1010d532` citation, infer a fragment-state register
from the upload converter, sweep context bits, retry paired pipe
`0x00040103`, or enable sampled-alpha testing in normal Mesa selection.
