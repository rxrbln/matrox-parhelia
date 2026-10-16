# Matrox M9 blend and raster lowering

This note records the statically proven Direct3D render-state path in the
saved 32-bit Windows M-Series driver `MSerX/MxMD.dll`.  Addresses are image
virtual addresses.  It deliberately stops before assigning names to unknown
M9 indexed registers.  `[BIN]` is a direct instruction-level observation and
`[DER]` is a conclusion from more than one such observation.

## Frontend shadows and flat cache

The render-state dispatcher starts at `0x103370`.  For ordinary render-state
IDs it sets `edi=1` at `0x1033d5` before jumping through the handler table.
The following frontend fields therefore have exact definitions [BIN]:

| state | frontend field | frontend dirty |
|---|---|---:|
| `ALPHATESTENABLE` | `p2514+0xf4 bit 4` | `p2514+4 |= 0x2000` |
| `ALPHAREF` | low byte of `p2514+0xec` | `0x2000` |
| `ALPHAFUNC` | `p2514+0xf4[10:7]` | `0x2000` |
| `ALPHABLENDENABLE` | low byte of `p2514+0xf8` | `0x4000` |
| `SRCBLEND` | `p2514+0xf8[24:20]` | `0x4000` |
| `DESTBLEND` | `p2514+0xfc[4:0]` | `0x4000` |
| `BLENDOP` | `p2514+0xf8[11:9]` | `0x4000` |
| `SEPARATEALPHABLENDENABLE` | `p2514+0xf8 bit 8` | `0x4000` |
| `SRCBLENDALPHA` | `p2514+0xf8[19:15]` | `0x4000` |
| `DESTBLENDALPHA` | `p2514+0xf8[29:25]` | `0x4000` |
| `BLENDOPALPHA` | `p2514+0xf8[14:12]` | `0x4000` |
| `BLENDFACTOR` | D3DCOLOR at `p2514+0xd0` | `0x8000` |
| `SRGBWRITEENABLE` | `p2514+0xf4 bit 11` | `0x10000` |
| `MULTISAMPLEANTIALIAS` | `p2514+0xf4 bit 0` | `0x1000` |
| `MULTISAMPLEMASK` | `p2514+0xe8` | `0x1000` |
| `COLORWRITEENABLE0..3` | `p2514+0x100,+0x104,+0x108,+0x10c` | target bits `0..3` |
| private output masks `4..7` | `p2514+0x110..+0x11c` | target bits `4..7` |
| `CULLMODE` | `p2528+4[1:0]` | `p2528+0 |= 4` |

Initializer/canonicalizer `0x101550` copies these fields to a flat D3D cache,
but emits no command [BIN]:

| semantic | flat-cache offset |
|---|---:|
| alpha-test enable, reference, function | `+0x40,+0x64,+0x68` |
| blend enable, source, destination | `+0x70,+0x50,+0x54` |
| blend operation | `+0x2b0` |
| separate-alpha enable, source, destination, operation | `+0x33c,+0x340,+0x344,+0x348` |
| blend factor, sRGB write | `+0x308,+0x30c` |
| multisample enable | `+0x288` |
| color masks `0..7` | `+0x2a4,+0x2fc,+0x300,+0x304,+0x360,+0x364,+0x368,+0x36c` |
| fill, shade, cull | `+0x24,+0x28,+0x5c` |

This cache is an API-facing mirror.  It must not be confused with an indexed
register shadow.

## Fill and shade fan-out and compiler key

`FILLMODE` handler `0x10405d` masks the D3D value to two bits and updates all
three of these representations [BIN]:

```text
p2528+0x48[5:4] = value & 3;  p2528+0 |= 0x00000002
p2514+0x0c[16:15] = value & 3; p2514+4 |= 0x08000000
p252c+0x0c[1:0] = value & 3;   p252c+0 |= 0x00040000
```

`SHADEMODE` handler `0x1040c9` does the corresponding fan-out [BIN]:

```text
p2528+0x48[7:6] = value & 3; p2528+0 |= 0x00000008
p252c+0x0c[3:2] = value & 3; p252c+0 |= 0x00080000
p2520+0x18 bit 0 = (value == 1); on change p2520+0 |= 0x00000080
```

The last comparison is exact because the common dispatcher establishes
`edi=1`.  This resolves the earlier uncertainty about whether that bit meant
flat shading: Direct3D `D3DSHADE_FLAT` is enum value 1 [DER].

