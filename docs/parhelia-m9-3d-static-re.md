# Matrox M9/Phoenix 3D static reverse engineering

This note is the pre-capture specification recovered from the saved Matrox
M-Series Windows and Linux drivers.  It covers indexed command construction,
the 3D state banks, shader program upload, the vendor compiler interface, and
the compiler's Iris instruction vocabulary.  It deliberately separates
compiler IR from the native two-dword hardware instruction format and its
eight-dword upload lines.

Confidence labels are:

* **[BIN]**: directly visible in a saved binary or its disassembly.
* **[DER]**: a deterministic result derived from binary instructions.
* **[INF]**: a likely meaning which still needs a differential capture or an
  M9148 test.

Addresses below are preferred-image virtual addresses for `MxMGLX.dll` and
section-relative virtual addresses for `MxMD.dll`.  They are useful for finding
the code again; they are not M9 MMIO addresses.  Named Linux functions use the
virtual `.text` addresses from their respective ELF binary.

## Binary identity

The principal inputs are:

| file | SHA-256 | role |
|---|---|---|
| `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` | OpenGL ICD, GLSL/ARB compiler and GL lowering |
| `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` | D3D/GDI driver, packet and native-program emission |
| Linux `xserver/1_13_0/m9x_drv.so` | `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273` | Linux GL/X implementation and submission path |
| Linux `KernelLibraries.o_shipped` | `bec4490ff72b29887400e988a26e107e80ebb0b11e9d0b437267bec9db5b26e7` | proprietary kernel hardware core |

The Windows build paths call this family `phoenix`.  The compiler source names
left in the ICD include `PsCompile.cpp`, `PsTokenStream.h`, `PsIris.cpp`,
`PsIris.h`, `PsTranslate.cpp`, `PsTrace.cpp`, `PsIrisTrace.cpp`, and
`PsTranslateBuiltins.cpp`.  Its public version string is `1.10 Matrox GLSL
compiler` [BIN].

## Indexed command packets

An ordinary state packet begins with a dword containing four byte-sized register
indices in little-endian execution order.  `0x45` ends a short header and fills
the remaining bytes [BIN].  An index maps to the engine window as
`0x1000 + index * 4` [BIN].  Examples are:

| header dword | writes | engine offsets |
|---:|---|---|
| `0x45454569` | `69` | `0x11a4` |
| `0x45456a69` | `69, 6a` | `0x11a4, 0x11a8` |
| `0x457a7978` | `78, 79, 7a` | `0x11e0, 0x11e4, 0x11e8` |
| `0x63626160` | `60, 61, 62, 63` | `0x1180..0x118c` |

The last example has no terminator because all four slots are live.  Command
buffer bookkeeping places a count/control dword before the header in the paths
examined.  The exact outer record semantics belong to the requester/DMA ABI and
must not be inferred from `0x45` alone.

Frequently used banks therefore occupy these engine offsets [DER]:

| indices | engine offsets | recovered role |
|---|---|---|
| `60..67` | `0x1180..0x119c` | eight program/vector data ports |
| `68..6e` | `0x11a0..0x11b8` | program selector, address, and control bank |
| `70..73` | `0x11c0..0x11cc` | four texture-unit selectors |
| `78..87` | `0x11e0..0x121c` | shared texture descriptor fields |
| `90..9f` | `0x1240..0x127c` | 16-entry common state bank |
| `a8..be` | `0x12a0..0x12f8` | render/depth surface bank, with unused indices between proven groups |
| `c0..c7` | `0x1300..0x131c` | surface and adjacent state |
| `e3,e6` | `0x138c,0x1398` | command allocation/resource result and frequent transition command |

[`tools/m9-packet-scan.py`](../tools/m9-packet-scan.py) finds candidate
`0x45`-terminated header constants in an x86 binary or existing Intel-syntax
`objdump` output.  It intentionally omits full four-index headers because an
arbitrary immediate cannot be classified safely without data-flow context.
Terminated matches also need contextual confirmation because unrelated x86
immediates can accidentally have the same byte pattern.

[`tools/m9-command-decode.py`](../tools/m9-command-decode.py) handles the
complementary case: it decodes raw command-buffer dumps, where packet framing
makes full four-index headers unambiguous, into index/MMIO/value writes or JSON.
It also recognizes the recovered H03 numeric-range, H07 32-bit-address, and H06
packed-16 draw records and derives their spans or element counts.  It rejects
embedded-stop, truncated, reversed, and malformed-range records so a bad
capture does not silently desynchronize the stream.  The decoder also joins
the exact consecutive `68`, `60..63`, `64..67` native-upload sequence into one
32-byte line containing four numbered two-dword instructions.  A half-port
sparse constant write is deliberately left as ordinary packets.

As a cross-build check, the scanner finds 355 sites and 143 distinct terminated
header immediates in `MxMD.dll`, and 180 sites/82 immediates in Linux
`xserver/1_13_0/m9x_drv.so`.  Seventy-five exact header immediates occur in both
the independently compiled 32-bit Windows and 64-bit Linux drivers [DER].  The
shared set includes the texture, surface, program-port, `18/19`, `1a/42`,
`2c/2d`, `2e/2f`, `e3`, `e6`, and main raster groups described below.  This
substantially lowers the chance that those constants are x86 data coincidences.

## The driver's context-index template

`MxMD.dll` function `0x65430` constructs a 248-byte template at runtime and
copies it into each object.  This is the densest static inventory of 3D-visible
indices in the binary [BIN].  The following rows retain its dword boundaries;
`--` denotes a `0x45` stop byte.  Repeated `47` bytes are preserved because
their command meaning is not yet proven.

```text
47 47 -- -- | f0 ef ed ee | ec f2 f3 f4 | 47 47 47 47
b0 b1 b2 b3 | b4 b5 b6 b7 | b8 b9 ba bb | be bc bd --
47 47 47 47 | a8 a9 aa ab | ac ae f5 f6 | af c0 a7 a5
c4 c5 c6 c7 | 47 -- -- -- | a1 a3 a4 ad | 47 47 47 47
88 89 8a c2 | c3 8c 8d 8e | 74 78 79 75 | 78 79 76 78
47 -- -- -- | 79 77 78 79 | 47 47 47 47

70 78 79 7a | 7c 7d 82 83 | 84 85 80 81 | 7e 7f 87 --
47 47 47 47 | 71 78 79 7a | 7c 7d 82 83 | 84 85 80 81
7e 7f -- -- | 47 47 47 47 | 72 78 79 7a | 7c 7d 82 83
84 85 80 81 | 7e 7f -- -- | 47 47 47 47 | 73 78 79 7a
7c 7d 82 83 | 84 85 80 81 | 7e 7f -- --

47 47 47 -- | ea eb d0 d1 | ed d3 d4 -- | 8f 8b d6 c1
47 47 47 47 | 90 91 92 93 | 94 95 96 97 | 98 99 9a 9b
9c 9d 9e 9f | 47 47 47 47 | 68 69 6a 6b | 6c 6d 6e --
60 61 62 63 | 64 65 66 67 | 47 47 47 -- | f1 d7 f3 --
d7 d8 d9 da | db dc dd de | 47 -- -- -- | d2 a2 d5 e8
```

The four nearly identical blocks selected by `70`, `71`, `72`, and `73` are
strong evidence for four texture units in this particular state path [DER].
Indices `78..87` form their shared descriptor payload.  The contiguous
`90..9f` bank contains 16 state values, while `68..6e` plus `60..67` is the
program/parameter upload bank [DER].  Names for the other groups remain
provisional.

## State emitters and register clusters

The following `MxMD.dll` functions build real packet payloads.  This table is a
map for later annotation, not a final register-name header.

| function | observed indices | static conclusion |
|---:|---|---|
| `0xf4350` | `f0`, dynamic `70 + unit`, `78..7a`, `7c,7d,87`, `7e..85`, `c0` | texture/sampler state, four selectable units [BIN][DER] |
| `0xf4f50` | `ef`, `e6,8f`, `90..92`, dynamic `00`, `70`, `f0` | common 3D setup and state-bank selection [BIN] |
| `0xf5730`, `0xf59b0` | `c2,c3,8f`, `e6,e3` | resource/cache transition path [BIN][INF] |
| `0xf6f20` | `ec,f4,ea,eb`, `30`, `28`, repeated `23`, `d0,d1`, then program helpers | central dirty-state emitter [BIN] |
| `0xf74d0` | `69,6a,6d,e3,68,60..67` | external fixed-fragment-database bind/control group or alternative inline native upload; the external address translation remains unresolved [BIN][CAP] |
| `0xf78c0` | `68,6b,6c,60..67` | auxiliary resource/control group; live `6c` selects a tagged system record [BIN][CAP] |
| `0xf7c60` | `68,60..67` | another complete auxiliary record [BIN] |
| `0xfb920` | `b8..be`, `24`, `bc,bd`, `af`, `a8..ae`, `f1` | render/depth surface group [BIN][INF] |
| `0xfc0e0` | `f0,ef,d7`, `ee`, `22`, `a7,c6` | late raster state group [BIN] |
| `0xfc3a0` | `a5,d3` | paired state [BIN] |
| `0xfc620`, `0xfcb60` | `44,2e,2f`, with `e6` in the latter | cache/TLB/address-domain synchronization records [BIN][DER] |
| `0xfc8f0` | `1a,42`, sometimes followed by `e6` | cache/TLB/resource transition [BIN][DER] |
| `0xfb260` | `19,18` | paired input/geometry state candidate [BIN][INF] |

Index `e6` occurs in many state transitions and payload builders.  It is a
general command/transition port whose values cover ordering operations and an
inline-data count; it must not be assigned one global barrier or draw meaning.
Its value encoding is not yet complete.  Indices `2c/2d` occur as a pair in many
render paths and agree with the earlier sub-buffer start/end finding.  Neither
should be admitted to a kernel command whitelist until its address and length
constraints are understood.

Top-level commit function `0xfadf0` emits these blocks in the fixed order
`0xfc0e0` (late state), `0xfb920` (surfaces), `0xf4f50` (common banks),
`0xf6f20` (program/vector state), then `0xfb260` (paired input state), and
finally clears the context dirty word at `+0xc` [BIN].  The open driver should
preserve this ordering until captures prove which dependencies can be relaxed.

The task-object joins are exact [BIN].  Constructor `0x1df90` allocates and
initializes the 0x90-byte late-state object, which is stored at outer D3D
context `+0x2538`.  Calls at `0x102f49..0x102f56` and
`0x11bddd..0x11bdf0` feed that object to binder `0x10a730`; the binder writes
it to wrapper `+0x20` and to task `+0xc4`.  Function `0x20770`, duplicated at
`0x13db29..0x13db58`, establishes the complete emitter routing:

| task field | bound object | consumer |
|---:|---|---:|
| `+0xb4` | per-batch `+0x3e8` | `0xfb920` |
| `+0xb8` | per-batch base | `0xf4f50` |
| `+0xbc` | program state `+0xc04` | `0xf6f20` |
| `+0xc0` | wrapper input state | `0xfb260` |
| `+0xc4` | D3D context `+0x2538` late state | `0xfc0e0` |

This closes the structural object-to-emitter join but does not by itself map
the frontend `p2514` render-state shadow to final indices.  The depth/stencil
exception is joined below; no observed copy or lowering path justifies
assigning the remaining blend/raster names to neutral banks `88..9f`.

### Texture shadow layout

The texture emitter at `0xf4350` exposes its complete per-unit dirty-state
layout [BIN].  The object at `edi` uses `+0xc` as the unit number and its first
dword as a dirty mask.  It clears that mask after emission.  The individual
groups are:

| dirty bit | indices | source fields |
|---:|---|---|
| common | dynamic `70 + unit` | `+0x10` |
| `0x02` | `78,79,7a` | `+0x4c`, `+0x50`, plus relocated resource `+0x44/+0x48` |
| `0x04` | `7c,7d,87` | `+0x14`, `+0x18`, `+0x54` |
| `0x08` | `82,83,84,85` | `+0x2c`, `+0x30`, `+0x34`, `+0x38` |
| `0x10` | `80,81,7e,7f` | `+0x24`, `+0x28`, `+0x1c`, `+0x20` |
| `0x20` | `c0` plus relocation | zero, then resource `+0x3c/+0x40` |

The emitter also rewrites bits `14:13` of the current `f0` value with the unit
number before sending it.  The relocation callback receives selectors
`unit + 0x0b` for the first resource and `unit + 0x0f` for the second [BIN].
This gives the Mesa state encoder an exact shadow grouping and dirty-bit model
whose filtering, wrap, format, extent, and address subfields are decoded below.

