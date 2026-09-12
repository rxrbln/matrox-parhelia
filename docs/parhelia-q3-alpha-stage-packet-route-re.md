# Q3 key-0x0e alpha-stage packet-route audit

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Scope

This is an offline audit of the retained working P690 ioquake3 state extract
and the matching saved Linux and Windows vendor binaries. It asks one narrow
question: whether the Q3 stage-zero input
`ALPHAOP=SELECTARG1`, `ALPHAARG1=TEXTURE` can be converted into a complete
P3D combiner or sampler packet from material already in the archive.

No target, module, vendor driver, Mesa source, or kernel source was changed.

## Result

There is **no replayable packet route** in the saved material. The Q3 record
proves that the vendor accepts texture alpha as a stage-compiler input. The
saved final textured P3D packets prove the raw alpha instruction encoding.
They do not correlate one to the other in a controlled alpha-test draw, and
the P690 direct test disproves treating the raw instruction as sufficient.

Consequently, there is no safe opt-in diagnostic packet to add. In
particular, changing Mesa's alpha instruction to a guessed `SELECTARG1`
encoding, injecting a key-`0x0e` record, or changing a sampler cache/pipe word
would not replay a vendor-observed route.

## Proven Q3 input record

The SHA-256 of `captures/ioq3-p690-working-draw.txt` is:

```
4ade3269b3b6d482dd38c32778ef26dd5759cbaf9e34a05faef85168b2864768
```

At state-submit-5 offset `0x378`, it contains 112 `(u16 unit, u16 field,
u32 value)` entries. The unit-zero prefix is directly present at offsets
`0x388..0x3b4`:

| field | value | stage interpretation |
|---:|---:|---|
| 1 | 4 | `COLOROP = MODULATE` |
| 2 | 2 | `COLORARG1 = TEXTURE` |
| 3 | 0 | `COLORARG2 = DIFFUSE` |
| 4 | 2 | `ALPHAOP = SELECTARG1` |
| 5 | 2 | `ALPHAARG1 = TEXTURE` |
| 6 | 0 | `ALPHAARG2 = DIFFUSE` |

This is a vendor context input, not a P3D register list. In the same state
object, the `0x46` key at `0x170` has a zero payload value. The retained
extract has `SUBMIT` changes for the vendor state mapping (`off=0x788000`),
but no matching final command-buffer words for `0x1200`, `0x11e0`,
`0x1260..0x126c`, or `0x12e4`. The key-`0x46` occurrence therefore cannot
be decoded as a final combiner object from this file.

## Established compiler boundary

The Linux vendor object SHA-256 is:

```
5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47
```

The disassembly gives an exact but bounded path:

```
key 0x0e field 4/5/6
  -> raw stage descriptor +0x3c/+0x44/+0x48       (mtx673)
  -> compiled record +0x10/+0x14/+0x18            (mtx2223)
  -> eight heap-backed sparse-map copies           (mtx950 -> mtx713)
```

At `mtx3136+0x23..+0x38`, `mtx580(device, 0)` supplies the compiled record
to `mtx950(context, record)`. `mtx950` calls `mtx713` eight times, with the
source increasing by `0x74` and the map container increasing by `0x60` each
time. `mtx713` selects a source dword through a dynamic node ID and copies it
to a heap-backed destination. Static code supplies neither that destination
address nor a subsequent mapping to a P3D register.

The immediately following `mtx3136+0x59` call is a separate map update:
`mtx273` receives four dwords from device offsets `+0xcc1`, `+0xcc5`,
`+0xcc9`, and `+0xccd`, rather than the compiled stage-record offsets. It
forwards those four dwords through generic `mtx1024` 16-byte map copies. This
rules out interpreting that adjacent call as a direct serialization of fields
4--6. It does not rule out a later dynamic consumer of the sparse map.

Reproduce the relevant static facts with:

```sh
objdump -drwC -Mintel --start-address=0x14be0 --stop-address=0x14c20 \
  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -drwC -Mintel --start-address=0x1b680 --stop-address=0x1b7a7 \
  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
```

## Packet evidence and its limit

The saved RGBA `GL_MODULATE` streams co-issue the final alpha fragment half:

```
0x1260 = 0x00004000;  0x1268 = 0x00000c00  # texture-address slot
0x1260 = 0x08000000;  0x1268 = 0x01400000  # saturated alpha MUL t0, v0
```

This is compatible with the Q3 stage expression when vertex alpha is one, but
it is not a `SELECTARG1` packet. More importantly, the P690 test target has
already shown that this program with A8R8G8B8 and a correct alpha comparator
still lets an A=0 texel pass, whereas vertex A=0 rejects. The missing state is
therefore upstream of the known raw alpha instruction or a hidden sampler/
context delivery condition.

The Windows ICD preserves the RGBA upload alpha byte, but its corrected
`GL_RGBA` upload branch is a transfer path, not a fragment compiler. No
Windows static trail connects the stage API fields to a final P3D packet.

## What would establish an implementation route

A separate known-good vendor P690 capture must collect the same final draw for
otherwise identical RGBA A=255 and A=0 inputs: the complete key-`0x0e` record,
the copied `mtx2223` record, `mtx713` dynamic destinations for IDs 4--6, the
final command section/key-`0x46` object, sampler words, alpha-test packet,
and a result pixel. Only an observed complete delta that rejects A=0, paints
A=255, rejects vertex-A=0, and retires a fence can define a private P690
diagnostic. The prescribed observer and collection procedure is in
`parhelia-vendor-capture-tooling.md`.

Do not derive a diagnostic from this Q3 state extract, retry P690 pipe
`0x00040103`, sweep sampler/cache bits, or enable sampled-alpha cutout in the
normal Mesa path.