The compiler builder at `0x13a080` publishes its outer object's `+0x18` as
core `p2528` and `+0x64` as the per-target program-state manager.  Therefore
builder `+0x60` is exactly live raster `p2528+0x48`.  Initial builder routine
`0x13a0e0` packs that word into the separate compiler key at builder `+0x78`:

| frontend field | compiler-key field | exact enum contribution |
|---|---|---|
| `FILLMODE`, `p2528+0x48[5:4]` | `+0x78[1:0]` | point `1`, wireframe `2`, solid `3` |
| `p2528+0x48[14:13]` | `+0x78[3:2]` | unchanged two-bit value |
| `SHADEMODE`, `p2528+0x48[7:6]` | `+0x78[5:4]` | flat `0x10`, Gouraud `0x20`, Phong `0x30` |

The two shifts at `0x13a1c2` and `0x13a1cf` are both required for this map.
The persistent `p252c+0x0c` nibbles remain dependency inputs; the exact
builder-key packing does not by itself identify a final M9 index [BIN].

Point fill does have one exact final-command consequence.  Render-entry
constructor `0x17edb0` receives the live `p2528` pointer in entry field
`+0x14` and the per-batch common command shadow in `+0x24`.  Its helper
`0x17f040` tests `p2528+0x48[5:4] == 1`, the `D3DFILL_POINT` encoding.  When
entry `+0x2b0` is zero and the primitive code in `p2528+0x48[3:0]` is outside
`{0,1,2,3,4,0xb,0xe}`, it ORs `0x00000fff` into common-shadow word `+0x1c`;
if any of those conditions fails, it clears that word's low 16 bits.  Raster
`+0x48 bit 8` and up to four per-item flags additionally control bits
`15:12`, whose exact role remains unnamed [BIN].  The shadow marks common
groups `0x01|0x02` dirty.  Emitter `0xf4f50` maps group `0x01` fields
`+0x10,+0x14,+0x18,+0x1c` to final indices `88,89,8a,c1`, so the point-fill
mask reaches final index `c1[11:0]` [BIN].  This is a point-mode effect, not
evidence that every fill mode is encoded directly in index `c1`.

## Alpha-test fragment lowering

The fixed-state validator is built at `0x136940`.  Its frontend-state view is
at validator `+0x80`; consequently `p2514+X` is validator `+0x80+X`.  During
per-target fragment construction, `0x1374d3` calls `0x170800` with the
frontend-state view when alpha testing can be active [BIN].

`0x170800` performs the following exact lowering [BIN]:

1. If `p2514+0xf4 bit 4` is clear, it emits no alpha-test fragment.
2. It converts the integer at `p2514+0xec` to a float and stores it in the
   fragment object's `+0x24` constant slot.
3. For `ALPHAFUNC=NEVER` it replaces that constant with the literal float
   `-1.0` from `0x1f5198`.
4. `ALPHAFUNC=ALWAYS` emits no comparison.  Other functions append the native
   word pair `e4bf801d a04443cf`, an instruction generated by `0x1859e0`, and
   a comparison generated by `0x186090`.

The comparison selector helper at `0x170970` maps Direct3D functions exactly:

| D3D alpha function | `NEVER` | `LESS` | `EQUAL` | `LESSEQUAL` | `GREATER` | `NOTEQUAL` | `GREATEREQUAL` | `ALWAYS` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| compiler selector | `1` | `1` | `5` | `3` | `2` | `4` | `0` | no fragment |

The shared selector for `NEVER` and `LESS` is intentional: the `NEVER`
constant substitution makes their generated comparison path differ.
`0x137577`/`0x1709d0` appends the resulting fragment object at validator
`+0x8e4` to the per-target program under construction [BIN].

## Blend lowering is native-program generation

Blend state does not select a canned program at `0x13b570`.  That routine is
the fixed-function vertex/declaration companion selector documented in
`parhelia-m9-native-shader-fragments.md`.  The actual blend compiler is
`0x170ff0`, called once per attached render target at `0x1374a4` and appended
to the program at `0x1375bc..0x1375c5` [BIN].

The target is enabled when `(p2514+0xf8 low byte) & (1 << target)` is nonzero
(`0x1710c7..0x1710ed`).  Helper `0x1722e0` recognizes the replace-equivalent
case and suppresses unnecessary blend code [BIN]:

```text
SRCBLEND       = 2
DESTBLEND      = 1
BLENDOP        = 1 or 2
and, if separate alpha is enabled:
SRCBLENDALPHA  = 2
DESTBLENDALPHA = 1
BLENDOPALPHA   = 1 or 2
```