The frontend-to-backend join now identifies index `7c` filter, anisotropy, wrap,
resource-format and condition fields; index `7d` LOD bias/bounds; indices
`7e..81` converted border color; and indices `82..85` bump-environment matrix.
The exact tables and bit fields are in `parhelia-m9-texture-state.md`.  Resource
method `+0x5c` is now flattened into the bit layout for indices `78/79`,
including the seven-bit hardware format field.  Index `78[15:0]` is the aligned
horizontal row span in format elements, `79[15:0]` is the ordinary vertical
extent, and common D3D color, luminance, vector, float, depth, and DXT formats
are joined to hardware codes.  Resource dimensions are proven as 2D, cube, and
volume/3D.  The live selector-2 view leaves indices `78[19]`, `78[20]`,
`78[22]`, and `78[24]` clear and has no separate D3D channel-swizzle field.
The visible class-3 block geometry, ordinary volume-mip offset recurrence, and
packed-tail 32x16 atlas are exact.  Tail upload coordinates and byte offsets
are recovered for uncompressed and DXT resources.  Only the physical
permutation within class-3 blocks remains unresolved.  Index `87` remains
reserved.

### Common state-bank shadow layout

The common emitter at `0xf4f50` maps another group of dirty bits and shadow
fields exactly [BIN]:

| dirty bit | indices | source fields or action |
|---:|---|---|
| `0x01` | `88,89,8a,c1` | `+0x10`, `+0x14`, `+0x18`, `+0x1c` |
| `0x02` | helper `0xf5730` | resource/cache transition group |
| `0x04` | helper `0xf59b0` | resource/cache transition group |
| `0x10` | `90,91,92` | `+0x3a8`, `+0x3ac`, `+0x3b0` |
| `0x20` | `94,95,96,97` | `+0x3b4`, `+0x3b8`, `+0x3bc`, `+0x3c0` |
| `0x40` | `98,99,9a,9b` | `+0x3c4`, `+0x3c8`, `+0x3cc`, `+0x3d0` |
| `0x100` | `9c,9d,9e,9f` | `+0x3dc`, `+0x3e0`, `+0x3d4`, `+0x3d8` |

The `0x10..0x100` groups therefore form four independently dirty slices of
the 16-entry `90..9f` bank [DER].  The nonsequential source order in the last
slice is intentional and must be preserved.  The surrounding path also emits
the fixed setup pair `e6 <- 0x04000001`, `8f <- 0x80000600`, dynamically
constructs indexed resource packets, and refreshes selectors `70` and `f0`.
The point-fill path above assigns a gated `c1[11:0]` effect; the other fields
in this group remain unnamed.  Static evidence does not yet assign GL blend,
viewport, the remaining raster state, or linkage names to the four slices, so
the driver should retain neutral bank names until the differential capture
matrix labels them.

The late-state emitter at `0xfc0e0` supplies another compact exact map [BIN]:

| dirty bit in `+0x0` | indices | source fields |
|---:|---|---|
| `0x01` | `f0,ef,d7` | `+0x28`, `+0x20`, `+0x24` |
| `0x02` | `ee` | `+0x1c` |
| `0x04` | `ec,f2,f3,f4` | `+0x18`, `+0x2c`, `+0x30`, `+0x34` |
| `0x10` | `22` | `+0x38` |
| `0x20` | `a7,c6` | either `+0x3c/+0x40` or `+0x60/+0x64`, selected by `+0x68` |

Its secondary mask at `+0xc` bit `0x08` calls the `1a/42` emitter at
`0xfc8f0` with fields `+0x7c..+0x88`; bit `0x04` calls the `44/2e/2f/e6`
emitter at `0xfcb60`.  The function derives `+0x68` from two live object
pointers and fields in `+0x28`, so the `a7/c6` pair is conditionally selected
state rather than an independent dirty block [BIN].

### Direct3D semantic anchors

`MxMD.dll` function `0x103370` is the Direct3D render-state dispatcher.  It
takes the numeric `D3DRS` ID in `edx` and value in `eax`; IDs `2..219` pass
through the byte dispatch table at `0x104bf4` and handler jump table at
`0x104990` [BIN].  Static handler data flow gives exact frontend bit packing
for the following states.  Here `p2514` means the object pointer at context
`+0x2514`, and similarly for `p2518` and `p2528`.

[`tools/m9-d3d-state-map.py`](../tools/m9-d3d-state-map.py) reproduces the
complete dispatcher directly from those two PE tables.  It resolves all 218
IDs in the accepted range without depending on guessed function boundaries:
150 IDs have a real handler and 68 reach the common unsupported return at
`0x104986` [DER].  The implemented group includes 140 public legacy/D3D9
names plus ten driver-private IDs `210..219`.  The private handlers are real,
but their API names must remain unknown until the private-state producer is
found.  One subtle table detail is that unsupported state 62 uses selector
`0x2f`, while the other unsupported entries use selector `0x98`; both slots
contain the same `0x104986` target [BIN].

The unsupported public states are also useful negative capability evidence:
texture perspective; Z visibility; subpixel modes; stippled alpha and stipple
enable; edge antialias; legacy flush/sort; extents; patch edge style; the debug
monitor token; position/normal degree; antialiased lines; and adaptive
tessellation states all dispatch to the common no-op/error path [BIN].  The 32
legacy stipple-pattern values `64..95` do have individual storage handlers even
though `STIPPLEENABLE` itself is unsupported; this proves frontend acceptance,
not a usable stippled-raster path.

The ten accepted IDs beyond the public D3D9 enum lower as follows [BIN]:

| private ID | exact frontend effect | semantic conclusion |
|---:|---|---|
| `210..212` | store to `p2514+0x78,+0x7c,+0x80`; increment serial `+0x68`; dirty `+4 |= 0x200000` | three additional two-sided-stencil values, strongly matching independent CCW ref/read-mask/write-mask [INF] |
| `213` | value bit 0 -> `p2520+0x18 bit5` and `p2514+0x0c bit20`; dirty `p2520+0 |= 0x80000000`, `p2514+8 |= 0x10` | coupled raster/program boolean, exact API meaning unknown |
| `214` | value bit 0 -> `p2528+0x48 bit15`; dirty `p2528+0 |= 0x80` | raster boolean, exact API meaning unknown |
| `215..218` | store to `p2514+0x110..+0x11c`; set per-target dirty bits `0x40..0x200`, then use the same common path as public color-write states | `COLORWRITEENABLE4..7` extension [DER] |
| `219` | value bit 0 -> `p2514+0xf4 bit5`; dirty `+4 |= 0x2000` | alpha-test-group boolean, exact API meaning unknown |

The `215..218` identification follows directly from the contiguous layout:
public target-zero state 168 stores at `+0x100`, public targets 1..3 at
`+0x104..+0x10c`, and the private four handlers continue the same dword and
dirty-bit sequence for targets 4..7 [DER].  This is static evidence that the
frontend represents eight color-write masks even though another hardware
limit may reduce the simultaneously active render targets.

| D3DRS ID | handler | frontend shadow update | dirty update |
|---|---:|---|---|
| `ZENABLE` 7 | `0x103857` | `p2514+0xc[6:5] = (v&1)<<5`; `p2528+4 bit2 = v&1`; `p2518+0x1a8 bit9 = v&1` | `p2514+4 \|= 0x100000`; `p2528+0 \|= 0x10`; `p2518+0 \|= 0x40000` |
| `ZWRITEENABLE` 14 | `0x1038cb` | `p2514+0xc bit1 = v&1` | `p2514+4 \|= 0x100000` |
| `ZFUNC` 23 | `0x1038db` | `p2514+0xc[10:7] = (v&0xf)<<7` | same `0x100000` |
| `SLOPESCALEDEPTHBIAS` 175 | `0x104267` | raw `p2528+0x20`; nonzero in `p2514+0xc bit22` | `p2528+0 \|= 0x1000`; `p2514+8 \|= 0x40` |
| `DEPTHBIAS` 195 | `0x1042a9` | raw `p2528+0x24`; nonzero in `p2514+0xc bit21` | same two masks |

Stencil packing in `p2514` is [BIN]:

| D3DRS ID | handler | field | dirty update |
|---|---:|---|---|
| `STENCILENABLE` 52 | `0x103903` | `+0xc bit3` | `+4 \|= 0x200000` |
| `STENCILFAIL` 53 | `0x10392d` | `+0x10[3:0]` | same |
| `STENCILZFAIL` 54 | `0x103952` | `+0x10[7:4]` | same |
| `STENCILPASS` 55 | `0x103968` | `+0x10[11:8]` | same |
| `STENCILFUNC` 56 | `0x10397b` | `+0x10[15:12]` | same |
| `STENCILREF` 57 | `0x103a2d` | raw `+0x6c` | same |
| `STENCILMASK` 58 | `0x103a38` | raw `+0x70` | same |
| `STENCILWRITEMASK` 59 | `0x103a43` | raw `+0x74` | same |
| `TWOSIDEDSTENCILMODE` 185 | `0x10398e` | `+0xc bit4` | same |
| `CCW_STENCILFAIL` 186 | `0x1039b4` | `+0x10[19:16]` | same |
| `CCW_STENCILZFAIL` 187 | `0x1039c7` | `+0x10[23:20]` | same |
| `CCW_STENCILPASS` 188 | `0x1039da` | `+0x10[27:24]` | same |
| `CCW_STENCILFUNC` 189 | `0x103a02` | `+0x10[31:28]` | same |

Blend and alpha packing in `p2514` is [BIN]:

| D3DRS ID | handler | field | dirty update |
|---|---:|---|---|
| `ALPHATESTENABLE` 15 | `0x103815` | `+0xf4 bit4` | `+4 \|= 0x2000` |
| `ALPHAREF` 24 | `0x1036c8` | low byte of `+0xec` | same |
| `ALPHAFUNC` 25 | `0x103829` | `+0xf4[10:7]` | same |
| `ALPHABLENDENABLE` 27 | `0x10367c` | low byte of `+0xf8` | `+4 \|= 0x4000` |
| `SRCBLEND` 19 | `0x1036ed` | `+0xf8[24:20]` | same |
| `DESTBLEND` 20 | `0x10371c` | `+0xfc[4:0]` | same |
| `BLENDOP` 171 | `0x10375d` | `+0xf8[11:9]` | same |
| `SEPARATEALPHABLENDENABLE` 206 | `0x1036af` | `+0xf8 bit8` | same |
| `SRCBLENDALPHA` 207 | `0x103706` | `+0xf8[19:15]` | same |
| `DESTBLENDALPHA` 208 | `0x103747` | `+0xf8[29:25]` | same |
| `BLENDOPALPHA` 209 | `0x103773` | `+0xf8[14:12]` | same |
| `BLENDFACTOR` 193 | `0x1037a1` | raw `+0xd0`, serial at `+0xcc` | `+4 \|= 0x8000` |
| `SRGBWRITEENABLE` 194 | `0x1037e7` | `+0xf4 bit11` | `+4 \|= 0x10000` |

Raster and output anchors are [BIN]:

| D3DRS ID | handler | shadow update | dirty update |
|---|---:|---|---|
| `CULLMODE` 22 | `0x104172` | `p2528+4[1:0] = v&3` | `p2528+0 \|= 0x4` |
| `SCISSORTESTENABLE` 174 | `0x103a9b` | `p2514+0xc bit2` | `p2514+4 \|= 0x400000` |
| `MULTISAMPLEANTIALIAS` 161 | `0x10360b` | `p2514+0xf4 bit0` | `p2514+4 \|= 0x1000` |
| `MULTISAMPLEMASK` 162 | `0x103635` | raw `p2514+0xe8` | same |
| `COLORWRITEENABLE` 168 | `0x103454` | raw `p2514+0x100` | composite dirty path at `0x103517` |
| `COLORWRITEENABLE1` 190 | `0x103465` | raw `+0x104` | `+4 \|= 0x8`, then common dirty path |
| `COLORWRITEENABLE2` 191 | `0x103480` | raw `+0x108` | `+4 \|= 0x10`, then common dirty path |
| `COLORWRITEENABLE3` 192 | `0x103498` | raw `+0x10c` | `+4 \|= 0x20`, then common dirty path |

`FILLMODE` 8 dispatches to `0x10405d` and writes its masked value to
`p2528+0x48[5:4]`, `p2514+0x0c[16:15]`, and `p252c+0x0c[1:0]`, with dirty
masks `0x2`, `0x08000000`, and `0x00040000` respectively.  `SHADEMODE` 9 at
`0x1040c9` writes `p2528+0x48[7:6]` and `p252c+0x0c[3:2]`, with dirty masks
`0x8` and `0x00080000`; it also sets `p2520+0x18 bit 0` exactly when the value
is `D3DSHADE_FLAT` (1), marking `p2520` dirty `0x80` on a change [BIN].  These
D3D objects are distinct from the
lower command-shadow objects emitted by `0xf4f50`, `0xfb920`, and `0xfc0e0`.
Depth/stencil, viewport/scissor, and the compiled multisample pair are joined
to final indices below.  Blend, alpha test, and format-dependent color masks
are fragment-compiler inputs.  Point fill has one gated final-command effect
at index `c1`.  Builder `0x13a0e0` packs fill into compiler key
`+0x78[1:0]` and shade into `[5:4]`; `0x13ab40` packs cull into `[7:6]`.
The exact enum contributions are point/wireframe/solid `1/2/3`,
flat/Gouraud/Phong `0x10/0x20/0x30`, and none/CW/CCW
`0x40/0x80/0xc0`.  Their frontend names must not yet be attached directly to
packet registers because this compiler key has no proved final-index join
[BIN][INF].

