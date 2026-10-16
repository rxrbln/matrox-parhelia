# M9 Direct3D shader-model 2/3 and mip-generation audit

This audit follows the Direct3D 9 capability declarations into the saved M9
Windows compiler and command serializer.  It distinguishes four layers which
must not be conflated:

1. a public D3D capability or token number;
2. a call-reachable D3D parser route;
3. a common compiler lowerer;
4. an exact native two-dword instruction or final command field.

The pinned input is `MSerX/MxMD.dll`, SHA-256
`b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.
Run `python3 tools/m9-d3d-sm23-evidence.py` to verify every table, call edge,
sampler-bank boundary, and autogen-mipmap anchor described here.  Addresses are
image virtual addresses in that PE32 image.

## Capability declaration versus compiler storage

The D3D9 builder at `0x77fd0` takes configured shader versions from feature
methods `+0x268/+0x26c`, caps each at VS/PS 3.0, and declares these concrete
SM2/3 limits:

| public field | declared value |
|---|---:|
| VS/PS temporary registers | 32 / 32 |
| VS/PS dynamic-flow depth | 24 / 24 |
| VS/PS static-flow depth | 4 / 4 |
| VS constants | 256 vec4 |
| PS2 instruction slots | 512 |
| VS3/PS3 instruction slots | 512 / 512 when configured SM3 |
| maximum executed VS/PS instructions | 65535 / 65535 |
| simultaneous render targets | 4 |
| vertex texture filter | point minification and magnification only |

These are API/compiler limits.  Optimizer `0x1958a0` tests an intermediate
instruction count against `0x400` at `0x195b6a`, then calls whole-program
backend `0x1d1d40` at `0x195b88`.  Native slot packer `0x1db840` has the
separate 256-slot append limit already pinned in
`parhelia-m9-shader-termination-boundary.md`.  The 512-slot and 65535-executed
caps therefore do not describe a 512-entry physical native array.  They can be
valid only through optimization, frontend expansion/control flow, or another
unrecovered scheduling policy.  A new compiler must enforce its measured
native allocation limit rather than copying these cap scalars.

## Direct D3D compiler routes

Direct vertex-program callers `0x10e4e4/0x10e51f` call frontend `0x191130`;
direct pixel-program callers `0x10e716/0x10e759` call frontend `0x1922a0`.
Both call `0x1958a0` after parsing.  The separate Iris/OpenGL entry
`0x1a40b0` also reaches that optimizer, but its enum is not used to name D3D
tokens in this audit.

Frontend `0x191130` contains a complete D3DSIO control table at
`0x192138`, indexed after subtracting opcode `0x19`, and an arithmetic/special
table at `0x19223c`, indexed after subtracting one.  The first table proves the
following call-reachable D3D control and declaration routes:

| D3DSIO tokens | parser target | common lowerer |
|---|---:|---:|
| `CALL` | `0x1918e1` | `0x1a7ce0` |
| `CALLNZ` | `0x191906` | `0x1a7e50` |
| `LOOP` / `ENDLOOP` | `0x1919a2` / `0x1919cb` | `0x1a7f20` / `0x1a80f0` |
| `REP` / `ENDREP` | `0x19191e` / `0x191947` | `0x1a7970` / `0x1a91b0` |
| `IF`, `IFC` / `ELSE` / `ENDIF` | `0x19195d` / `0x19198b` / `0x191975` | `0x1a7040` / `0x1a7780` / `0x1a76a0` |
| `BREAK`, `BREAKC`, `BREAKP` | `0x1919e1` | `0x1a8230` |
| `LABEL` / `RET` | `0x1918bd` / `0x1919fa` | `0x1a7bd0` / `0x1a7b40` |
| `DCL` | `0x191624` | declaration-type subtable `0x192180/0x192194` |
| `DEF`, `DEFI`, `DEFB` | `0x1913f3`, `0x1914e6`, `0x19158b` | stage constant records |

This proves real graph construction for calls, labels, nesting, conditional
breaks, predicates, integer constants, and Boolean constants.  It does not
identify native branch target bits, reconvergence records, or whether a
particular constant branch is folded.  Those fields remain tied to semantic
graph-target placement and the opaque linked-pair byte serializer.

The second table proves these arithmetic families reach dedicated common
lowerers: MOV/MOVA, ADD/SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN/MAX, SLT/SGE,
EXP/EXPP, LOG/LOGP, LIT, DST, LRP, FRC, the five matrix forms, POW, CRS,
SGN, ABS, NRM, SINCOS, CMP, and DP2ADD.  In particular:

| D3DSIO token | parser target | lowerer / exact frontend selector |
|---|---:|---|
| `SLT` | `0x191ea0` | `0x1c96d0`, compare word `0x00040000` |
| `SGE` | `0x191ec8` | `0x1c96d0`, compare word `0x00030000` |
| `CMP` | `0x191fa2` | `0x1c8e90` |
| `SETP` | `0x191fc0` | `0x1c9430` |
| `TEXKILL` | `0x191f95` | `0x1c82d0` |
| `DSX`, `DSY` | `0x191fb1` | shared derivative lowerer `0x1c9300` |
| `TEX` | `0x191d05` | `0x1bda50`, mode derived from token bits 16/17 |
| `TEXLDD` | `0x191d51` | `0x1bda50`, explicit-gradient argument form |
| `TEXLDL` | `0x191d31` | `0x1bda50`, explicit-LOD argument form |

The direct D3D table does **not** route legacy `BEM` opcode `0x59`; it falls
through the table's generic rejection target.  The many PS 1.x texture opcodes
between `0x43` and `0x57` likewise mostly reject here, except EXPP/LOGP.  They
belong to a separate legacy normalization path, not to SM3 hardware claims.

## Pixel normalization boundary

Pixel frontend `0x1922a0` calls normalized-record constructor `0x193b30` at
`0x1930f8`.  The constructor extracts a private seven-bit field, converts up
to five operands through `0x1af770`, and feeds a second table at
`0x1939fc/0x193a9c`.  That table reaches derivative lowerer `0x1c9300`, compare
and selector families `0x1c9690/0x1c9bd0`, and special paths
`0x1c9f00/0x1ca580/0x1ca6e0`.

The seven-bit field is a normalized private semantic.  It is not safe to label
its numeric values as D3DSIO opcodes from static proximity alone.  This is the
narrow static boundary for the remaining pixel-only mapping.  A passive hook
at the call site `0x1930f8`, or entry `0x193b30`, should log the original token
opcode, extracted normalized field, and completed record field `+0x0c`.  One
shader per surviving special operation closes the semantic table without
changing rendering.

## Predication, integer operations, and precision

The public SM2 caps assert VS predication and all five PS2 capability bits.
Static code supports that declaration at the frontend layer:

* `SETP` has a dedicated lowerer and `BREAKP` shares the conditional-break
  builder;
* predicate descriptors `p0..p5` use the separate descriptor family already
  decoded in `parhelia-m9-native-lower-runtime.md`;
* D3D9 DP2 operations 77, 83, 93, and 94 dispatch to VS integer, VS Boolean,
  PS integer, and PS Boolean constant update handlers `0xe1780`, `0xe1710`,
  `0xe1560`, and `0xe14f0`;
* `DEFI` and `DEFB` create corresponding program declarations.

There is no D3D SM3 general integer ALU here.  Integer and Boolean constant
files feed comparisons, address/control expressions, and conversion/lowering;
the OpenGL compiler's wider internal integer vocabulary is frontend IR rather
than evidence for a native integer opcode bank.

The D3D destination modifier is carried into backend metadata, including the
partial-precision nibble.  Static transport proves the modifier survives
parsing; it does not prove distinct reduced-precision arithmetic in silicon.
Keep Mesa precision-neutral until native result/slot comparisons show a
hardware precision mode.

## Texture, stage, MRT, and depth outputs

`TEX`, `TEXLDD`, and `TEXLDL` share `0x1bda50` with different argument forms.
The table therefore closes compile-time acceptance and lowering for implicit,
explicit-gradient, and explicit-LOD requests.  It does not close the final
native texture-control bits.  Vertex texture fetch is structurally supported:
the D3D caps advertise four point-filtered vertex samplers, TEXLDL reaches the
common texture lowerer, and the serializer has the descriptor-only bank
described below.  Linear vertex filtering must remain disabled.

Shadow comparison remains sampler/resource state, not another D3DSIO texture
opcode.  The apparent sampler state-11 entry at `0x105c04` is unreachable
legacy fallback residue; the live matching store at `0x105648` and compiler
consumer `0x17f91d` are `D3DTSS_TEXCOORDINDEX`.  No sampler-side sRGB decode
path is joined.  Neither feature may be inferred from TEX acceptance.

Four output pairs `b0/b1` through `b6/b7`, four color masks, output declaration
lowerer `0x181fa0`, and D3D `NumSimultaneousRTs=4` establish a real four-target
resource/emitter design.  The live RT1/all-target teardown wedge remains an
execution and lifetime blocker.  Pixel depth output is accepted through the
known one-component depth semantic and live `gl_FragDepth` result, while its
native output-route field is still unjoined.  These facts support compiler
work behind gates; they do not make MRT1..3 safe to expose.

## Why eight D3D texture stages are not eight fragment samplers

D3DCAPS9 publishes both `MaxTextureBlendStages=8` and
`MaxSimultaneousTextures=8`.  Three exact implementation layers explain the
discrepancy:

1. fixed-function texture-stage shadowing has 28 logical records and the
   combiner compiler may consume eight API stages;
2. the backend constructs exactly four full `0x64`-byte sampler blocks;
3. final serializer `0xf54d4` specializes `count == 4` and checks exactly four
   pointers at `+0x384/+0x388/+0x38c/+0x390` before looping into full emitter
   `0xf4350`.  That emitter selects final indices `0x70 + unit` and can emit
   address, descriptor, filter/wrap/aniso, LOD, border, and bump state.

There is a second four-entry collection at `+0x394/+0x398`, emitted by
`0xf4630` with selector `0x74 + unit`.  It is not a hidden second full fragment
bank: it emits the texture root and only the `0x78/0x79` descriptor pair, and
lacks the full sampler controls.  Its shape and the D3D vertex-texture cap are
consistent with vertex-stage use, while the later five-sampler capture proves
the same generic descriptor/route operands also carry scratch feedback.

The registry field `3D\AllowMultipass` exists at config offset `+0x30` and
defaults to zero.  No call-reachable consumer has been joined to a pass split,
temporary target, or replayed draw.  It therefore does not explain the sampler
bank discrepancy by itself.  The static interpretation is:

* eight fixed-function combine stages are compiler vocabulary;
* four fully stateful fragment samplers are physically proved;
* four descriptor-only route entries exist and are used by both vertex
  texture/resource paths and fragment scratch feedback;
* the D3D8-era value of eight simultaneous textures requires compiler
  scheduling or virtualization beyond the four physical fragment slots.

The subsequent Linux GL capture in `parhelia-m9-five-sampler-capture.md`
decides the first N+1 case: five unique dependent samplers compile, link, and
produce the exact output while one DMA submission serializes five
texture/raster blocks and distinct resources.  This proves a compiler schedule
beyond four logical fragment samplers.  It does not yet decide whether every
block is one texture instruction phase or a framebuffer pass, nor the maximum
safe schedule.  A clean driver should initially expose four direct fragment
samplers and four point-only vertex samplers, then raise the fragment limit
only with equivalent dependency-preserving intermediate management.

## Autogenerated mipmaps

The global cap has a concrete feature gate.  Builder `0x77340` reaches the
feature object through `+0xfe70`, loads feature entry `+0x248`, calls its
vmethod `+0x0c`, and tests result bit 2 at `0x77883`.  When bit 2 is **clear**,
it ORs `0x40000000` (`D3DCAPS2_CANAUTOGENMIPMAP`) into common `Caps2 +0x14c`.
This inverted polarity must be retained when comparing config captures.

DP2 initializer `0xdd8e0` installs operation **89** at table slot
`0x20cb04`, pointing to `0xe1920`.  Public D3DHAL defines operation 89 as
`GENERATEMIPSUBLEVELS`; operation 96 is `SURFACEBLT`.  Handler `0xe1920`:

1. bounds-checks `count * 8`, proving request layout `{u32 hSurface,
   u32 Filter}`;
2. resolves `hSurface` through the context handle table;
3. passes filters 1, 2, and 3 through, maps 6 to internal 4, maps 7 to internal
   5, and maps every other value to zero;
4. calls the resolved surface object's vmethod `+0x80`;
5. advances eight bytes and finally marks parser success at context `+0x44`.

This is call-reachable mip generation, not a caps-only string.  Static data
flow ends at the surface virtual call, so it does not yet establish the
generation kernel, per-level layout walk, dirty rectangles, resource fence,
or whether generation uses render, blit, or CPU fallback.  Global
`CANAUTOGENMIPMAP` also does not prove which formats carry the independent
`D3DFORMAT_OP_AUTOGENMIPMAP` bit.

The minimal passive Windows hook is a breakpoint at `0xe19aa` after handle and
filter resolution.  Log `ecx`, `[ecx]`, `[[ecx]+0x80]`, the mapped filter, the
surface's format/dimension/mip descriptors, and command-buffer/fence cursors
before and after the call.  Set 4x4 level 0 to distinct texels, generate with
POINT and LINEAR, then read every level.  That single run closes the vmethod
target, mip recurrence, filter semantics, write ordering, and fence ownership.

## Driver boundary

The following are safe implementation inputs now: D3D token validation,
control-flow graph construction, arithmetic/matrix expansion into common IR,
stage-specific constant updates, four full fragment samplers, four point-only
vertex descriptor entries, and the front half of autogen-mipmap dispatch.

Exact DDX/DDY encodings, ordered pre-pack descriptor substitution, the
fixed-chain inline upload, common terminal, and fence lifetime are now closed.
The external programmable path is one opaque immutable linked VS/FS pair
selected through `6c`; it has no public native start/count, per-stage entry, or
end-PC fields.  Private whole-pipeline allocation, keyed reuse, COW
publication, the canonical KIL seven-mask basis, TEXLDD's nine-slot expansion,
ten texture metadata expansions, eight shadow-compare functions, and all three
finalizer paths are now exact.  Arbitrary SM2/3 emission remains gated by the
opaque internal pair byte serializer, semantic branch/backedge/CALL/BREAK/
reconvergence placement, linked-resource B/P/L texture controls, unrecorded
predicate combinations, output routing, and hardware stop semantics.  MRT1..3,
more than four simultaneous fragment textures, sampler
sRGB, and mip generation remain disabled until the narrowly scoped
result/resource experiments above close their execution contracts.