These are raw Direct3D enum values.  The two complete compiler dispatch maps
are [BIN]:

| D3D factor value | source path | destination path |
|---:|---:|---:|
| 1 | initial/default path | initial/default path |
| 2 | `0x1711fe` | `0x171693` |
| 3 | `0x17120e` | `0x17169e` |
| 4 | `0x171232` | `0x1716bd` |
| 5 | `0x171257` | `0x1716e2` |
| 6 | `0x17127e` | `0x171703` |
| 7 | `0x1712a0` | `0x17172a` |
| 8 | `0x1712d3` | `0x17176d` |
| 9 | `0x171334` | `0x1717d4` |
| 10 | `0x171361` | `0x1717f3` |
| 11 | `0x171394` | `0x171818` |
| 12 | `0x1713d5` | `0x171923` |
| 13 | `0x17141a` | `0x171923` |
| 14 | `0x1712f3` | `0x17179d` |
| 15 | `0x171310` | `0x1717b5` |
| 16 | `0x1714b6` | `0x171892` |
| 17 | `0x1714e4` | `0x1718b5` |
| 18 | `0x17150e` | `0x1718db` |
| 19 | `0x17153a` | `0x1718fc` |
| 20 | `0x17145f` | `0x171858` |
| 21 | `0x171489` | `0x171871` |

The source table is at `0x17216c`; the destination table is at `0x1721bc`.
Each path fills compiler operand tuples and the shared tail generates native
two-dword instructions.  Blend operations are consumed directly from
`p2514+0xf8[11:9]` and, when separate alpha is set, `[14:12]`; values 4 and 5
have dedicated min/max paths at `0x171d2a..0x171da3` and
`0x171ef4..0x171f59` [BIN].

Constant blend color is also fully joined to compiler constants.  `0x173cd0`
reads the D3DCOLOR at `p2514+0xd0`, converts its bytes by `1/255`, and writes
the following float vector twice [BIN]:

```text
fragment +0x24..+0x30 = { A, R, G, B } / 255
fragment +0x34..+0x40 = { A, R, G, B } / 255
```

## Color-write masks

Per-target routine `0x1389a0`, called at `0x137488`, reads
`p2514+0x100+4*target`.  For its direct output path it builds these exact
16-bit masks and records the original enable bits at validator
`+0x218+4*target` [BIN]:

| D3DCOLORWRITEENABLE bit | generated repeated-nibble mask |
|---:|---:|
| red, bit 0 | `0x4444` |
| green, bit 1 | `0x2222` |
| blue, bit 2 | `0x1111` |
| alpha, bit 3 | `0x8888` |

The OR of enabled channel masks is stored as a 16-bit value at validator
`+0x358+2*target`.  The direct mask path is taken only when the surface format
callback returns internal type `0x0c` or `0x0d` and both
`p2514+0xf4 bits 2/3` are clear.  Every other case takes the compiler path
through `0x173ee0`; it generates channel-routing native instructions instead
of relying only on the 16-bit mask [BIN].  If validator
`+0x1b0` is zero, `+0x1ac >= 6`, and `MULTISAMPLEMASK` is zero, the routine
clears the 16-bit target mask at `0x138be2..0x138bf2` [BIN].

## Compiler-result cache and native upload boundary

The fragment pieces for output conversion, alpha test, blending, and the
application pixel shader are appended into one per-target compiler program by
`0x1371e0` [BIN].  Finalizer `0x1397e0` then:

* seeds constants including the alpha reference at validator `+0x908`;
* finalizes the compiler object into a 256-byte candidate record at validator
  `+0x248`;
* byte-compares that record with the previous record at validator `+0x688`;
* on change, increments a serial through validator `+4`, stores it at
  validator `+0x244`, copies all 64 dwords to `+0x688`, and marks nested
  compiled-state dirty bit `0x00100000` at validator `+0x1b8`.

Backend builder `0x13eea4` reads only the first dword of validator `+0x248`.
It adds that dword to the backend program object's `+0xa0` base and also saves
the dword at program-object `+0xa4`; adjacent validator dwords are consumed as
separate fields later in the builder [BIN].  This disproves the former claim
that all 256 bytes are a native fragment-program image.  Their exact internal
layout remains opaque, but the block is conclusively a compiler-result cache
record whose identity changes when the specialized fragment compilation
changes [BIN][DER].