### Depth/stencil frontend-to-command join

The fixed-state validator constructed by `0x136940` contains a copy of the
`p2514` frontend state at validator `+0x80` and a nested compiled-state object
at validator `+0x1b8`.  `0x137110` installs those bindings: instructions
`0x137159..0x137168` bind the frontend copy, while `0x13716b..0x137171` publish
the nested object through core `+0xfc` [BIN].  The validator at `0x137f20`
lowers depth/stencil into three dwords at validator `+0x34c,+0x350,+0x354`,
which are nested-object `+0x194,+0x198,+0x19c`, and marks nested dirty mask
`0x01021060` at `0x137f3b` [BIN].

The selected draw-state instance with this same nested layout reaches
`0x181900`.  That function copies nested `+0x194` to surface shadow `+0x14` at
`0x18190a`, nested `+0x19c` to `+0x1c` at `0x181913`, and nested `+0x198` to
`+0x18` on the two format-dependent paths at `0x1822b9` and `0x1823c5`.  Both
paths mark surface dirty bit `0x100` (`0x1822bf` or `0x1823d9`) [BIN].  The
surface emitter then tests that bit at `0xfbe58`, emits header `0x45aaa9a8` at
`0xfbe7f`, and copies `+0x14,+0x18,+0x1c` at `0xfbe89,0xfbe92,0xfbe9b`
respectively [BIN].  The resulting exact join is:

| final M9 index | compiled offset | proven role |
|---:|---:|---|
| `a8` | `+0x194` | depth enable/write/compare plus two-sided stencil enable, compares, and operations |
| `a9` | `+0x198` | first stencil ref/read-mask/write-mask triplet plus a format nibble |
| `aa` | `+0x19c` | second stencil ref/read-mask/write-mask triplet |

The intervening draw-state cache selects an instance of the compiled layout;
the static analysis has not yet proved whether it aliases or copies the
validator's original nested object.  It does not change the field offsets or
the final command join [DER].

`0x137f20` builds `a8` as follows [BIN]:

| `a8` bits | source and conversion |
|---:|---|
| `0` | depth enabled when `(p2514+0x0c & 0x60) == 0x20` (`0x137f59..0x137f75`) |
| `1` | `ZWRITEENABLE`, from `p2514+0x0c bit 1` (`0x137f4e..0x137f75`) |
| `4:2` | `cmp_map((p2514+0x0c >> 7) & 0xf)`, `ZFUNC` (`0x137f77..0x137f8f`) |
| `5` | stencil enable from `p2514+0x0c bit 3`; cleared if the attached depth/stencil format is incompatible (`0x137fa9..0x138009`, `0x13818c..0x13819e`) |
| `6` | two-sided stencil enable from `p2514+0x0c bit 4`, subject to the validator's raster-mode gate (`0x138009..0x13805e`) |
| `9:7` | selected face stencil compare |
| `12:10` | selected face stencil-fail operation |
| `15:13` | selected face depth-fail operation |
| `18:16` | selected face pass operation |
| `21:19` | other face stencil compare |
| `24:22` | other face stencil-fail operation |
| `27:25` | other face depth-fail operation |
| `30:28` | other face pass operation |

In normal face order, the low compare/operation quartet comes from frontend
`+0x10` nibbles `15:12,3:0,7:4,11:8`, and the high quartet comes from nibbles
`31:28,19:16,23:20,27:24` (`0x13829f..0x138361`, with the shared tail at
`0x138161..0x138182`) [BIN].  The alternate path swaps those quartets
(`0x13809f..0x138182`).  Frontend `+0x0c bit 20` participates in that face
selection, but its exact public/API meaning remains unknown.  An internal
override controlled by validator `+0x9a4` also forces the depth compare field
at `0x137f9b..0x137fa3`; it must be preserved until its caller is identified
[BIN].

The comparison remap helper at `0x139670` converts Direct3D enum values to
three-bit hardware codes [BIN]:

| D3D comparison | `NEVER` | `LESS` | `EQUAL` | `LESSEQUAL` | `GREATER` | `NOTEQUAL` | `GREATEREQUAL` | `ALWAYS` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| M9 code | `0` | `2` | `1` | `3` | `4` | `6` | `5` | `7` |

October 8 correction: the comparison table above follows the actual indirect
jump table at `1396ac`; the previous table incorrectly followed the linear
order of return blocks. The actual validator has now been executed for all
eight comparisons and both write masks. A complete FAA three-DRAW differential
also proves LESS/write `38038b`, LEQUAL/write `38038f`, ALWAYS/no-write `38039d`.
[Source check](../tests/m9-faa-depth-state-source-20261008/README.md).

The stencil-operation remap at `0x1396e0` likewise converts Direct3D values
[BIN]:

| D3D operation | `KEEP` | `ZERO` | `REPLACE` | `INCRSAT` | `DECRSAT` | `INVERT` | `INCR` | `DECR` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| M9 code | `0` | `3` | `1` | `6` | `7` | `2` | `4` | `5` |

The common one-face path at `0x1381bb..0x138292` places stencil reference,
read mask, and write mask in bytes `0,1,2` of both `a9` and `aa`.  It obtains
them from frontend `+0x6c,+0x70,+0x74` and masks them through an attachment
format callback [BIN].  The two-face paths at `0x138366..0x13852a` assign that
triplet to one word and frontend `+0x78,+0x7c,+0x80` to the other, swapping
the two words when the face order swaps [BIN].  Private render states
`210..212` write the second triplet, so its independent two-sided-stencil
role is strong, but the exact private API contract is still inferred [DER].
Finally, `0x138530..0x138550` places a format-dependent helper result from
`0x139750` in `a9[27:24]`; static evidence does not yet justify a more specific
name for that nibble.

The remaining frontend-to-final-index boundary is now precise [BIN][DER]:

* `0x138c10` compiles viewport and scissor inputs into validator
  `+0x410,+0x414,+0x418,+0x41c` through helper `0x138cd0` and marks nested
  dirty bit `0x04000000` at `0x138cb6`.  These are nested offsets
  `+0x258,+0x25c,+0x260,+0x264`.  Lowerer `0x181fa0` tests that bit at
  `0x18283e`, copies the four words to surface shadow
  `+0xa4,+0xa8,+0xac,+0xb0`, and sets surface dirty bit `0x400` at
  `0x182870`.  Emitter `0xfb920` consequently writes them to final indices
  `f5,f6,c4,c5` [BIN].  The exact rectangle construction and packing are
  recorded in `parhelia-m9-render-surfaces-clear.md`.
* `CULLMODE` remains in the separate `p2528` raster object.  Raster clone
  `0x1119b0` preserves `+4[1:0]` and dirty `0x4`.  Builder `+0x1c` is exactly
  `p2528+4`; `0x13ab40` shifts its low bits into compiler key
  `+0x78[7:6]`, distinguishing none/CW/CCW as `0x40/0x80/0xc0`.  It also
  creates gated per-target record `+0x80[25:24]` effects and marks dirty group
  `0x2`, for which emitter `0xf6f20` has no case.  `0x17e8d0` separately uses
  `CULL_NONE` only for draw-path eligibility.  A final indexed write remains
  unproved, and the `a8` face-order selection must not be inferred from this
  separate state [BIN].
* Render-entry constructor `0x17edb0` keeps the live `p2528` pointer at entry
  `+0x14` and a per-batch common command shadow at `+0x24`.  Helper `0x17f040`
  recognizes `D3DFILL_POINT` (`p2528+0x48[5:4] == 1`) and, under entry/primitive
  gates, sets shadow `+0x1c[11:0]` to `0xfff`; failure of any gate clears
  `+0x1c[15:0]`.  It marks common group `0x01` dirty, and
  `0xf4f50` emits shadow `+0x1c` as final index `c1`.  Bits `15:12` also depend
  on raster bit 8 and per-item flags, so their name is still open.  This joins
  a point-fill consequence to `c1[11:0]`.  Compiler key `+0x78[1:0]`
  distinguishes point/wireframe/solid as `1/2/3`, and `[5:4]` distinguishes
  flat/Gouraud/Phong as `0x10/0x20/0x30`, but neither field is joined to a
  final index [BIN].
* Emitter `0xf6f20` writes per-target record `+0x78` to index `30`.  This is
  not compiler-builder `+0x78`: the record comes from builder
  `+0x74 + target*0xc04`, and no copy between the equal-numbered fields occurs
  in `0x13a020..0x13c610` [BIN].
* Multisample compiler `0x139240` reads `MULTISAMPLEANTIALIAS` from validator
  `+0x174` (`p2514+0xf4 bit 0`) and rebuilds nested words `+0x1b0,+0x1b4`.
  `0x181fa0` copies them to surface `+0x28,+0x2c`, and `0xfb920` emits them as
  four replicated writes to final index `ad` followed by index `af` [BIN].
  `MULTISAMPLEMASK` has only a zero-value join at `0x138be2`, where it can
  suppress a target color-output mask; no nonzero mask bits reach a fixed
  index in this saved path.
* `0x101550` canonicalizes blend, alpha-test, color-mask, and multisample
  frontend fields into a flat D3D cache, but it is not a command emitter.
  Blend is lowered per render target by compiler routine `0x170ff0`, alpha
  test by `0x170800`, and color masks by `0x1389a0`/`0x173ee0`.  Finalizer
  `0x1397e0` produces and caches a 256-byte compiler-result record.  Backend
  builder `0x13eea4` reads only its first dword as an offset from the program
  object's `+0xa0` base, so the complete record is not an uploaded native
  image.  Linker `0x13bf80` instead produces the native buffer at program state
  `+0xbb0/+0xbb4`, which `0xf74d0` uploads through selector `68` and ports
  `60..67`.
  `0x13b570` belongs to fixed-function vertex/declaration conversion and is
  unrelated to the blend dirty bit.  The exact maps and remaining raster
  boundaries are in `parhelia-m9-blend-raster-lowering.md` [BIN][DER].

### Render and depth surface shadow layout

The surface emitter at `0xfb920` provides a second exact shadow-state map
[BIN].  The object at `edi` has command dirty bits at `+0x0`, resource dirty
bits at `+0x8`, a live attachment count at `+0x34`, attachment resource pairs
at `+0x38`, and one state dword per attachment at `+0x58`.  Both attachment
arrays advance with the loop index.  The emitted indices and fields are:

| dirty bit in `+0x0` | indices | source fields or relocation |
|---:|---|---|
| bits `0..3`, selected by attachment number | `b0,b1`; `b2,b3`; `b4,b5`; `b6,b7` | state `+0x58 + 4*n`; resource pair `+0x38 + 8*n`; relocation selector `n + 2` |
| `0x10` | `b8,b9` | `b9 <- +0x70`; resource `+0x68/+0x6c`, selector `6` |
| `0x20` | `ba,bb` | `bb <- +0x7c`; resource `+0x74/+0x78`, selector `7` |
| `0x40` | `24`, `e6,e6,e6`, `bc,bd` | resources `+0x9c/+0xa0`, `+0x80/+0x84`, `+0x94/+0x98`, selectors `0x2b`, `9`, `0xa`; fixed `e6` values `0x6dc00000,1,1` |
| `0x80` | `ad,ad,ad,ad`, then `af` | `ad <- +0x28` four times; `af <- +0x2c` |
| `0x100` | `a8,a9,aa` | `+0x14`, `+0x18`, `+0x1c` |
| `0x200` | `ab,ac,ae` | `+0x20`, `+0x24`, `+0x30` |
| `0x400` | `f5,f6,c4,c5` | `+0xa4`, `+0xa8`, `+0xac`, `+0xb0` |
| `0x800` | `be` | resource `+0x8c/+0x90`, selector `8` |
| `0x1000` | `f1` | `+0xbc` |

The first four pairs are conclusively four indexed color render-target slots.
Lowerer `0x181fa0` decodes the fragment-output declaration, copies its selected
resource address/relocation/descriptor to `+0x38/+0x3c/+0x58 + slot*8/4`, and
sets `1 << slot` at `0x182096..0x1820f3` [BIN].  The driver then derives the
packet headers from byte offsets `0x2c0/0x2c4`, increments the offsets by eight
for each slot, and bounds the loop with `+0x34` [DER].  Thus `b0/b1` through
`b6/b7` are the four color attachment address/descriptor pairs [BIN].

