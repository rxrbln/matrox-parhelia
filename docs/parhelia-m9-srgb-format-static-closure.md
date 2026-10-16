# M9 sRGB and uncommon-format static closure

This note separates four questions which the saved vendor driver answers at
different confidence levels: public format translation, native descriptor
construction, API capability advertisement, and actual operation legality.
Conflating them previously produced one wrong RGB8 code and mistook the
public sampler enum value for the HAL/DDI state delivered to the driver.

All analysis here is offline.  The primary images are:

| image | SHA256 |
|---|---|
| Windows `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| Windows `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| Linux 1.15 `lib64/libGL.so.2.0.0` | `5aef9a8942b0380bf996e9879a10438a242f0b5743fa9d8678880ce8cebf5c97` |
| saved live `glxinfo.txt` | `f7d05b100adff3e11090566a53485dda147977df5f2d8adcabcf1e0f7e0e13f6` |

`tools/m9-srgb-format-evidence.py` verifies the hashes, RVAs, direct-call
reachability, format-name references, public mapping, descriptor initializer
results, native sRGB-write suffix, and clear conversion constants using only
the Python standard library.  `[BIN]` below means direct binary data flow and
`[DER]` means a deterministic decode performed by that tool.

## Result

The Direct3D render-target sRGB-write path is real and call-reachable.  It is a
compiler-inserted native instruction suffix for draw output zero, with a CPU
equivalent for color clears.  Sampler sRGB also has a live frontend transport:
the Direct3D 9 runtime maps public `D3DSAMP_SRGBTEXTURE` onto legacy HAL/DDI
state 29, which both pixel/fixed and vertex sampler dispatchers store at sampler
shadow `+0x46 bit 5`.  State blocks and normalized state queries preserve that
bit.  No consumer of bit 5 is joined in the pinned final sampler lowerers, so
this proves accepted state transport but not native texture decode.

A separate state-11 slot at `0x105c04` remains unreachable legacy table
residue.  Live state 11 is `D3DTSS_TEXCOORDINDEX`; its storage and compiler
consumer select texture-coordinate records and are unrelated to sRGB.

Neither saved GL implementation contains `GL_EXT_texture_sRGB` or
`GL_EXT_framebuffer_sRGB`.  The existing vendor `glxinfo` capture advertises
neither extension, and a live `glxinfo` taken from the running vendor stack on
the M9148 on 2026-09-17 confirms it (`parhelia-m9-openbits-gl-capture.md`):
no sRGB extension of any kind, so the vendor's own GL never exercises a
sampler decode path.  Direct3D render-target sRGB is therefore a dormant
implementation lead for Mesa, not an advertised GL feature.

The public Direct3D mapper accepts 46 named formats and explicitly rejects 13
other public formats.  Every one of the 59 names is directly referenced by the
live D3D capability-builder body.  A later audit recovered the complete
52-record baseline operation table at `0x20a008` and proved that the runtime
tree can only remove bits.  See `parhelia-m9-format-operation-matrix.md` and
`tools/m9-format-operation-evidence.py`.  The operation masks are exact API
capability declarations, but still do not replace result validation for a new
driver.

## Reachable sampler state, unresolved native decode

Direct3D 9 presents sampler states to legacy HAL drivers in the texture-stage
state namespace.  In that mapped sequence state 28 is `RESULTARG`, state 29 is
`SRGBTEXTURE`, state 30 is `ELEMENTINDEX`, state 31 is `DMAPOFFSET`, and state
32 is `CONSTANT`.  The DP2 texture-stage parser at `0xd094b..0xd0984` passes
its 16-bit stage and state IDs plus the 32-bit value to dispatcher `0x105340`.

For pixel/fixed stages below 28, the fallback path `0x1057a2 -> 0x105850`
routes mapped state 29 to handler `0x105b82`.  For vertex sampler stage IDs
257..272, `0x105340` subtracts 257 and calls dispatcher `0x105cf0`; its selector
and target tables route state 29 to mirror handler `0x105fa9`.  Both handlers
replace Boolean sampler shadow `+0x46 bit 5` and set dirty group `+0x04 |= 8`
[BIN][DER].  State-block builders at `0x12fe37` and `0x130b79` serialize the
same bit.  State normalization at `0x102a3c` publishes it at output `+0x74`.
Thus sampler sRGB is call-reachable, retained, and queryable.

The final filter helper `0x18afb0..0x18b4d7` consumes `+0x46` bits 11, 12, and
`4:0`; central lowerer `0x18ba27..0x18c22b` consumes bits 8, 10, 11, and 12.
Neither consumes bit 5.  Their exact byte ranges are hash-pinned by the
verifier.  The native sRGB-decode instruction or register path, if effective
on this hardware, is therefore still unjoined.  Do not expose texture sRGB
until a packet differential or a complete static data-flow join establishes
that consumer and result behavior.