The actual native-program upload is independently joined.  Linker `0x13bf80`
builds the native instruction buffer represented by program-state byte length
`+0xbb0` and CPU pointer `+0xbb4`.  Emitter `0xf74d0` streams that buffer in
32-byte lines through selector index `68` and data indices `60..67` [BIN].
The 64 sparse records handled by `0xf6f20` are a different state array at
`+0xc0`, stride `0x1c`: each has a key, four payload dwords, current dirty at
record `+0x14`, and pending dirty at `+0x18`.  State promotion at `0x13dac0`
copies pending to current for all 64 records; `0xf6f20` emits current records,
using even keys for `60..63`, odd keys for `64..67`, and the same selector
index `68`, then clears current dirty [BIN].  No data flow associates the
validator's 256-byte cache record with these sparse uploads.

## Cull, fill, shade, and multisample final paths

Multisample enable has an exact compiler-to-command join.  Validator
`0x139240` reads `MULTISAMPLEANTIALIAS` from validator `+0x174`, corresponding
to `p2514+0xf4 bit 0`.  Together with attachment and mode conditions, it
rebuilds nested compiled-state words `+0x1b0` and `+0x1b4` and marks dirty bits
`0x000a0000` [BIN].  Lowerer `0x181fa0` copies these words to surface shadow
`+0x28,+0x2c` at `0x1826a5..0x1826b4` and marks surface dirty `0xc0`.
Emitter `0xfb920` then replicates `+0x28` to four writes of final index `ad`
and writes `+0x2c` to final index `af` [BIN].  Enable therefore controls a
compiled `ad/af` state pair; it is not a single independently copied command
bit.

`MULTISAMPLEMASK` has a narrower proved effect.  Its validator field `+0x168`
is read at `0x138be2`: a zero value can suppress the per-target 16-bit color
output mask described above.  No nonzero mask bits are copied into the
compiled `ad/af` words or into another fixed command word by this saved path
[BIN].  Partial sample-mask behavior consequently remains unjoined.

Cull, wireframe/solid, and shade mode now have exact compiler-key encodings,
but still stop before a proved fixed index [BIN][DER]:

* `CULLMODE` starts at `p2528+4[1:0]`.  Since builder `+0x18` is `p2528`,
  incremental lowerer `0x13ab40` reads the same field at builder `+0x1c` and
  copies it to compiler key `+0x78[7:6]`: none `0x40`, clockwise `0x80`, and
  counterclockwise `0xc0`.  It also makes `CULL_CW` set per-target program
  record `+0x80 bit24`; a gated second path makes culling enabled
  (`value != CULL_NONE`) affect record `+0x80 bit25`.  The lowerer marks record
  dirty group `0x2`.  The final program/vector emitter `0xf6f20` has no group
  `0x2` case, so neither these record fields nor key bits `7:6` can yet be
  named as an indexed hardware word.  The separate `0x17e8d0` decoder accepts
  `CULL_NONE` as a draw-path eligibility condition [BIN].
* Clone `0x1119b0` covers the raster object only through `+0x44` and does not
  copy the fill/shade mirror at `p2528+0x48`.  That does not remove the live
  raster object from render-entry construction: `0x17f040` consumes it and
  produces the point-fill `c1[11:0]` effect described above.  Routine
  `0x133c80` separately tests fill value 3 while computing renderer-internal
  eligibility boolean `+0x1d18`; no corresponding live-raster test for value
  2 (`D3DFILL_WIREFRAME`) reaches an indexed command in the saved path.
* Persistent fill/shade nibbles in `p252c+0x0c` participate in renderer
  dependency checks at `0x132e10`, which invalidate and rebuild compiled draw
  entries.  Flat shade also has the independent `p2520+0x18 bit 0` fan-out
  documented above.  Builder key bits `1:0` and `5:4` now retain the complete
  fill and shade enums.  Static data flow has not isolated the resulting
  native instruction or indexed word for either field.

There is one easy false join to avoid.  Emitter `0xf6f20` writes its bound
per-target program record `+0x78` to final index `30`.  That object is selected
from builder `+0x74` with a `0xc04` target stride.  It is not builder `+0x78`,
the compiler key above.  No copy between those two `+0x78` fields occurs in
the builder range `0x13a020..0x13c610` [BIN].

Controlled command captures are still needed to locate the final effect of
the now-known `CULL_CW`/`CULL_CCW`, wireframe/solid, and flat/Gouraud compiler
keys, and to determine whether nonzero partial sample masks survive in a
different surface or device path.  Such captures should include both the
indexed command stream and the native program buffer uploaded from
`+0xbb0/+0xbb4`.  `tools/m9-raster-pack-evidence.py` pins the exact lowering,
call joins, and the negative dirty-group boundary.