The same lowerer joins the enabled depth/stencil attachment to two
relocation-backed pairs: nested `+0x21c/+0x220/+0x224` becomes surface shadow
`+0x68/+0x6c/+0x70` and final `b8/b9`; nested
`+0x228/+0x22c/+0x230` becomes `+0x74/+0x78/+0x7c` and final `ba/bb`
(`0x182201..0x182291`, with the update path at
`0x182549..0x182620`) [BIN].  Constructor defaults identify the first pair:
`+0x224 = 0xb8010001` has depth format code `0x5c` (`D16`), while
`+0x230 = 0xf0010100` has `G16R16` format code `0x78`.  `HZFlags` allocator
`0x54be0` closes this second pair as hierarchical-Z metadata: it creates
internal format `0x0b` at `ceil(width/8) x ceil(height/4)`, and binder
`0xaaa7d` installs its selector-12 view in the `ba/bb` shadow.  Initializer
`0x55070` fills the whole plane with conservative word `0x007ff000`; clear
companion `0x54f80` seeds complete clears and invalidates the whole plane for
partial clears, while transition `0x55370` refreshes it after uncoupled writes.
The creator owns the auxiliary, clones are non-owning, and traversal
`0xa6dc4` tracks parent and auxiliary with the same arguments.  Alternate
lowerer `0x182b40` also installs resource view 9 at `b8/b9`, forces its format to a
depth code, and leaves `ba/bb` null.  Thus `b8/b9` is ordinary Z and `ba/bb`
is an optional hierarchical-Z metadata surface, not an independent API
stencil plane [BIN][DER].  The saved Linux build repeats both constructor
constants at `0x3bf008/0x3bf09d`.

Common view builder `0x467e0` also closes the render descriptor layout.
For color view 8 and depth view 9, descriptor bits `15:0` are aligned
horizontal span `layout+0x184`, bits `18:16` are layout selector `+0x138`,
bit 21 is layout flags dword 0 bit 3, bit 23 is `+0x14c bit 2`, and bits
`31:25` are format `+0x140`; bits `20:19` and 24 remain zero [BIN][DER].
The special view sets bit 22, moves `+0x150[3:0]` to address bits `5:2`, moves
row-span bit 5 to address bit 6, and either applies helper `0x46e40`'s aligned
offset or replaces the low span with the descriptor's high extent.  View 8
selects this for storage class 2; view 9 selects it for layout-flags bit 2.
The complete branch conditions are recorded in
`parhelia-m9-render-surfaces-clear.md`.  The `bc/bd` and `be` paths remain
other auxiliary relocation-backed surface state.

Before emitting commands, the same function walks every dirty resource pair
and calls the command-buffer resource tracker.  This proves that addresses in
these packet groups are relocation-backed and must not be accepted as raw
userspace GPU addresses [BIN].

Color clear leaf `0xa48b0` and depth/stencil leaf `0xb4fe0` converge on the
same ordered executor [BIN]: whole-allocation direct clear `0xb5e70`, GPU
rectangle draw `0xb5030`, then CPU fallback `0xb5a80`.  The direct clear emits
`e6,ac` with `e6=2` and the replicated clear word, followed by a `2a,2b,e6`
inline run with layout `0x454545e6`; its raw payload is an
eight-pages-per-word mask of the allocation.  It finishes with `e6=1`,
resource tracking, surface dirty bit
`0x200`, and transition close `0x55370(0)`.  The draw path binds resource view
8 as color slot 0 and launches the standard state/draw backend.  Its special
depth branch binds view 9 temporarily as color slot 0 and emits rectangle
records `0x504e4c48/0x5e4f4c48`; the same literals occur in the saved Linux
build.  The fallback maps and modifies 8--128-bit elements on the CPU.  Exact
eligibility tests, page-mask arithmetic, and cleanup are in
`parhelia-m9-render-surfaces-clear.md` [BIN][DER].

### Program and vector state shadow layout

The central emitter at `0xf6f20` uses the first dword of its state object as a
dirty mask.  Its directly decoded groups are [BIN]:

| dirty bit | action |
|---:|---|
| `0x01` | write `28 <- 0`, then emit each four-dword vector through a `23,23,23,23` header; vector storage begins at `+0x18` |
| `0x04` | emit up to four variable-length blocks selected by metadata at `+0xb90`; inline data begins at `+0x870` with a `0x64`-byte stride |
| `0x08` | `ec <- +0x8c`, `f4 <- +0x88`, `ea <- +0x90`, `eb <- +0x94` |
| `0x10` | `d0 <- +0x98`, `d1 <- +0x9c` |
| `0x40` | `30 <- +0x78` |
| `0x80`, `0x100`, `0x200` | call the native program/resource emitter at `0xf74d0` |
| `0x3c00` | call auxiliary upload emitter `0xf78c0` |
| `0x4000` | call auxiliary upload emitter `0xf7c60` |
| `0x8000` | walk 64 possible four-dword upload records beginning at `+0xc0`, with a `0x1c`-byte stride |

Each live `0x8000` record has a key at record `+0x0`, four payload dwords at
`+0x4..+0x10`, and a dirty/live field at `+0x14`.  The driver computes [DER]:

```c
selector = 0x80000000 | ((((key >> 1) + 0x80) & 0x1ff) << 5);
ports = (key & 1) ? indices_64_through_67 : indices_60_through_63;
```

It emits the selector through index `68` when it changes, sends the four
payload words through the selected half of the eight data ports, then clears
record `+0x14`.  This is an exact sparse vector/constant upload mechanism
[BIN][DER].  Record `+0x18` is pending dirty: state promotion at `0x13dac0`
copies it to current dirty `+0x14` for all 64 records.  These records have no
data-flow association with the 256-byte compiler-result cache at validator
`+0x248/+0x688`; native program bytes use the separate `+0xbb0/+0xbb4` path
described below [BIN].  Stage and register-class names still require a
controlled shader diff, but the address calculation, data width, capacity, and
dirty tracking can already be implemented.

### Fixed index-44 address-domain transitions

Functions `0xfc620` and `0xfcb60` each emit exactly eight writes to index `44`
(engine offset `0x1110`), followed by relocation-backed indices `2e/2f`
[BIN].  The two forms are:

| function | state resource | eight index-`44` values | other fixed writes |
|---:|---|---|---|
| `0xfc620` | object `+0x58` | `0x24081913` repeated eight times | `e6 <- 0x42000000` |
| `0xfcb60` | object `+0x74` | `0x28041968` repeated eight times | `e6 <- 0x42000000`, then two `e6 <- 3` writes |

Both paths compare the resource's low 29 address bits and upper-three flag
bits separately, track the resource before emitting, and call relocation
helpers with selectors `0x2c` and `0x2e`.  Caller context identifies these as
fixed-width cache/TLB/address-domain synchronization records [BIN][DER], not
vertex stream descriptors.  Their exact resource-domain semantics and the
meanings of the two repeated constants still need differential naming.

## Userspace requester submission

Linux function `m9x22842` at `0x303150` is identified by its
`BrdgDirectRequester::PostDOSBuffer` diagnostic and constructs the proven
`2c/2d` submission packet [BIN].  Its input has a bus-master object at `+0x0`
and byte size at `+0x8`.  Given bus-master base field `BM+0x34` and current
write offset `BM+0x44`, it emits [DER]:

```c
start = BM_34 | (BM_44 + 8);
end   = (BM_44 + 8 + size - 8) | 2;
emit(0x2c, start);
emit(0x2d, end);
```

Requester vtable slot `+0xd0` flushes and returns the fence; slot `+0x100`
submits with argument `1`.  Chained-buffer function `m9x17181` at `0x303c90`
uses the same pair.  Its optional post-link sequence is exactly [BIN]:

```text
e6e6e6e6 63000008 2c000000 62000020
ac000002 45e6e6e6 62804020 e04f7fff
b0000000
```

The sequence is likely ordering/cache work [INF]; its byte values and position
in the chain are established and should be retained as an opaque transition
until captures give each `e6` operation a semantic name.

Function `0xf9c70` in `MxMD.dll` exposes a direct inline-data path [BIN].  Its
sole caller `0x4ca40` is a 2D rectangle/stretch batch, not a generic 3D draw.
It emits header `2b,2a,e6`, writes `2b <- 0x00ffffff`, selects an inline-layout
word of `0x45fcf2f3` or `0xfcf2f379` through `2a`, and writes
`e6 <- 0x20000000 | ((count - 1) & 0xffff)`.  Each 24-byte input coordinate
record becomes packed dwords
`p0=(u16(+4)<<16)|u16(+0)`, `p1=(u16(+12)<<16)|u16(+8)`, and
`p2=(u16(+20)<<16)|u16(+16)`; output is an optional layout word followed by
`p1,p0,p2` [BIN][DER].

Function `0xb5e70` uses the same `2a/2b/e6` family for inline mask/pixel data
at depths `0x10,0x20,0x40,0x80` [BIN].  Therefore the high `e6` value denotes a
generic inline-data count/form.  It is not evidence for a 3D primitive launch;
the 3D draw launcher uses a different record family.

The recovered D3D callback table, topology mapping, 16 stream descriptors,
index descriptor, legacy and D3D9-style vertex-declaration IR, inline-vertex
frontend staging, and modern draw records are documented in
`parhelia-m9-draw-frontend.md`.  Context vtable slots `+0x08/+0x0c` are now
joined through the common backend to concrete command-buffer writes [BIN].
The launch is a three-dword record rather than an indexed-register packet:
family `0x03000001` carries numeric start/exclusive-end ranges, family
`0x07000001` carries relocation-backed 32-bit address ranges, and family
`0x06000001` carries relocation-backed packed-16 address ranges.  Launch flags
raise those headers into the `0x2?000001` or `0x6?000001` forms.  Primitive
topology and base-vertex byte adjustment are lowered before launch and are not
additional payload dwords [BIN][DER].

Linux `m9x1011` at `0x2be20` provides another exact fixed transition stream
[BIN].  It emits `45454524` and obtains index `24` through relocation type
`0x2b`; emits header `453ce6e6` with values `0x6dc00000`, `0x42000000`, and
`0x000000a1`; then emits two `44444444` packets whose eight index-`44` values
are all `0x30081898`; and ends with header `45452f2e` using relocation types
`0x2c` and `0x2f`.  These bytes are safe to preserve as an opaque transition,
but their semantic names remain provisional.

Linux `m9x528` at `0x2db30` emits header `453e421a` [BIN].  Index `1a` combines
tag fields at `+0x00` and `+0x18`; index `42` receives the tag `+0x10` pointer
advanced by `0x10` and ORed with 1; index `3e` combines resource fields `+0x34`
and `+0x44`.  This is an exact resource/tag-tail encoding and another anchor for
joining descriptor address domains.

## Kernel DMA ABI and hardware doorbell

The proprietary Linux `KernelLibraries.o_shipped` provides the matching static
kernel side.  Its ioctl dispatcher `m9x1185` at `0x4e80` masks the command with
`0xff` and exposes these relevant calls [BIN]:

| command | userspace ioctl | saved name/operation | structure size | core action |
|---:|---:|---|---:|---|
| `1` | `0xc0384601` | `GetBmBufferAddr` | `0x38` | vtable `+0x98` |
| `12` | `0xc840460c` | `ExecuteDma` | `0x840` | vtable `+0x80` |
| `13` | `0xc018460d` | `DrawOnSyncBuffer`, selector `1..4` | `0x18` | vtable `+0xe0/+0xe8/+0xf0` |
| `14` | `0xc010460e` | DMA front-end restart | `0x10` | writes `0x314 <- 1` |
| `15` | `0xc010460f` | DMA front-end clear | `0x10` | writes `0x31c <- 0` |
| `16` | `0xc0104610` | `SetCommandBufferSize` | `0x10` | vtable `+0xf8` |
| `18` | `0xc0304612` | `ReleaseBmBuffer` | `0x30` | vtable `+0xa0` |

The shipped source in `kernel/src/m9x_fops.c` validates ioctl type `'F'`, uses
number bit 7 to route public management calls, and sends private numbers below
`0x80` to this proprietary core dispatcher while holding the device mutex
[BIN].  Thus the full request values above and the low-byte command ordinals are
both ABI-significant; a new DRM uAPI should not inherit this old pointer-bearing
ABI directly.

DMA initialization function `m9x109` at `0x2d150` writes the following device
offsets through the vendor MMIO method [BIN]:

| offset | value construction |
|---:|---|
| `0x400` | `(tag_context[0x50] & ~3) \| BM[0x34]` |
| `0x109c` | `0x00080000` |
| `0x448` | zero |
| `0x1068` | `tag_context[0x18] \| tag_context[0x00]` |
| `0x106c` | `tag_context[0x40] \| tag_context[0x28]` |
| `0x10ec` | optional `((BM[0x44] + 0x200) & ~0x1f) \| (BM[0x34] & 3)` |
| `0x1094` | optional `(BM[0x44] + 0x200) & ~0x1f` |
| `0x1098` | `(base + 3 * field * 0x200 - 0x400) & ~0x1f \| 6` |
| `0x44c,0x450,0x454` | object `+0x6c,+0x70,+0x74` |

No write to `0x1070` occurs in this Linux initialization function; any earlier
tentative list containing it must not become part of the open-driver sequence.