The old state-11 interpretation provides a useful negative control.  Main
fixed-stage dispatch handles state 11 at `0x105648` as
`D3DTSS_TEXCOORDINDEX`, storing a three-bit coordinate-set selection per unit
in `ctx->texture_state +0x48:+0x4c`.  Helper `0x109850` replaces exactly:

```text
field = (field & ~(7ull << (3 * unit))) |
        ((uint64_t)(value & 7) << (3 * unit))
```

Compiler construction at `0x17f91d` consumes that field on its pre-SM3 route.
The fallback table's state-11 entry at `0x105c04` cannot be reached because the
main dispatcher intercepts state 11 before fallback.  It is neither the public
sampler enum route nor evidence of native sRGB decoding.

## Reachable render-target sRGB write

`D3DRS_SRGBWRITEENABLE` is render state 194.  Handler `0x1037e7` replaces
frontend shadow `+0xf4 bit 11` and marks dirty group `0x10000` [BIN].  During
native fragment-program construction, `0x137242` tests the copied bit at
compiler state `+0x174`.  The test is conditional on output/attachment index
zero.  When RT0 is bound, `0x1373e7` calls `0x174ea0` to insert the conversion
suffix.

The suffix builder emits a complete 25-slot sequence.  Fifteen two-dword slots
are direct literals in `0x174ea0`; between direct slots 9 and 10 it calls
`0x186320` with arguments `(4, 1, 3, 0)` to pack five slots and calls
`0x1864e0` to append five more fixed slots [BIN][DER]:

```text
e411801c b048804f    e491821c b048811f    e411841c b048837f
043f801c ac490140    44bf821c ac490110    843f841c ac490170
043f801c ac494140    44bf821c ac494110    843f841c ac494170
423f821d ac3fc300    82bf801c ac3fc140    e43f909c a029804f
e4bf929d a029800f    e400101c a3098f5f    e481121d a3098f0f
```

The argument-packed middle slots derived from the exact helper inputs are:

```text
5fff861d ae3ff000    41bf88bc 8c26ac10    c204061d a2608f00
41bf88bc 8c26ac10    41bf88bd a2608f00
```

The five fixed helper slots are:

```text
1fff8a1c ae3ff100    403f8c1c ac3ffd41    43050a1c a6608741
403f8c1c ac3ffd41    43050a1c a6608741
```

These words are an exact implementation artifact, but the current native-ISA
decode does not name every opcode and constant source.  They prove a
compiler-inserted conversion sequence, not a fixed output-register enable.
The ordered sequence is the first nine direct literals, five argument-packed
slots, five fixed helper slots, then the remaining six direct literals.  It
should first be replayed as the complete vendor suffix; replacing the
sequence with a guessed one-bit register setting is unsupported.

Color clear `0x13e0f0` independently tests surface-set `+0xf4 bit 0x800` only
for attachment zero and converts the clear RGB channels before normal
format-specific packing.  Alpha is retained.  Its CPU transfer is exactly:

```text
if x <= 0.0030400000978261232:
        y = 12.920000076293945 * x
else:   y = 1.0549999475479126 *
            pow(x, 0.4166666567325592) - 0.054999999701976776
```

The comparison orientation follows the two branches in `0x13e169..0x13e23d`.
These are the vendor constants, including its `0.00304` threshold.  Do not
silently substitute a different threshold in a bit-exact clear test.

## Exact public-to-native format map

Mapper `0x4ee30` is called at `0x4f9f8`, `0x9a5e7`, and `0x9a602`.  It
translates ordinary public enum values through byte selector
table `0x4f060` and target table `0x4efc4`.  It handles the supported FourCCs
through explicit comparisons at `0x4ef3f..0x4efc2`.  Internal IDs select one
of 86 descriptor initializers through table `0x50b48`.  Descriptor field `+8`
becomes the seven-bit hardware code in sampler index `78[31:25]` and the same
format field in render-target selector-8 descriptors [BIN][DER].

The class column is the vendor's internal descriptor class.  The API name
defines normalized, signed, floating, depth/stencil, or compressed semantics;
the class/code pair proves that those names are represented distinctly, but
does not by itself prove sampling conversion or renderability.

### Packed and normalized color

| public `D3DFMT` | internal | class | hardware code | stored bits |
|---|---:|---:|---:|---:|
| `R8G8B8` | `0d` | 2 | `66` | 32 |
| `A8R8G8B8` | `0c` | 2 | `66` | 32 |
| `X8R8G8B8` | `0d` | 2 | `66` | 32 |
| `R5G6B5` | `0a` | 2 | `4e` | 16 |
| `X1R5G5B5` | `09` | 2 | `4a` | 16 |
| `A1R5G5B5` | `08` | 2 | `4a` | 16 |
| `A4R4G4B4` | `06` | 2 | `46` | 16 |
| `X4R4G4B4` | `07` | 2 | `46` | 16 |
| `A2B10G10R10` | `10` | 2 | `68` | 32 |
| `A8B8G8R8` | `0e` | 2 | `64` | 32 |
| `X8B8G8R8` | `0f` | 2 | `64` | 32 |
| `G16R16` | `0b` | 2 | `78` | 32 |
| `A2R10G10B10` | `11` | 2 | `6a` | 32 |
| `A16B16G16R16` | `12` | 2 | `38` | 64 |

`R8G8B8` uses 32 stored bits and aliases the `X8R8G8B8` internal descriptor.
Its hardware code is `0x66`, not `0x64`.  The old `0x64` claim confused it
with the BGR-ordered `A8B8G8R8`/`X8B8G8R8` descriptor.

### Alpha, luminance, signed/bump, and YUV

| public `D3DFMT` | internal | class | hardware code | stored bits |
|---|---:|---:|---:|---:|
| `A8` | `36` | 5 | `33` | 8 |
| `L8` | `33` | 5 | `30` | 8 |
| `A8L8` | `37` | 5 | `51` | 16 |
| `A4L4` | `34` | 5 | `31` | 8 |
| `L16` | `35` | 5 | `50` | 16 |
| `V8U8` | `3a` | 6 | `59` | 16 |
| `L6V5U5` | `3b` | 7 | `55` | 16 |
| `X8L8V8U8` | `3c` | 7 | `71` | 32 |
| `Q8W8V8U8` | `3d` | 6 | `65` | 32 |
| `V16U16` | `3e` | 6 | `79` | 32 |
| `A2W10V10U10` | `3f` | 6 | `6d` | 32 |
| `Q16W16V16U16` | `40` | 6 | `39` | 64 |
| `UYVY` | `43` | 8 | `58` | 16 |
| `YUY2` | `44` | 8 | `58` | 16 |

UYVY and YUY2 retain different internal IDs, flags, and FourCC metadata but
share hardware code `0x58`.  Static descriptor construction therefore does
not yet locate the chroma/order selector or establish their filtering rules.

### Half and full floating point

| public `D3DFMT` | internal | class | hardware code | stored bits |
|---|---:|---:|---:|---:|
| `R16F` | `17` | 3 | `54` | 16 |
| `G16R16F` | `19` | 3 | `74` | 32 |
| `A16B16G16R16F` | `15` | 3 | `3c` | 64 |
| `R32F` | `18` | 3 | `75` | 32 |
| `G32R32F` | `1a` | 3 | `3b` | 64 |
| `A32B32G32R32F` | `16` | 3 | `34` | 128 |

Both vendor GL binaries contain generic texture-float token strings, but the
saved live M9 extension string does not advertise `GL_ARB_texture_float` or
`GL_ATI_texture_float`.  Those strings are not capability evidence.  The D3D
mapper and native descriptors are executable paths; filtering, blending,
renderability, NaN/Inf behavior, and readback conversion remain unproven.

### Depth and stencil

| public `D3DFMT` | internal | class | hardware code | stored bits | masks |
|---|---:|---:|---:|---:|---|
| `D16_LOCKABLE`, `D16` | `25` | 4 | `5c` | 16 | depth `ffffffff` |
| `D32` | `30` | 4 | `7c` | 32 | depth `ffffffff` |
| `D15S1` | `26` | 4 | `5d` | 16 | depth `fffefffe`, stencil `00010001` |
| `D24S8` | `27` | 4 | `7d` | 32 | depth `ffffff00`, stencil `000000ff` |
| `D24X8` | `28` | 4 | `7d` | 32 | depth `ffffff00` |
| `D24X4S4` | `2c` | 4 | `7d` | 32 | depth `ffffff00`, stencil `0000000f` |

The masks are the exact vendor descriptor fields.  Their repeated-bit form for
16-bit depth reflects the vendor's conversion representation and should not be
reinterpreted as a 32-bit storage size.  Depth descriptor presence proves the
depth-attachment path; it does not make every depth format texture-sampleable.

### Block-compressed formats

| public FourCC | internal | class | hardware code | bits/texel |
|---|---:|---:|---:|---:|
| `DXT1` | `00` | 1 | `10` | 4 |
| `DXT2` | `02` | 1 | `15` | 8 |
| `DXT3` | `03` | 1 | `15` | 8 |
| `DXT4` | `04` | 1 | `17` | 8 |
| `DXT5` | `05` | 1 | `17` | 8 |