Submission function `m9x1039` at `0x2d750` pads to the configured command
alignment with `0x45454545`, stores the final write address in tag-context
`+0x50`, and rings the engine by writing that address to MMIO `0x404` [BIN].
If `BM+0x34` is zero it first writes zero to `0x31c`.  With object flag `+0x80`
bit 1 set it waits for tag-context `+0x54` to clear, then polls `0x8f0` until
`value & 0x00030000` is zero.  Flag bit 2 adds callbacks around submission and
an idle-wait helper.  The returned fence is stored at object `+0x1a8`.

Requester wrap function `m9x1200` at `0x2b1c0` publishes and quiesces, writes
the two-dword record `{2, resource[0x44]}`, resets its software producer to CPU
base `resource[0x50]`, recomputes the end guard from size `resource[0x40]`, and
publishes and quiesces again [BIN].  Resource initialization stores
`resource[0x44] - resource[0x50]` as the CPU-to-GPU translation at `+0x58`, so
record opcode 2 is the requester wrap-to-GPU-base marker [DER].  It is outside
the indexed 3D packet grammar.

This strongly establishes `0x400` as DMA base/control and `0x404` as the submit
doorbell for this binary [DER].  The vendor's unbounded waits must become
bounded DRM waits with reset/error recovery; copying them literally would make
a malformed stream capable of hanging the kernel.

### Board enable and PCI bus mastering

The X-driver failure path in `m9x21829` calls `m9x28121` at `0x28fff0` with
`(board, 1, 0)`.  That wrapper calls the real lifecycle orchestrator `m9x30894`
at `0x294670`; it returns 0 on success, 7 on a false result, and 8 for a null
board [BIN].

The enable branch prepares the bridge through `m9x11035` at `0x300bb0`, updates
the board service at `+0xa0`, then performs the one direct hardware operation
which is important to a DRM port [BIN]:

```text
value = board->read_config(selector = 4);   /* vtable +0x68 */
board->write_config(selector = 4, value | 4); /* vtable +0x58 */
```

Selector/bit pair `4/4` is strong evidence for PCI command offset `0x04` and
`PCI_COMMAND_MASTER` bit 2 [DER].  A kernel M9 driver must enable PCI bus
mastering before requester initialization and submission.

The rest of `m9x30894` initializes the display manager, copies a 32-byte bridge
descriptor through `m9x2240`, enables GLServices and UnitServices, constructs
four 56-byte head/output objects for indices 0..3, and builds display-manager
objects.  Its disable branch tears those objects and services down in reverse
through `m9x14719` and the corresponding bridge/service callbacks [BIN].  It
does not contain a direct 3D-engine reset MMIO sequence.  Therefore the vendor
userspace lifecycle is useful for ordering and prerequisites, but should not be
copied wholesale into the kernel as an engine-arm function.

### Static reset audit and its limit

An audit of every Linux core call through the hardware write32 vtable slot
whose register offset is immediate found 168 calls covering 54 distinct MMIO
offsets [BIN].  They fall into the already decoded groups: front-end controls
`0x30c/0x314/0x31c`, service/context setup `0x330..0x3a8`, requester and
doorbell state `0x400..0x524`, interrupt banks `0x8d8..0x8fc`, address windows,
write-back fences, and the `0x1094..0x10ec` context registers.  There is no
separate immediate-offset engine-reset leaf in this set.  Dynamic-offset
writes, especially the interrupt-descriptor publisher `m9x115`, prevent this
count from proving that no undocumented register exists; the result is a
bounded statement about the complete literal-offset set in the shipped kernel
object.

Two reset-looking candidates can be excluded more strongly.  Linux
`m9x1172` writes zero to `0x30c`, changes only bits 17:16 of `0x3a8` to `1`,
then publishes a table of register/value/extent descriptors through `m9x115`.
It is constructed by the service initializer at `0x121d0`, not by requester
submission or timeout recovery [BIN].  The Windows sequence at `0x179a2`
temporarily masks `0x8c8/0x8dc`, toggles bits in `0x3c0`, waits for `0x3c4`
bit 31, delays 15 ms twice, and restores the masks.  Its diagnostic strings are
exactly `Test PLL lock`, `Program PLL`, and `Hardware init`; this is the PLL
lock/reprogramming procedure, not a 3D reset [BIN].

The open Linux core interface contains start/stop interrupt callbacks but no
reset or recovery callback, and neither the Linux requester timeout paths nor
the Windows memory-tag wait calls either candidate above [BIN].  The strongest
static conclusion is therefore that the saved stacks do not provide a callable
3D/global-reset contract.  This is not proof that the silicon lacks a reset
bit.  A new DRM backend can implement the decoded requester-local restart and
fail closed if it does not recover; it still needs a controlled capture or
hardware experiment before claiming a stronger engine reset.

### Interrupt-controller banks

The kernel object's interrupt reset function `m9x704` at `0x3f310` establishes
three status/mask pairs [BIN].  It writes zero to mask registers `0x8fc`,
`0x8e8`, and `0x8dc`, then writes `0xffffffff` to status registers `0x8f8`,
`0x8e4`, and `0x8d8`.  It also clears the cached mask at interrupt object
`+0x10 -> +0x8c`.  The paired source helpers prove that the status registers
are write-one-to-clear and the adjacent registers hold enables [DER]:

| status/W1C | enable mask | statically observed source bits |
|---:|---:|---|
| `0x8d8` | `0x8dc` | no individual source isolated in this pass |
| `0x8e4` | `0x8e8` | vblank `0x1`, `0x2`, `0x100`, `0x200`; also `0x4`, `0x8`, `0x10` |
| `0x8f8` | `0x8fc` | `0x10000`, `0x20000`, `0x40000`, `0x01000000` |

For example, `m9x444` at `0x3f500` enables low-bank source `0x100`: it reads
`0x8e8`, clears a pending event with `0x8e4 <- 0x100`, ORs `0x100` into the
mask, and writes the result to `0x8e8`.  Its paired disable/ack helper
`m9x1208` at `0x3f3c0` clears that mask bit first and then writes `0x100` to
`0x8e4`.  Functions `m9x550` at `0x3f6d0` and `m9x639` at `0x3f650` perform
the equivalent enable and disable/ack sequence for high-bank source
`0x20000` through `0x8f8/0x8fc` [BIN].

The complete list of one-source W1C helpers immediately before the reset
function is [BIN]:

| function | status register | source bit |
|---:|---:|---:|
| `m9x1061` `0x3f1b0` | `0x8f8` | `0x10000` |
| `m9x1158` `0x3f1d0` | `0x8f8` | `0x40000` |
| `m9x23` `0x3f1f0` | `0x8e4` | `0x10` |
| `m9x146` `0x3f210` | `0x8e4` | `0x4` |
| `m9x1007` `0x3f230` | `0x8e4` | `0x1` |
| `m9x755` `0x3f250` | `0x8f8` | `0x20000` |
| `m9x104` `0x3f270` | `0x8f8` | `0x01000000` |
| `m9x488` `0x3f290` | `0x8e4` | `0x200` |
| `m9x904` `0x3f2b0` | `0x8e4` | `0x100` |
| `m9x1223` `0x3f2d0` | `0x8e4` | `0x8` |
| `m9x780` `0x3f2f0` | `0x8e4` | `0x2` |

All four recovered high-bank sources also have complete enable and disable
pairs [BIN]:

| source | enable helper | disable helper | W1C helper |
|---:|---:|---:|---:|
| `0x10000` | `m9x61` `0x3f850` | `m9x1122` `0x3fb70` | `m9x1061` `0x3f1b0` |
| `0x20000` | `m9x550` `0x3f6d0` | `m9x639` `0x3f650` | `m9x755` `0x3f250` |
| `0x40000` | `m9x1038` `0x3f9b0` | `m9x794` `0x3f930` | `m9x1158` `0x3f1d0` |
| `0x01000000` | `m9x610` `0x3f750` | `m9x450` `0x3f7d0` | `m9x104` `0x3f270` |

Enable helpers clear pending status first, then write `cached_mask | bit` to
`0x8fc` and object cache `+0x8c`.  Disable helpers write
`cached_mask & ~bit` to both mask locations first, then acknowledge the status
bit.  Relocation and xref analysis finds no caller or function-pointer reference
to these exported leaves in the shipped kernel object or duplicated DDX code,
so none can yet be named DMA completion or fault without cross-family inference
[BIN].

Kernel functions `m9x664` at `0xc8a0` and `m9x161` at `0xc820` are anchored by
the literal diagnostics `EnableVSyncInterrupt` and `DisableVSyncInterrupt`
[BIN].  Their four-bit API mask maps to low-bank hardware sources exactly:

| API selector mask | `0x8e4/0x8e8` source | enable helper | disable helper |
|---:|---:|---:|---:|
| `0x1` | `0x1` | `m9x520` `0x3fa90` | `m9x239` `0x3fa30` |
| `0x2` | `0x2` | `m9x959` `0x3fc50` | `m9x258` `0x3fbf0` |
| `0x4` | `0x100` | `m9x444` `0x3f500` | `m9x1208` `0x3f3c0` |
| `0x8` | `0x200` | `m9x71` `0x3f480` | `m9x428` `0x3f420` |

This is enough to implement four vblank sources with masked dispatch and safe
acknowledgement.  The roles of low-bank `0x4`, `0x8`, `0x10`, all recovered
high-bank sources, and the third bank still require callback joins or
controlled source tests before assigning requester-completion, fault, or
hotplug names.

The open wrapper in `kernel/src/m9x_irq.c` confirms the intended interrupt
split [BIN].  Its shared-IRQ top half calls proprietary `irq_fast` after a
nonblocking device-mutex attempt; a negative result means `IRQ_NONE`, zero means
handled, and a positive result schedules the `irq_slow` tasklet.  The bottom
half obtains the mutex, calls `irq_slow`, or reschedules if the mutex is busy.
A DRM implementation can retain the recovered status/mask/ack ordering while
using current DRM vblank and scheduler primitives instead of this global
tasklet design.

Relocations in the core driver's operation table join those wrapper callbacks
to exact leaves [BIN]: `irq_fast` is `m9x782` at `0x5190`, which delegates to
`m9x1012`; `irq_slow` is `m9x293` at `0x5160`, which delegates to `m9x434`;
interrupt start is `m9x529` at `0x50e0`; and stop is `m9x1066` at `0x5100`.
Start only sets the underlying object's `+0x08` flag to 1.  Stop first calls
`m9x562` at `0xc810`, which directly calls controller reset `m9x704`, and then
clears that flag.  Thus source masks must be configured by the individual
enable helpers after start, while stop masks and acknowledges the controller
before declaring interrupt service inactive.  This is an IRQ lifecycle, not an
engine reset.

Low-level handler `m9x547` at `0x3fd90` always stores raw `0x8f4` to its first
output.  When raw bit 31 is set it reads `0x8e0` and `0x8e8`, stores
`low_active = status & enable` in its second output, and acknowledges that mask
at `0x8e4`.  Independently it computes
`high_active = raw_8f4 & cached_8fc` and acknowledges that at `0x8f8`.  It
returns 1 when either acknowledgement occurred [BIN].

Core fast handler `m9x1012` at `0xdec0` nevertheless rejects the IRQ unless raw
`0x8f4` bit 31 is set.  With bit 31 set, it translates only low active sources
`0x1,0x2,0x100,0x200` into pending bits 0..3 for slow work.  Consequently a
high-bank-only IRQ can be acknowledged by `m9x547` and still return
`-1`/`IRQ_NONE`; there is no hidden high-source completion/fault dispatch in the
saved Linux stack [BIN].  Older Parhelia evidence cannot assign meanings to
these M9 sources; the Windows cross-check below further separates requester
completion from this interrupt dispatch.

### Windows miniport interrupt cross-check

The MSerX `MxMM.sys` miniport ISR at `0x10875` uses the same two main banks
[BIN].  It first takes an interlocked per-device guard, reads low enable
`0x8e8`, then reads raw high/cascade status `0x8f4` and the driver's cached high
enable at object `+0x8c`.  It rejects `0x8f4 == 0xffffffff`.  If high bit 31 is
set, it reads low pending `0x8e0` and services only sources also present in
`0x8e8`, acknowledging each through `0x8e4`.  This independently confirms that
bit 31 is the low-bank cascade rather than an ordinary high-bank event.

The Windows routine exposes more source-routing machinery than the Linux
wrapper, but not enough names to classify requester faults.  In particular:

| source | statically observed Windows ISR action |
|---:|---|
| high `0x40000` | W1C at `0x8f8`, increment record counter, store event code 9 |
| high `0x10000` | W1C at `0x8f8`, increment record counter, store event code 10 |
| high `0x20000` | W1C at `0x8f8`, increment record counter, store event code 11 |
| high `0x8` | W1C at `0x8f8`, invoke an optional registered callback once |
| high `0x4` | W1C at `0x8f8` |
| high `0x01000000` | route an event record through guarded callback/work logic |
| low `0x1`, `0x2`, `0x4`, `0x8` | W1C at `0x8e4`, update per-source notification records |
| low `0x100`, `0x200`, `0x10` | W1C at `0x8e4`, route notification indices 4, 6, and 8 |

The miniport's logical interrupt selector is now fully joined to these two
banks [BIN].  Function `0x19b4d` reads low enable `0x8e8` and cached high
enable at device `+0x8c`, then tests one source selected by an integer from
zero through 12.  Dispatcher `0x19bee` uses the same selector to call the
matching enable or disable/ack helper:

| logical selector | bank | source bit | enable helper | disable/ack helper |
|---:|---:|---:|---:|---:|
| 0 | low | `0x1` | `0x192ca` | `0x19317` |
| 1 | low | `0x4` | `0x19358` | `0x193a5` |
| 2 | low | `0x2` | `0x193e6` | `0x19433` |
| 3 | low | `0x8` | `0x19474` | `0x194c1` |
| 4 | low | `0x100` | `0x19502` | `0x19551` |
| 5 | - | unsupported | - | - |
| 6 | low | `0x200` | `0x19598` | `0x195e7` |
| 7 | - | unsupported | - | - |
| 8 | low | `0x10` | `0x1908c` | `0x190d9` |
| 9 | high | `0x40000` | `0x1911a` | `0x19169` |
| 10 | high | `0x10000` | `0x191aa` | `0x191f9` |
| 11 | high | `0x20000` | `0x1923a` | `0x19289` |
| 12 | high | `0x01000000` | `0x1962e` | `0x1967d` |

This ordering independently explains the event codes written by ISR
`0x10875`: high sources `0x40000`, `0x10000`, and `0x20000` become selectors
9, 10, and 11, while low sources `0x100`, `0x200`, and `0x10` become 4, 6,
and 8.  High source `0x01000000` becomes selector 12.  It also prevents an
easy implementation error: logical selectors 1 and 2 do not follow increasing
hardware bit order.

Function `0x19d5c` builds a notification subscription from a 12-bit logical
mask: mask bit *n* appends selector *n* for selectors 0 through 11.  It can
therefore include the unsupported selector holes 5 and 7, but it cannot create
selector 12; that guarded high-bank service is registered separately.  Record
helper `0x19e59` clears the global pending word at `+0x9c`, increments the
selected 12-byte record's counter at `+4`, and stores its selector at `+0`.
Reset helper `0x19e72` clears 13 such records and sets the global pending word
to one.  These joins recover the routing ABI and record layout.  They still do
not provide semantic fault names, so a new DRM driver should expose only the
four proven vblank sources initially and keep the other sources masked.

High source `0x8` has one exact service-level join [BIN].  Miniport interface
method `0x1b374` builds and publishes a rectangle-transfer command, installs
its optional completion callback and argument at device `+0x1ac4/+0x1ac8`,
and clears completion word `+0x1acc`.  If the caller did not supply a callback,
the method waits for that word after publishing through `0x146bc`; otherwise it
returns asynchronously.  ISR `0x10875` acknowledges raw high bit `0x8`, invokes
the installed callback once, and then sets `+0x1acc` to one.  Thus `0x8` is the
completion/service interrupt for this special miniport transfer method.  It is
not the generic 3D requester fence, whose Linux and Windows paths both wait on
a GPU-written memory tag.  No static join establishes whether the transfer
source is safe or useful for a DRM render scheduler, so it should remain masked
outside an implementation of that specific transfer service.

The ISR also acknowledges enabled low sources `0x400`, `0x800`, `0x1000`,
`0x2000`, `0x4000`, `0x8000` and their corresponding high-word bits, with an
auxiliary dispatcher handling further source groups.  Those observations prove
source existence and W1C behavior only.  Event numbers 9--11 are internal
notification selectors, not semantic names, and no ISR path is joined to the
requester's memory-tag completion.

Windows interrupt-controller shutdown at `0x10f22` writes zero to `0x8fc`, the
cached high mask, and `0x8e8`, then acknowledges `0x032f7f1f` through `0x8f8`
and `0xff07ffff` through `0x8e4` [BIN].  Linux `m9x704` uses all ones for its
reset acknowledgement instead.  Both establish mask-before-ack ordering.  A
new DRM handler should acknowledge only captured active sources during normal
shared-IRQ service and leave every unnamed source masked until a controlled
test associates it with a hardware event.

This Windows join also removes the earlier completion-IRQ hypothesis.  The
miniport requester functions at `0x224c8` and `0x224f3` program `0x400` and
`0x404`; synchronous helper `0x22533` emits the exact three-dword record
`{0x4545e3e6, 3, target | 1}`, waits on a GPU-written memory dword, and
repeatedly kicks `0x314`.  Its header means `e6 <- 3`, `e3 <- target | 1`, then
two `45` terminators.  Linux `m9x604` independently emits the same reduced
record at `0x309f..0x30bd`.  Fence completion is therefore a memory write-back
in both saved operating-system stacks.  The high-bank sources may still include
fault or service notifications, but none is required to retire a requester
fence and none has a defensible fault name yet [BIN].

## Native shader upload transport

Function `0xf74d0` gives an exact inline program upload [BIN]:

1. A program resource descriptor lives at state offsets `+0xa0/+0xa4`.  The
   command-buffer relocation callback is invoked with usage/type `0x19`.
2. Index `69` receives the relocated address, `6a` receives state `+0xa8`, and
   `6d` receives state `+0xbc`.
3. For an inline program, `+0xbb0` is the byte length and `+0xbb4` is the CPU
   pointer.  Index `e3` receives a command-buffer allocation/result value.
4. The driver computes `upload_line_count = byte_length >> 5`.  It reserves
   `15 * upload_line_count + 10` dwords in the command buffer.
5. For each upload line, index `68` receives a selector.  It starts at
   `0x80000000`; selector bits `13:5` advance by `0x20` for each 32-byte line.
6. Indices `60,61,62,63` receive record dwords 0 through 3.  Indices
   `64,65,66,67` receive record dwords 4 through 7.  The source pointer advances
   by exactly 32 bytes.

Equivalent pseudocode is:

```c
for (unsigned byte_off = 0; byte_off < byte_length; byte_off += 32) {
        emit(0x68, 0x80000000 | (byte_off & 0x3fe0));
        emit4(0x60, 0x61, 0x62, 0x63, code[0], code[1], code[2], code[3]);
        emit4(0x64, 0x65, 0x66, 0x67, code[4], code[5], code[6], code[7]);
        code += 8;
}
```

This proves that `68` is the indexed-store selector and that ports `60..67`
write one 32-byte store line [DER].  It does not set the native instruction
width: the fixed-fragment database below proves that each line contains four
8-byte/two-dword native instructions.

Linux function `m9x11728` at `0x4b9340` independently implements the same
transport [BIN].  In that build the native byte length and CPU pointer are at
state `+0xbd8/+0xbe0`.  It again computes `length >> 5`, reserves `15*N+10`
dwords, emits selector `68` followed by ports `60..67`, and advances the source
by `0x20`.  Its selector update preserves bits selected by `0xc01f` and inserts
`(instruction_number & 0x1ff) << 5`, independently fixing the selector's
instruction-number field at bits `13:5` [DER].

The Linux external resource form binds the fixed-fragment database at index
`69` from resource
`+0xa0/+0xa8` with usage `0x19` and descriptor `2`, writes `6a <- +0xb0`, and
`6d <- +0xd0`.  It also exposes a short control form selected by state
`+0xbcc` [BIN].  The old address-directed BAR snapshot at the emitted address
is an opaque alias, not a linear view of that database; its translation remains
unresolved [CAP/DER]:

```c
if (state_bcc) {
        emit(0x6d, low16(state_bd4));
        emit(0x69, 4);
        emit(0x6a, low16(state_bd0));
} else {
        emit(0x69, 4);
        emit(0x6a, low16(state_bd0) | (low16(state_bd4) << 16));
}
```

Windows `0xf7819..0xf78aa` is independently identical with offsets shifted to
`+0xba4/+0xba8/+0xbac` [BIN].  When `+0xba4` is zero it packs the two low-16-bit
starts into index `6a`; otherwise it writes the second start alone to `6d` and
the first start alone to `6a`.  Both forms write literal `4` to index `69`.
Companion selector `0x13b570` sets `+0xba4` when it installs its override at
state `+0xbc` (`0x13b656..0x13b67c`) and clears it when no override is active
(`0x13b7c1`).  Thus the split form is exactly the companion-entry form in this
fixed vertex/declaration-conversion path [BIN][DER].

No execution count or end address occurs in either final record.  The aligned
byte length is consumed only by the upload loop and includes zero padding; it
is not copied into `69`, `6a`, or `6d`.  The final record therefore binds
entry instruction slots rather than start/end ranges [BIN][DER].

The same function can bind a relocated program resource rather than uploading
inline words.  Registry names `PsBmMicrocode`, `PsBmConstants`,
`CeBmMicrocode`, and `CeBmConstants` select bus-master paths independently for
two program classes [BIN].  The registry reader at `0xba400` stores them at
configuration offsets `+0x13c`, `+0x140`, `+0x144`, and `+0x148`, respectively,
with default zero [BIN].  `Ps` and `Ce` are exact vendor labels; static data flow
does not yet prove which auxiliary selector or sparse-record half belongs to
each label.  This explains why native code may be absent from an ordinary
MMIO-only trace.  Capture work must exercise both inline and BM modes.

### Program linking and limits

`MxMD.dll` function `0x13bf80` links microcode fragments from two source lists
into the buffer later consumed at state `+0xbb0/+0xbb4` [BIN].  It:

* accumulates fragment byte counts;
* moves to a second source/bank when adding a fragment reaches `0xc00` bytes;
* rounds the final allocation up with `(size + 0x1f) & ~0x1f`;
* copies both lists into the allocation;
* stores the two selected fragment-chain starting instruction offsets
  (`cumulative_byte_offset >> 3`) at `+0xba8/+0xbac`;
* stores the aligned length at `+0xbb0` and pointer at `+0xbb4`.

The `0xc00` boundary is 3,072 bytes, or 384 native 8-byte instructions and 96
32-byte upload lines [DER].  It is a bank/assembly limit visible in this path,
not yet proof that every shader stage exposes 384 user instructions.
The two offsets are entry points for the selected composable chains in this
fixed-program assembly path.  They do not by themselves identify vertex and
fragment shader stages.

The exact run-boundary contract is consequently:

* `+0xba8/+0xbd0` and `+0xbac/+0xbd4` are instruction-slot starts, truncated
  to 16 bits by the emitter;
* the code image is uploaded in complete 32-byte lines, and its stored byte
  length is transport state only;
* companion inactive: the two starts occupy `6a[15:0]` and `6a[31:16]`;
* companion active: the primary start occupies `6a[15:0]`, the companion
  start occupies `6d[15:0]`, and the unused high halves are zero;
* `69 <- 4` completes the inline-program bind; its finer semantic name is not
  established by static evidence;
* execution begins later with the ordinary H03/H06/H07 draw record after the
  rest of context state is emitted.  The program upload/bind itself is not a
  draw launch.

This is sufficient to implement a typed inline upload and entry bind without
inventing a count register.  It is not sufficient to submit a new minimal
programmable vertex-plus-fragment pair: the fixed image here belongs to the
vertex/declaration-conversion path, and static data flow has not joined a
programmable fragment image to one of these starts.  The exact native stop
operation also remains unnamed.

### Fixed native-fragment database

`MxMD.dll` function `0x1454a0` builds a complete table of 57 fixed-fragment
lists [BIN].  Slots `0x00..0x38` have stride `0x10`; helper
`0xadf00` appends `{read_only_data_pointer, byte_length}` descriptors to each
slot.  Function `0x120910` walks the whole table at offsets `+0x000..+0x380`
(end `+0x390`) and concatenates every descriptor payload verbatim into a
fixed-function vertex-program resource.  The static payloads span VA
`0x1e91f8..0x1ef780`.
Linux 1.15 exports the equivalent initializer as `m9x30489` at `0x4462d0`,
with its mirrored read-only data beginning near `0xea40e0` [BIN].

Every fragment length is divisible by eight, and the database includes a
standalone eight-byte fragment at VA `0x1ecdd8` [BIN]:

```text
dword 0 = 0xfffc0fff
dword 1 = 0x8bffffc0
```

Thus a native instruction is exactly 8 bytes/two dwords.  Linker `0x13bf80`
concatenates selected fragments and pads only the final total to the 32-byte
store-line boundary; that alignment is transport granularity, not instruction
width [DER].

A second independent pair of functions constructs a short program dynamically:
Windows `0x147d30` and Linux `m9x25489` at `0x445ea0` [BIN].  It sets a count of
four and emits these first two native instructions followed by two zero slots:

```text
{ 0xffc0291d, 0xe29fff1f }
{ 0xffc1291e, 0xa29fff1f }
{ 0x00000000, 0x00000000 }
{ 0x00000000, 0x00000000 }
```

The builder proves that dword 0 low five bits form an independently selected
opcode-like field (`0x1d`, then `0x1e`), dword 0 bit 16 can be toggled without
changing that field, and dword 1 high nibble can independently select `0xe` or
`0xa` [DER].  Operand and control meanings still need cross-fragment
differencing; these fields should retain neutral names until that join is made.

The complete 57-slot manifest, D3D vertex-declaration selector, fixed-blob hash,
conditional prefixes, and dynamic-builder masks are recorded in
`parhelia-m9-native-shader-fragments.md`.

Several address fields preserve bits `31:29` while replacing bits `28:0`, using
the masks `0xe0000000` and `0x1fffffff` [BIN].  The upper three bits therefore
carry address-space or resource flags; relocations must preserve them.

### Auxiliary upload records

Windows function `0xf78c0` uses selectors in the `0x1300` region.  Linux twin
`m9x2427` at `0x4b99d0` resolves its apparent half-record loop into four
complete 32-byte records [BIN][DER]:

| selector | eight state dwords in port order `60..67` |
|---:|---|
| `0x80001300` | `+0x7d4,+0x7d8,+0x7dc,+0x858,+0x7e4,+0x7e8,+0x7ec,+0x854` |
| `0x80001320` | `+0x7f4..+0x810` |
| `0x80001340` | `+0x814..+0x830` |
| `0x80001360` | `+0x834..+0x850` |

The surrounding resource-bind packet writes `68 <- 0`, `6b <- +0xc8`, and
relocates resource `+0xb8/+0xc0` into `6c` with usage `0x1a`, descriptor `4`.
Dirty bit `0x08` controls the `1320/1340/1360` group.

The Windows producer fixes their stage assignment and its differing state
layout.  Vertex/declaration-conversion wrapper `0x13c2c0` calls state builders
`0x13b7e0`, `0x13b9b0`, `0x13bb80`, and `0x13bda0` before the same fixed
fragment linker.  `0x13b7e0` copies device `+0xac0..+0xadc` to pass
`+0x7c0..+0x7dc`; selector `1300` emits pass
`+0x7c0,+0x7c4,+0x7c8,+0x844,+0x7d0,+0x7d4,+0x7d8,+0x840` in port order.
`0x13b9b0` copies 24 dwords from device `+0xb44` to pass `+0x7e0`, and
selectors `1320/1340/1360` emit the three consecutive eight-dword pieces
`+0x7e0..+0x83c` [BIN][DER].

Windows function `0xf7c60` and Linux twin `m9x14617` at `0x4b8fa0` use selector
`0x80001500` and all eight data ports.  The Linux state map is [BIN]:

| port | source |
|---:|---|
| `60` | `+0x884` |
| `61` | truncated float `+0x85c` in low 16 bits, truncated float `+0x860` in high 16 bits |
| `62` | relocation `+0x888/+0x890`, usage `0x15`, descriptor `5` |
| `63` | same resource, usage `0x16`, descriptor `2` |
| `64` | `+0x878` |
| `65` | `+0x87c` |
| `66` | relocation `+0x868/+0x870`, usage `0x13`, descriptor `5` |
| `67` | same resource, usage `0x14`, descriptor `2` |

Builder `0x13bda0` produces the `1500` state in the same fixed-function
vertex/declaration-conversion path.  On Windows, `f7c60` relocates resources
`+0x868/+0x86c` into ports `62/63` with usage `0x15/0x16` and resources
`+0x850/+0x854` into ports `66/67` with usage `0x13/0x14`; the other ports are
dimensions and controls.  The five selectors are therefore auxiliary
fixed-function vertex-conversion records, and `1500` is specifically a
resource/control record.  They are not generic vertex or fragment constant
banks [BIN][DER].

## Matrox GLSL compiler interface

The `MxMGLX.dll` compiler object's vtable is at `0x10804014`.  Constructors
`0x1066e4a0` and `0x1066e640` install it.  Relevant slots are [BIN]:

| slot | function | result |
|---:|---:|---|
| `+0x14` | `0x1066e7a0` | main compile pipeline |
| `+0x1c` | `0x1066f500` | return output pointer and byte count |
| `+0x20` | `0x1066f540` | metadata at object `+0x484` |
| `+0x24` | `0x1066f550` | metadata at object `+0x4a4` |
| `+0x28` | `0x1066f560` | metadata at object `+0x494` |
| `+0x2c` | `0x1066f570` | metadata at object `+0x4b4` |
| `+0x30` | `0x1066f580` | metadata at object `+0x4c4` |

The output getter returns the vector beginning at object `+0x14`; its byte count
is the dword-vector length multiplied by four.  With a debug flag enabled,
function `0x10672c00` writes that exact vector to `iris.o` [BIN].  The compile
pipeline calls translation, allocation/linkage, and serialization passes at
`0x106717e0`, `0x10671930`, `0x10671b10`, and gated serializer `0x106726a0`
before the dump.

The Iris stream is variable length and contains metadata/sentinels as well as
instructions.  It is not the fixed two-dword native instruction stream copied
by `MxMD.dll`.  A Mesa compiler can reuse the recovered operation semantics,
but must still implement a separate Iris/native lowering.  The physical native
bit packer is now decoded in `parhelia-m9-native-shader-fragments.md`; its
five source/resource descriptors and one result descriptor are distinguished.
Native operation mnemonics remain unjoined.

Compiler built-in generator `0x10689080` gives the exact 48-byte runtime-limit
structure consumed when it creates the GLSL preamble [BIN]:

| structure offset | generated GLSL constant |
|---:|---|
| `+0x00` | `gl_MaxLights` |
| `+0x04` | `gl_MaxClipPlanes` |
| `+0x08` | `gl_MaxTextureUnits` |
| `+0x0c` | `gl_MaxTextureCoords` |
| `+0x10` | `gl_MaxVertexAttribs` |
| `+0x14` | `gl_MaxVertexUniformComponents` |
| `+0x18` | `gl_MaxVaryingFloats` |
| `+0x1c` | `gl_MaxVertexTextureImageUnits` |
| `+0x20` | `gl_MaxCombinedTextureImageUnits` |
| `+0x24` | `gl_MaxTextureImageUnits` |
| `+0x28` | `gl_MaxFragmentUniformComponents` |
| `+0x2c` | `gl_MaxDrawBuffers` |

Wrapper `0x10669a90` passes this structure to the generator when a live limits
object is supplied and selects a separate default-preamble builder at
`0x106820d0` when it is null [BIN].  The live compiler caller at
`0x105658a0`, immediately before its call to `0x10669ee0` at `0x10565925`,
constructs that structure with these exact advertised values [BIN][DER]:

| GLSL limit | value |
|---|---:|
| `gl_MaxLights` | 8 |
| `gl_MaxClipPlanes` | 6 |
| `gl_MaxTextureUnits` | 4 |
| `gl_MaxTextureCoords` | 8 |
| `gl_MaxVertexAttribs` | 16 |
| `gl_MaxVertexUniformComponents` | 512 |
| `gl_MaxVaryingFloats` | 60 |
| `gl_MaxVertexTextureImageUnits` | 4 |
| `gl_MaxCombinedTextureImageUnits` | 20 |
| `gl_MaxTextureImageUnits` | 16 |
| `gl_MaxFragmentUniformComponents` | 512 |
| `gl_MaxDrawBuffers` | 4 |

These are the vendor GLSL frontend limits for this M9 build and are suitable
upper bounds for the Mesa implementation.  They agree with the independently
recovered four vertex-sampler and sixteen fragment-sampler linker checks.  The
four-draw-buffer value also identifies the four attachment pairs `b0..b7` as
the hardware-visible color target capacity; private render-state IDs
`215..218` extend the API shadow to masks 4..7 but do not raise the advertised
active draw-buffer count.

### Serialized Iris object

The serializer gate is compiler `+0x460` bit 2, tested at `0x1066f33d`; the
constructor at `0x1066e4a0` initializes that field to `4`, so normal compilation
calls serializer `0x106726a0` [BIN].  Its header is [DER]:

```c
header = 0xfffc0100 |
         ((~(*(uint32_t *)(compiler + 0x478) << 14)) & 0x10000);
```

Header bit 16 is therefore the inverse of compiler `+0x478` bit 2.  Primary
records are emitted in the following fixed list order while the corresponding
list is nonempty [BIN]:

| list below `compiler+0x464` | tag | payload |
|---:|---:|---|
| `+0x40` | `0x002f` | six dwords/24 bytes from node `+0x10` |
| `+0x198` | `0x8008` | six dwords/24 bytes from node `+0x18` |
| `+0x220` | `0x8009` | six dwords/24 bytes from node `+0x18` |
| `+0x2a8` | `0x800a` | six dwords/24 bytes from node `+0x18` |
| `+0x110` | `0x001d` | six dwords/24 bytes from node `+0x18` |

Each primary record is one tag dword plus six payload dwords: 28 bytes, not a
native 8-byte instruction.  For tag `0x2f`, node `+0x14` bit 1 controls a
separate descriptor canonicalization.  When clear, the derived descriptor
field is [DER]:

```c
n = (payload_dword1 >> 7) & 7;
m = ((1u << n) - 1) & 0xf;
descriptor_dword2 = (old_descriptor_dword2 & 0x07803fff) |
                    (m << 14) | 0x07800010;
```

After the primary lists, the serializer appends another variable dword vector
and `0xffff`, then metadata from `0x106731c0`.  That metadata begins with
`0xfff9`, contains variable `0xfffa` symbol records with counts and 60-byte
subrecords, and ends with a second `0xffff` [BIN].  Getter `0x1066f500` returns
this dword vector and its exact byte length; debug writer `0x10672c00` opens
literal filename `iris.o` in `wb` mode.

Functions `0x105686e0` and `0x10569370` operate after program installation but
do not perform native packing [BIN].  The first walks `program_env_%d`,
`program_local_%d`, and `__constant__` symbols and maintains 20-byte/16-byte
binding tables plus 40-byte symbol nodes.  The second allocates `count*0x18`
and materializes runtime matrix/vector parameters.  Neither uses a 32-byte
stride.  The `GL_PROGRAM_FORMAT_ASCII_ARB` path at `0x10568220` feeds its
translated program back through compiler frontend `0x105658a0`.

No Iris header or tag constants occur in `MxMD.dll`, and `MxMGLX.dll` does not
import `MxMD.dll` [BIN].  The final Iris-to-native lowering therefore lies
behind another interface or code path; the traced serializer cannot be treated
as the hardware ISA packer.

The per-instruction representation is nevertheless shared.  MxMD converter
`0x1a3e00` reads the same token offsets formatted below: token `+0x08` bits
0/1 become conditional and predicate-enable flags, `+0x04` supplies the D3D
opcode, `+0x44` is the result, `+0x7c+0x38*n` are four sources, `+0x0c` is the
predicate descriptor, and each source's trailing auxiliary pair is retained
[BIN][DER].  Thus the opaque boundary is the container/entry interface, not
the instruction layout or its descriptor vocabulary.

## Iris instruction token layout

The trace formatter at `0x106ae3b0` provides these token facts [BIN]:

* the opcode is the low 16 bits of the dword at token offset `+4`;
* bits `27:24` of that dword encode instruction modifiers:
  `0` none, `1` `_sat`, `2` `_pp`, `4` `_centroid`;
* bits `31:28` encode result scale:
  `0` none, `1` `_x2`, `2` `_x4`, `3` `_x8`, `13` `_d8`, `14` `_d4`,
  `15` `_d2`;
* the operand formatter sees component masks/swizzles, source negation and
  absolute forms, immediates, array/range indices, and predicate state;
* predicate names include `p0` through `p5` and negated forms;
* control operations include `PushConditionalStack`,
  `MaskConditionalStack`, and `PopConditionalStack`.

The instruction-prefix formatter at `0x106ae220` further identifies token
offset `+0x8` bit 0 as the printed conditional marker and bit 1 as predicate
enable.  Predicate-enabled instructions take the predicate number from token
`+0x18`, a polarity/modifier nibble from `+0x16c`, component enables from bits
`16:13` of `+0x44`, and four packed two-bit component selectors from bits
`24:17` of `+0xc` [BIN].  Modifier values `0`, `1`, and `13` print as no prefix,
`-`, and `!`, respectively.  The resulting debug form is a predicate register
plus an `x/y/z/w` mask.  This layout is directly usable by an Iris dump parser,
while its eventual native encoding remains separate.

The operand formatter at `0x106afa90` and its caller at `0x106af220` expose a
substantial part of the Iris operand descriptor [BIN].  Operand `n` begins at
token `+0x7c + 0x38*n`; a related per-operand nibble is stored at token
`+0x15c + 4*n`.  In the descriptor's first dword:

| bits | compiler-IR meaning |
|---:|---|
| `4:0` | register-file code; known printed files include `r`, `v`, `c`, `a`, `t`, `s`, `cb`, `aL`, `l`, `p`, and special face registers |
| `10:6` | operand/addressing form; formatter has specialized paths for values `3`, `7`, and `15` |
| `12:11` | `x/y/z/w` component used by relative-address syntax |
| `16:13` | four component-enable bits, from `x` at bit 13 through `w` at bit 16 |
| `24:17` | four packed two-bit `x/y/z/w` selectors |
| `27:25` | type class: `1` scalar, `2` vector, `3` matrix, `4` struct, `5` array, `6` label |
| `30:28` | array element type when the outer class is array, using the same scalar through label numbering |

Descriptor dword `+0x4` bit 0 selects the formatter's immediate marker and bit
1 selects its `$` marker.  Fields at `+0x8/+0xc`, `+0x14/+0x18`, and
`+0x20/+0x24` supply scalar or range bounds in the printed addressing forms
[BIN].  These are compiler-container fields, not native M9 instruction bits,
but they are enough to build a compatible Iris parser and differential dumper.

The instruction vocabulary recovered from the formatter is:

| opcode | Iris operation | opcode | Iris operation |
|---:|---|---:|---|
| `0` | NOP | `1` | MOV |
| `2` | ADD | `3` | SUB |
| `4` | MAD | `5` | MUL |
| `6` | RCP | `7` | RSQ |
| `8` | DP3 | `9` | DP4 |
| `10` | MIN | `11` | MAX |
| `12` | EXP | `13` | LOG |
| `16` | LRP | `17` | FRC |
| `18` | M4x4 | `19` | M4x3 |
| `20` | M3x4 | `21` | M3x3 |
| `22` | M3x2 | `23` | CALL |
| `30` | POW | `31` | SGN |
| `32` | ABS | `33` | NRM |
| `34` | SINCOS | `41` | BREAK family |
| `45` | KILL | `46` | TEXLD |
| `48` | CMP | `49` | DSX |
| `50` | DSY | `51` | TEXLDD |
| `52` | SETP | `53` | TEXLDL |
| `54` | TEXLDB | `55` | TEXLDP |
| `0x101` | FTOI | `0x102` | FTOU |
| `0x103` | IADD | `0x104` | integer comparisons |
| `0x106` | IMAD | `0x107` | IMAX |
| `0x108` | IMIN | `0x109` | IMUL |
| `0x10a` | INEG | `0x10b` | ISHL |
| `0x10c` | ISHR | `0x10d` | ITOF |
| `0x10e` | NOT | `0x10f` | OR |
| `0x110` | UDIV | `0x111` | UMOD |
| `0x112` | unsigned comparisons | `0x114` | UMUL |
| `0x115` | UMAD | `0x116` | UMAX |
| `0x117` | UMIN | `0x118` | USHR |
| `0x119` | UTOF | `0x11a` | XOR |
| `0x11b` | ICMP | `0x11d` | SIN |
| `0x11e` | COS | `0x8001` | CEIL |
| `0x8002` | TRUNC | `0x8003` | FLOOR |
| `0x8004` | DP2 | `0x8005` | M2x2 |

The comparison families represented at `0x104`, `0x112`, and `0x11b` retain
the condition names `GT`, `EQ`, `GE`, `LT`, `NE`, and `LE`.  Their signed forms
print as `ISGT/ISEQ/ISGE/ISLT/ISNE/ISLE`, unsigned forms as
`USGT/USEQ/USGE/USLT/USNE/USLE`, and conditional-select forms as `ICMP GT`
through `ICMP LE` [BIN].

Base opcodes `14..15`, `24..29`, `35..40`, `42..44`, and `47` take the
formatter's unsupported/reserved path.  The extended integer vocabulary is
compiler-front-end evidence.  It does not prove that M9 executes those
operations natively; GLSL 1.10 lowering can expand them into simpler native
records.

The ARB assembly parser separately recognizes `KIL`, `TXB`, `TXP`, `TEX`,
`SWZ`, `MAD`, `LRP`, `CMP`, `XPD`, `SUB`, `SLT`, `SGE`, `MUL`, `MIN`, `MAX`,
`DST`, `DPH`, `DP4`, `DP3`, `ADD`, `POW`, `SCS`, `SIN`, `RSQ`, `RCP`, `LG2`,
`EX2`, `COS`, `MOV`, `LIT`, `FRC`, `FLR`, and `ABS` [BIN].

## Linker resource limits

The GLSL linker/resource allocator at `0x1055d710` applies these exact checks
[BIN]:

| stage/resource | limit |
|---|---:|
| vertex samplers | 4 |
| vertex attributes | 15 |
| vertex output registers | 15 |
| fragment samplers | 16 |
| fragment input registers | 16 |
| vertex allocation metric | `0x7c00` |
| fragment allocation metric | `0x7c00` |

The two `0x7c00` comparisons are internal allocation offsets/metrics.  They
must not be exposed as 31,744 constants.  The ICD has built-ins for
`gl_MultiTexCoord0..7`; those eight input names do not override the separate
four-vertex-sampler check.

Linux function `m9x20565` at `0x6bbb40` repeats the same checks and provides
diagnostic strings that identify the first three vertex fields as samplers,
attributes, and output registers, and the fragment fields as samplers and
input registers [BIN].  It calls the `0x7c00` fields constant-allocation units,
but their hardware granularity remains unknown.  The independent 64-bit build
therefore corroborates both the caps and the warning against treating
`0x7c00` as a public vector count.

### Vertex-to-fragment varying linkage

The GLSL stage linker at `MxMGLX.dll` `0x1055db40` supplies the software side
of the vertex/fragment interface [BIN][DER].  It initializes fourteen dword
entries at linker `+0x5bbc..+0x5bf0` to `0xffffffff` and resets a high-water
value at `+0x5bf4`.  With both stage-presence bytes `+0x61ac/+0x61ad` set, it
walks every fragment varying and searches the vertex outputs.  A match requires
the exact symbol name, identical type field, and identical size.  Failure emits
the corresponding saved diagnostics:

```text
Varying "%s" is used in fragment shader, but not written in vertex shader.
Varying "%s" is is defined with different types in the vertex and fragment shaders.
Varying "%s" is is defined with different size in the vertex and fragment shaders.
```

The linker rejects an assigned end greater than `14` with `Not enough resources
to allocate varyings.`  For each component/register element of a successful
match it writes the translated slot number into the `+0x5bbc` table, then
raises `+0x5bf4` to the largest occupied end.  This is a directly usable Mesa
link rule: fragment inputs must be matched by name/type/size and allocated
through an explicit remap.  It does not reveal how those logical slots are
encoded in the final seven-bit selector, but the next static join is now known.
The table and high-water mark are passed through `0x1055deb0` during paired
program installation.  MxMD's `o` converters `0x1a34e0/0x1a3760` then scan a
16-byte-stride link table at compiler context `+0x34..+0x38`, match the logical
index against entry `+0x04`, and replace it with entry `+0x00` before native
descriptor encoding.  The `v` input path uses the same record form at context
`+0x24..+0x28` [BIN][DER].  This proves that logical varying order is routed
through an explicit physical-index table before packing.

The 16-fragment-sampler compiler check is a logical program-resource limit.  It
does not prove 16 physical texture pipes and does not contradict the four live
`70..73` texture-state blocks recovered from `0xf4350`.  The subsequent
five-dependent-sampler capture proves time-serialized compiler scheduling:
five distinct resources produce the exact result through five texture/raster
blocks in one DMA submission.  Report the logical compiler cap, implemented
scheduling limit, and four-slot physical throughput separately.

The `GL_MTX_fragment_shader` validator at `0x1012c180` also proves a legacy
fragment path with two special temporaries, texture dependency rules, paired
color/alpha requirements for DP3/DP4, result-only saturate/scale modifiers,
argument-only replicate/convert modifiers, unique invariant accounting, and a
mandatory final output write [BIN].  That validator describes the extension
language.  Its validation rules should be used as frontend constraints, not
copied blindly into native ISA validation.

## Native ISA facts and remaining unknown fields

Static analysis has established these native facts:

* instruction: two dwords/eight bytes [BIN][DER];
* upload-store line: eight dwords/32 bytes, carrying four instructions [BIN];
* inline instruction-store selector: index `68`, enable bit 31, address in bits
  `13:5` [BIN][DER];
* data ports: indices `60..67`, one dword each [BIN];
* linked bank boundary: `0xc00` bytes, 384 instructions or 96 upload lines
  [BIN][DER];
* program resources can be uploaded inline or fetched by bus mastering [BIN];
* GPU resource addresses preserve three flag bits above a 29-bit address [BIN];
* selectors `0x1300,0x1320,0x1340,0x1360,0x1500` address complete auxiliary
  eight-dword records in the fixed-function vertex/declaration-conversion
  path; `1320..1360` partition one 24-dword table and `1500` contains two
  relocation-backed resource pairs plus controls [BIN][DER];
* index `44` is an eight-write record port in two relocation-backed paths
  [BIN][DER];
* `2b,2a,e6` carries 2D rectangle/stretch and mask/pixel inline payloads, with
  count-minus-one in the low bits of the `e6` value [BIN][DER];
* the serialized Iris object uses 28-byte primary records and variable metadata,
  conclusively separating it from native 8-byte instructions [BIN][DER].
* native packer `0x1db840..0x1db9bb` places two five-bit operation selectors in
  `W0[4:0]` and `W1[25:21]`, with a format-dependent third selector in
  `W1[18:14]`; it also fixes the gated seven-bit descriptor fields, split
  selector, class-`0xb` bank remap, and two scattered component-mask maps
  [BIN][DER].
* whole-program emitter `0x1db510` tests source-zero auxiliary fields in the
  current instruction IR and can replace it with a complementary `0x1d/0x1e`
  result pair, or prepend two identical synthetic slots and set output property
  `+0x397` bit `0x20000000` [BIN][DER];
* compiler-run metadata at output `+0x39b/+0x39c` drives cross-slot patching
  during stream concatenation [BIN][DER];
* the GLSL stage linker builds a fourteen-entry vertex-output/fragment-input
  varying remap after exact name, type, and size checks, and MxMD resolves `o`
  and `v` descriptors through 16-byte physical-index tables before native
  packing [BIN][DER].
* MxMGLX's register-file strings join MxMD descriptor classes: `v` uses prefix
  `0x10000000`, `c/ci/cb` use `0x20000000`, system inputs use
  `0x40000000`, `p` uses `0x60000000`, and `s` uses `0x70000000`
  [BIN][DER].

The native field boundaries and encoder are no longer the blocking unknown.
What remains ambiguous is the complete semantic mapping from each numeric
native operation to an ALU/texture/control mnemonic, the `xyzw` order of
scattered component metadata, special-lowerer predicate details, the execution
meaning of the program property, and the final seven-bit physical varying
selector.  The
readable operation-name switch in `MxMGLX.dll` decodes Iris tokens and is not a
native disassembler.  These semantic joins still require controlled compiler
outputs, including output properties and generated-run boundaries, capturing
the linked `+0xbb4` buffer, or differential DMA/MMIO capture.

## Driver implementation consequences

M9 needs an engine and compiler backend distinct from Parhelia.  Code that can
be implemented before capture is:

1. a typed M9 packet builder with `0x45` short-header termination;
2. typed H03 numeric-range, H07 32-bit-address, and H06 packed-16 draw records,
   including checked exclusive ends and relocation selectors;
3. register-bank definitions for the proven texture selectors and program
   upload ports, retaining unknown names where semantics are incomplete;
4. a shader-program object model with two-dword instructions, four-instruction
   upload lines, a 384-instruction/96-line bank boundary, and relocations which
   preserve address flag bits;
5. Mesa compiler IR definitions for the recovered Iris operations and
   modifiers, plus the recovered native packer behind a semantic-opcode table;
6. resource checks for four vertex samplers, 15 attributes/outputs, 16 fragment
   samplers/inputs, and six predicate names;
7. command validation which rejects native program selectors outside the
   proven store and checks all bus-master resources and lengths;
8. requester/DMA scaffolding for the proven `2c/2d` start/end pair, `0x404`
   doorbell, fences, bounded idle waits, and reset/error recovery;
9. a vendor-Iris parser/dumper for exact compiler comparisons, while keeping
   native operations without a proven mnemonic behind an explicit unsupported
   boundary.  The five source/resource descriptors, result descriptor, result
   routing, and `xy`/`zw` component-pair modes can already be represented.

The first capture matrix should compile one-operation vertex and fragment
programs with identity operands, then vary one destination, source, swizzle,
write mask, negate/absolute modifier, predicate, saturation/scale, and texture
unit at a time.  Save both the compiler output and the linked 8-byte
instructions with their containing 32-byte upload lines.
For submission, capture inline microcode/constants with all four BM knobs off,
then repeat with each knob enabled.  This will connect the Iris vocabulary to
native bit fields without conflating compiler containers, linked programs, and
DMA packets.