DXT1 maps to internal ID zero, not one.  Internal ID one is the vendor's
private `DXTN` descriptor and has no public DXT1 mapping.  DXT2/3 and DXT4/5
share hardware codes while retaining distinct internal FourCC metadata.  The
descriptor alone does not prove where premultiplied-alpha semantics are
handled.

### Explicit public rejections

The same mapper returns `-1` for:

```text
R3G3B2             A8R3G3B2          A8P8              P8
D32F_LOCKABLE      D24FS8            VERTEXDATA        INDEX16
INDEX32            CxV8U8            R8G8_B8G8         G8R8_G8B8
MULTI2_ARGB8
```

Some rejected names still occur in the live format-capability builder.  That
is a useful negative control: a referenced name or table row is not sufficient
to claim that resource creation reaches a native descriptor.

## Operation-legality matrix

| operation | static result | safe driver conclusion |
|---|---|---|
| ordinary sampling | public mapper, descriptor code, allocation layout, and sampler bind are joined | expose only formats already validated by result captures; code presence does not settle signed/float/depth/YUV filtering or conversion |
| sRGB sampling/decode | exact baseline masks advertise `SRGBREAD`; mapped D3D9 state 29 is live for pixel and vertex samplers, stores sampler `+0x46` bit 5, and survives state blocks/query normalization, but neither pinned final native lowerer consumes bit 5; the unreachable state-11 fallback is only a TEXCOORDINDEX negative control | treat this as accepted frontend transport with an unresolved execution path; keep Gallium sRGB sampler formats disabled until a later derived-record consumer or explicit shader lowering is joined and result-validated |
| ordinary render target | selector-8 descriptors carry the same format code and the exact 52-row D3D baseline names RT-capable formats; runtime policy can only remove bits | use the operation matrix as the vendor capability upper bound, then require result validation before new-driver exposure |
| sRGB draw write/encode | D3DRS 194 reaches an exact compiler suffix only for output zero | replay and validate the complete suffix before advertising framebuffer sRGB; test blending separately |
| sRGB clear | RT0 clear performs the exact CPU conversion above before format packing | useful deterministic oracle for RGB8/10-bit clear captures; alpha must remain unchanged |
| same-format copy/blit | no sRGB shadow consumer is joined in copy paths | treat copies as encoded-byte preserving until a conversion capture proves otherwise |
| cross-format copy | format descriptors feed CPU/transfer helpers, but no complete pairwise legality table is static | keep conversions on tested paths only |
| mip sampling | ordinary LOD/tail layout is joined and is independent of the dead sRGB slot | mip selection does not imply sRGB decode |
| automatic mipmap generation | the exact baseline sets `D3DFORMAT_OP_AUTOGENMIPMAP` for common packed color, luminance, GigaColor, and float rows, but not DXT, signed/bump, YUV, or depth | preserve that upper bound and validate generation precision/layout before exposure |

The Windows D3D capability builder at `0x784e0..0x79b7f` directly references
all 59 public format names as restriction tokens.  Its destination records come
from the fixed 52-entry, 32-byte template at `0x20a008`.  The exact masks and
post-construction restrictions are documented in
`parhelia-m9-format-operation-matrix.md`.  Static analysis now recovers the
vendor capability upper bound; only the board/config-specific removal of bits
requires a live query.

## Remaining capture gates

The static map is complete enough to construct descriptors, but a fully
advertised Mesa format table still needs narrowly scoped captures:

1. Query the vendor D3D format-operation records once to preserve the target's
   board/config-specific restrictions and compare them with the exact baseline.
2. Capture RT0 draw output with D3DRS 194 off/on and save the complete native
   program.  Match all 25 slots, including the ten-slot helper-generated middle,
   then validate byte results around the transfer-function breakpoint.
3. Run sample/filter matrices for half/full float, signed/bump, depth, YUV,
   and DXT formats.  Include nearest/linear/mip filtering and odd dimensions.
4. Run render/blend/readback matrices separately.  A successful sample must
   not be promoted to render-target or blend support.
5. Verify same-format copy is bit preserving and test each requested
   cross-format conversion explicitly.  No sRGB conversion should be assumed
   on a copy path.
6. Test autogen mipmaps per format and determine whether any implementation
   computes in encoded or linear space.  The current binary supplies no safe
   sRGB-mipmap conclusion.

Until those gates close, the exact native codes are implementation inputs and
diagnostic oracles, not a reason to broaden Gallium caps.
