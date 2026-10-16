# M9 GigaColor and FAA static closure

> **Correction, 2026-09-19.**  The multisampling half of this note is
> superseded by `parhelia-m9-faa-re.md`, which carries the hardware
> measurements as well.  Four statements below are wrong and are corrected
> there, with the instruction bytes:
>
> * the three AF flag bits are at **9, 10 and 11**, not 11, 12 and 13, and
>   the old AF's low twelve bits are discarded, not preserved - the
>   construction shifts right by three at `0x13933c` before it shifts right
>   by five;
> * **"AD bits 6:4" do not exist**: `0x139521..0x139594` writes nested
>   compiled state `+0x04`, a different dword, which the lowerer sends as
>   **index `ef` bits 31:29**, and which the M9128 obeys;
> * **auxiliary state 25 is `GL_SAMPLE_COVERAGE_VALUE`**, named from the
>   vendor ICD's own `glSampleCoverage` sink, so AF`[8:0]` is that value in
>   1.8 fixed point; 24 is its invert flag and 23 the multisample mode word;
> * the 16-bit word at `+0x358 + 2*target` is the per-target **colour write
>   mask**, four ARGB nibbles, not an AA output mask.
>
> The GigaColor half of this note stands unchanged.

This note records the implementation evidence for native 10-bit color and
multisampling in the saved 32-bit M-Series Windows driver. It deliberately
separates Direct3D declarations, internal resource metadata, final command
words, and diagnostic strings. It does not import register meanings from the
older Parhelia `MTXPARD.dll` driver.

Run the deterministic verifier from the `xf86-video-parhelia` root:

```sh
python3 tools/m9-gigacolor-faa-static-evidence.py
```

The verifier pins these exact inputs:

| image | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| `xddm32_210_00_105_se_u_nowhql/MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |

Addresses below are virtual addresses in `MxMD.dll`. `MxMGLX.dll` has image
base `0x10000000`; its file offsets equal RVAs for the cited `.rdata` strings.
`[BIN]` is an instruction/data fact, `[DER]` a deterministic consequence of
joined facts, and `[OPEN]` an implementation gate which static analysis has not
closed.

The descriptor field, the storage class, the render class at index `89`, the
write-mask behaviour and the complete display-controller answer are in
`parhelia-m9-gigacolor-10bit.md`, which extends this note.

## Results which correct earlier assumptions

Three tempting interpretations are now resolved:

1. `(AAMode >> 8) & 0x1f` is the number of Direct3D multisample **quality
   levels**, not the sample count. The independently encoded sample type is
   `(AAMode >> 3) & 0x1f`. The default stores internal type 8 and reports eight
   quality levels.  The format-table constructor stores the nonmaskable type,
   while one GetDriverInfo record path recomputes type 8 [BIN][DER].
2. `FragSize=6.0` is read and copied to a configuration snapshot, but no M9
   allocator, compiler, packet builder, or result path reads that snapshot
   field. There is no static basis for treating six as bytes per fragment, a
   fragment count, or an allocation multiplier [BIN].
3. The alleged `0x68` descriptor-code alias with `X8B8G8R8` is absent.
   `X8B8G8R8` uses code `0x64`; only native `A2B10G10R10` writes immediate
   code `0x68` in the descriptor initializer [BIN].

The older table in `parhelia-m9-texture-state.md` which gives internal `0x0f`
code `0x68` is contradicted by the exact `0x4fe00` jump table and initializer.
The new driver should use the corrected values below.

## AAMode and FragSize configuration chain

The raw Windows configuration object is at device `+0xfe74`; the derived
wrapper collection is at device `+0xfe70`. Its construction is exact [BIN]:

| setting | raw config offset/default | raw read | derived wrapper | snapshot |
|---|---:|---:|---:|---:|
| `3D\AAMode` | `+0x2c = 0x80000842` | `0xb90d0` | `0x6020b`, derived `+0x20c` | `0xbe484..0xbe497`, `+0x68` |
| `3D\FragSize` | `+0x40 = 0x40c00000` (`6.0f`) | `0xb9208` | `0x60317`, derived `+0x210` | `0xbe49a..0xbe4b3`, `+0x6c` |

`0x753e0` allocates the `0xd4`-byte snapshot, calls `0xbe290` at `0x75489`,
and passes the snapshot to `0x10a790`. `0x10a790 -> 0x11b9b0` stores it at
new context object `+0x08` [BIN].

AAMode snapshot `+0x68` is demonstrably live. For example,
`0x11027b..0x110287` follows an enclosing object at `+0xe8`, loads its context
`+0x08`, loads snapshot `+0x68`, and tests bit zero. Compiler `0x139240` follows
the same nested `+0x08,+0x08,+0x68` chain at `0x139250..0x13925f` [BIN].

By contrast, an operand audit over the complete `MxMD` `.text` finds no load
from this snapshot's `+0x6c`. The only direct loads from an object `+0x6c` near
`0x10b30/0x10b3c` operate on the separate 0x69c8-byte device object allocated
by `0x105f0 -> 0x2dea0`; they are not reached through context `+0x08` and are
not the `0xbe290` snapshot. Thus the configuration string and stored default do
not close a hardware `FragSize` path [BIN].

The M9 images contain no textual `FAA` option. They contain `AAMode`,
`FragSize`, `Multisample`, Direct3D render states, and OpenGL multisample entry
points. Naming the hardware behavior FAA remains a product name, not evidence
for a particular storage algorithm.

## Sample count and quality-level decoding

`DdGetDriverInfo` at `0x3db50` dispatches `GUID_GetDriverInfo2` to `0x3e3c0`.
That handler switches on the GetDriverInfo2 type at payload `+0x08`. Its jump
table routes type 22, `D3DGDI2_TYPE_GETMULTISAMPLEQUALITYLEVELS`, to
`0x3e6ad` [BIN]. The corresponding
`DD_MULTISAMPLEQUALITYLEVELSDATA` has `QualityLevels` at `+0x18`.
The handler does exactly:

```c
out->QualityLevels = (AAMode >> 8) & 0x1f;
```

It does not inspect the requested format, `MSType`, or flip flag before writing
this value. The default therefore publishes eight quality levels uniformly;
it does not say “eight samples” [BIN][DER].

The format-capability builder at `0x76dc0` supplies the independent sample type
join [BIN]:

```c
type = (AAMode >> 3) & 0x1f;          /* 0x76dfe..0x76e06 */
ms_mask = (uint16_t)(1u << (type - 1)); /* 0x76e09..0x76e13 */
```

For default `0x80000842`, `type=8` and `ms_mask=0x0080`, the bit for
`D3DMULTISAMPLE_8_SAMPLES`. The builder writes the mask into both 16-bit
multisample fields of each 0x20-byte format-operation record when either field
was already nonzero (`0x76e26..0x76e3b`). If device/context `+0x24` is nonzero,
it advertises zero. If helper argument two is nonzero, it advertises bit zero,
the nonmaskable type, instead of the configured bit (`0x76de2..0x76dfc`)
[BIN].

A separate restriction at `0x76e3f..0x76e6d` applies only when AAMode bit zero
and the sign bit are both set. It clears operation flag bit three for public
formats `0x17,0x18,0x19,0x1a,0x1e`. Neither GigaColor public format `0x1f` nor
`0x23` is on that list [BIN]. The numeric bit is recorded without assigning a
new semantic name.

This closes the configured internal mode as type eight with eight quality
levels.  A later whole-caller audit found two calls to this builder.  The table
constructor passes argument two as one and replaces every eligible pair with
`0x0001/0x0001`, the nonmaskable type.  One GetDriverInfo record path passes
zero and replaces it with `0x0080/0x0080`, the configured type-eight mask; a
sibling query branch copies the constructed record unchanged.  See
`parhelia-m9-format-operation-matrix.md`.  No static path reveals eight sample
locations or eight quality patterns.

## Native GigaColor identity and descriptors

Public mapper `0x4ee30` and its table at `0x4efc4/0x4f060` give [BIN]:

| Direct3D format | public value | mapper target | internal ID |
|---|---:|---:|---:|
| `D3DFMT_A2B10G10R10` | `0x1f` | `0x4ee8b` | `0x10` |
| `D3DFMT_A2R10G10B10` | `0x23` | `0x4eea3` | `0x11` |
| `D3DFMT_A8B8G8R8` | `0x20` | `0x4ee91` | `0x0e` |
| `D3DFMT_X8B8G8R8` | `0x21` | `0x4ee97` | `0x0f` |

The 86-entry descriptor initializer at `0x4fe00` stores a 0x30-byte format
record. Its exact relevant fields are [BIN]:

| internal ID | branch | `+0x04` | `+0x08` hardware code | `+0x0c` | `+0x18` bits | R `+0x1c` | G `+0x20` | B `+0x24` | A `+0x28` |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `0x10` A2B10G10R10 | `0x5005d` | 2 | `0x68` | 1 | 32 | `0x000003ff` | `0x000ffc00` | `0x3ff00000` | `0xc0000000` |
| `0x11` A2R10G10B10 | `0x5009a` | 2 | `0x6a` | 1 | 32 | `0x3ff00000` | `0x000ffc00` | `0x000003ff` | `0xc0000000` |
| `0x0e` A8B8G8R8 | `0x5001a` | 2 | `0x64` | 0 | 32 | `0x000000ff` | `0x0000ff00` | `0x00ff0000` | `0xff000000` |
| `0x0f` X8B8G8R8 | `0x50050 -> 0x50028` | 2 | `0x64` | 2 | 32 | `0x000000ff` | `0x0000ff00` | `0x00ff0000` | `0xff000000` |

There is exactly one `mov [eax+8],0x68` descriptor assignment in the image, at
`0x50064`. Thus code `0x68` is not aliased to `X8B8G8R8` in this binary. The
two eight-bit BGR variants do alias each other at code `0x64`, while their
internal IDs and descriptor `+0x0c` distinguish alpha behavior [BIN][DER].

The resource-layout constructor `0x45eb0` stores the supplied hardware code at
layout `+0x140`. Descriptor builder `0x467e0` shifts it into bits `31:25` for
selector 2 (sampler) and selector 8 (render target). The render lowerer
`0x181fa0` copies compiled resource pairs into the four surface slots, and
emitter `0xfb920` writes `b0/b1` through `b6/b7` [BIN]. Both Giga formats
therefore have real sampling and render-target descriptor construction, rather
than cap strings alone.

The render-target common-class helper at `0x181830` is not another format ID.
For both codes, `(code-0x30)` selects helper case zero and returns class zero;
that three-bit common class is packed into index `89`. It does not collapse the
resource's internal ID, channel masks, or final `b1` descriptor [BIN].

## GigaColor clear, blend, copy, and readback

D3D clear follows `0xdf9d0 -> 0x13de20 -> 0x13e0f0`. For ordinary 32-bit
attachments, the surface's vtable `+0x7c` resolves to `0xa6350`, which calls
format converter `0xb3910`. Its jump tables route internal `0x10` to `0xb3ef7`
and `0x11` to `0xb3ffa` [BIN]. Starting from a D3DCOLOR `0xAARRGGBB`, it uses
x87 truncation after adding 0.5, so the positive-channel equations are [DER]:

```c
u10(c) = (unsigned)(c * 1023.0 / 255.0 + 0.5);
u2(c)  = (unsigned)(c *    3.0 / 255.0 + 0.5);

A2B10G10R10 = u10(R) | u10(G)<<10 | u10(B)<<20 | u2(A)<<30;
A2R10G10B10 = u10(B) | u10(G)<<10 | u10(R)<<20 | u2(A)<<30;
```

Constants `1023.0` and `3.0` are at `0x1f5208` and `0x1f5210`. This is a native
10-bit clear value, not a replicated RGBA8 dword. The resulting word then goes
through the existing 32-bit whole-allocation, rectangle, or CPU fallback clear
selected below `0xa7ee0`; selector 8 supplies the same native render descriptor
used by draws [BIN][DER].

Blend lowerer `0x170ff0` has a 16-bit color-mask shortcut only for internal IDs
`0x0c/0x0d`. IDs `0x10/0x11` go through generic compiler `0x173ee0`, preserving
the 32-bit Giga render format [BIN]. Static code proves reachability but does
not establish every blend-factor rounding result at ten-bit precision [OPEN].

No special Giga readback converter is required for a same-format D3D lock: the
API packed format and backing dword have the layouts above. The generic copy
path `0xb21b0 -> 0x1b8d0 -> 0x1b650 -> 0x1b420` is a resource copy with source
and destination relocations. Static analysis has not joined that path to a
cross-format 10-to-8 conversion. A new driver must therefore treat native
same-format readback as a packed dword and use an explicitly validated render
or CPU conversion for different destination formats [BIN][OPEN].

The saved Linux GL driver's `GL_RGB10_A2` test remains a negative result: it
reported an actual RGBA8 internal format and quantized 10-bit inputs through
8-bit storage. That behavior is separate from the native Windows internal IDs
and cannot validate them.

## GigaColor presentation, LUT, dither, and depth

`MxMGLX.dll` contains this diagnostic at file offset `0x7f15a0`, phrase offset
`0x7f15c5` [BIN]:

```text
WGLICDLink: disabling page flipping, Page flipping is not supported when using gigacolor
```

This proves a no-page-flip presentation policy. It does not prove a scanout
format, a resolve, or a display-pipeline conversion. A blit/composited present
path is the conservative implementation until KMS depth-30 scanout is measured.

Neither `MxMD.dll` nor `MxMGLX.dll` contains `GigaColorFullScreen` or
`GigaColorRender`. Those two config names occur only in the separately saved
Linux display driver. Consequently there is no Windows config consumer to join
in these DLLs [BIN].

The audited Giga surface, clear, blend, copy, and WGL paths contain no joined
special depth format, depth-30 mode, display LUT, output gamma, or dithering
control. Normal depth descriptors remain the ordinary D16/D15S1/D32/D24 family
(`0x5c/0x5d/0x7c/0x7d`) and are bound independently from the color descriptor.
No branch selecting one of those depth formats specifically for internal
`0x10/0x11` was recovered [OPEN].

This absence matters: native 10-bit renderability is statically strong, but it
does not authorize a 30-bit scanout visual or imply that the CRT LUT operates
at ten-bit precision.

## Multisample frontend and compiler

The Direct3D state dispatcher routes render state 161,
`MULTISAMPLEANTIALIAS`, to `0x10360b`. It updates frontend `+0xf4` bit zero and
sets dirty `+0x04 |= 0x1000`. State 162, `MULTISAMPLEMASK`, reaches `0x103635`,
stores the raw 32-bit value at frontend `+0xe8`, and sets the same dirty bit
[BIN]. These later appear at validator `+0x174` and `+0x168`, respectively.

Compiler `0x139240` is called from `0x1399d0` at `0x139b0a`. Its output dwords
at validator `+0x368/+0x36c` alias nested compiled-state `+0x1b0/+0x1b4`,
because the nested base begins at validator `+0x1b8` [BIN].

The compiler permits this path only for current surface internal IDs
`0x0c,0x0d,0x0e,0x0f,0x10,0x11` (`0x139490..0x1394fa`). This is the six-member
32-bit RGB family, including both native GigaColor orders [BIN].

The local enable decision includes AAMode bit zero (`0x139250..0x139267`) and
API-state bits from validator `+0x174`. Default AAMode `0x80000842` has bit zero
clear, so the internal type-eight configuration does not force AA on every
surface [BIN][DER].

## Exact AD dword and allocation join

The surface interface vtable is `0x1f3ba0`; slot `+0x60` is `0xa6d00`. The
method follows [BIN]:

```text
surface +0xa8
  -> object +0x420
  -> object +0x24
  -> resource +0x1e4
  -> allocation +0x14
```

and returns zero if any required object is absent. `0x1395a4..0x1395dc` calls
that method and constructs [BIN][DER]:

```c
AD = (AD & ~0x001fffff) |
     min(allocation_byte_length >> 5, 0x000fffff);
```

Allocation `+0x14` is conclusively a byte extent. Resource-copy relocation
builder `0x1b420`, at `0x1b442..0x1b455`, computes
`allocation_base(+0x20) - resource_base(+0x198) + allocation_extent(+0x14) +
0xfff` and page-aligns the result. Independent surface builder `0x4bbd0` reads
the same `resource+0x1e4 -> allocation+0x14`, shifts by five, and packs its
low 21 bits at `0x4c071` [BIN]. This rules out address and row-stride
interpretations.

Before the backed-surface length is known, `0x1392e4..0x139304` seeds AD with
`(old & 0xffe00020) | 0x20`. Branches `0x139521..0x139594` manipulate bits
`6:4` as intermediate/fallback states. On an ordinary backed surface the later
low-21-bit replacement overwrites all of those bits. They cannot be labeled as
simultaneous final AA mode bits [BIN][DER]. If the length getter returns zero,
the fallback retains the seed/control form; its hardware meaning remains open.

This closes AD low 21 as allocation extent in 32-byte units. It does not say
that `allocation_byte_length` contains eight fragments per pixel. The resource
may use an enlarged or special layout chosen earlier, but no sample-count
multiplier or physical fragment permutation is joined here [OPEN].

## Exact AF dword and sample mask

When compiler `[validator+0x1b0]` is nonzero, `0x139316..0x139359` constructs
AF exactly [BIN]:

```c
uint32_t flags = (((validator_8c >> 2) & 0x00020000) |
                  ( validator_8c       & 0x00040000) |
                  ( validator_174      & 0x00010000)) >> 5;
AF = (old_AF & 0xfffff000) |
     flags |
     (magic_bias_round(validator_float_170) & 0x1ff);
```

The magic double at `0x2082c8` is `26388279066624.0`
(`0x42b8000000000000`); the low dword after the add/store supplies the low nine
bits. The visible bit consequences are [BIN][DER]:

| AF field | source |
|---:|---|
| `[8:0]` | low nine bits of the magic-bias float conversion from validator `+0x170`, written by reserved DP2 opcode 5 auxiliary state ID 25 |
| `[10:9]` | cleared by the merge |
| `11` | validator `+0x174` bit 16 |
| `12` | preserved old bit 12 OR validator `+0x8c` bit 19 |
| `13` | preserved old bit 13 OR validator `+0x8c` bit 18 |
| `[31:14]` | preserved old bits |

The producer of validator float `+0x170` is now exact [BIN]. The DP2 parser at
`0xd0770` maps reserved opcode 5 through compressed selector 2 to
`0xd0824`. That branch consumes the command count as eight-byte records, loads the
record dwords as `{id,value}`, and makes the image's only direct call to
auxiliary-state dispatcher `0x104ce0` at `0xd083e`. The dispatcher accepts IDs
1 through 31. Its complete target table at `0x1052b8` maps ID 25 to
`0x1051b9`, whose handler interprets `value` as float bits, stores it at
frontend `+0xf0`, and marks frontend dirty word `+0x08 bit 1`.

Constructor `0x137110` publishes compiler/validator `+0x80` as that frontend
at `0x137159..0x137166`; frontend `+0xf0` is therefore exactly validator
`+0x170`. Initializer `0x12de30` leaves `1.0f` on the x87 stack and stores it
at `0x12dfbf`, so the default magic-bias result contributes `0x100` to
AF `[8:0]` [BIN][DER]. The verifier pins the complete opcode-5 parser slice,
the sole direct call, all 31 table entries, the complete dispatcher slice, the
ID-25 handler, the alias, and the default store.

No public name for reserved opcode 5 or auxiliary ID 25 was recovered from the
saved binaries. Its numeric transport, destination, dirty bit, reset value,
and AF transform are closed; its API/product semantic and the three other AF
source bits are not. Static analysis still does not justify naming ID 25 as a
sample count, coverage inversion, or sample-pattern selector [OPEN].

`MULTISAMPLEMASK` is joined only at one behavior point. At
`0x138bd9..0x138bf0`, if compiled AD is zero, validator `+0x1ac >= 6`, and
validator sample mask `+0x168` is zero, the compiler clears the current target's
16-bit output mask at `+0x358 + 2*target` [BIN]. No path copying nonzero mask
bits into AD or AF was recovered. This proves zero-mask suppression; arbitrary
per-sample coverage and inversion remain open.

## Lowering and packet emission

Lowerer `0x181fa0` copies [BIN]:

```text
compiled +0x1b0 -> surface shadow +0x28  at 0x1826a5
compiled +0x1b4 -> surface shadow +0x2c  at 0x1826ae
surface dirty |= 0xc0                 at 0x182751
```

Emitter `0xfb920` then emits header `0xadadadad` and streams the same
surface-shadow `+0x28` dword four times (`0xfbe09..0xfbe31`). It follows with
header `0x454545af` and surface-shadow `+0x2c` once (`0xfbe37..0xfbe48`) [BIN].
The repeated AD writes look like a four-entry indexed FIFO transaction; they
are not four statically distinct registers. The packet words to implement are
therefore the exact repeated AD dword and the AF dword, with relocations and
allocation lifetime handled before submission.

## Fragment storage, resolve, query, presentation, and synchronization

The following boundaries remain explicit:

| item | strongest static result | missing fact |
|---|---|---|
| allocation size | AD carries the current color backing allocation byte extent / 32 | where a multisample surface creation path enlarges or transforms that allocation |
| fragment layout | selector 8 supplies the render descriptor and AD supplies total extent | per-pixel fragment order, block permutation, compression, and whether every sample is stored |
| sample positions | internal sample type 8 and eight quality levels; stored format masks say nonmaskable while one query path recomputes type 8 | coordinates/pattern for each quality level and the query branch used by the runtime |
| coverage mask/invert | zero mask suppresses target output; AF low nine is auxiliary state 25 with default float 1.0 -> `0x100` | public semantic of auxiliary state 25, nonzero mask-bit mapping, and coverage inversion |
| resolve | generic copy `0xb21b0` exists; AAMode bit zero changes surface path decisions at `0x994df..0x9951a` and `0x9b569..0x9b59c` | a call edge from multisample surface state to a specific resolve/copy packet and filter |
| query scaling | MxMD/D3D result `0x1199a0` type 9 returns `[query+0x30] - [query+0x28]` directly at `0x1199ef..0x1199f5`. MxMGLX/GL `0x100b96f0` calls four-slot summer `0x10097240`, then, when context `+0x258:+0x20 == 1`, multiplies the sum by context `+0x258:+0x08` at `0x100b97f9..0x100b9808` | whether the hardware counters count pixels, covered samples, or stored fragments; the GL layer treats the context dword as the required multisample scale |
| presentation | WGL disables page flipping for GigaColor; independent-buffer multisample is rejected by diagnostic | exact multisample/Giga blit-present sequence and completion point |
| synchronization | generic resource copy and render submission use ordinary dependency/fence machinery | dedicated resolve completion or fragment-buffer hazard rule, if any |

`MxMGLX` diagnostics at file offsets `0x7f1d90` (“CreateDepthAA64Buffer
failed”) and `0x7f1f54` (“ARB_MULTISAMPLE not supported for OGL independent
buffer”) establish allocation/presentation restrictions only. With no direct
xref or MxMD resource join, they are not evidence for a 64-bit depth layout or
a resolve command [BIN].

## Safe implementation boundary

The from-scratch driver can implement these pieces from static evidence:

- native `A2B10G10R10` and `A2R10G10B10` resource identities, channel masks,
  32-bit element size, sampler/RT descriptor codes, and D3DCOLOR clear packing;
- an experimental internal type-eight mode with eight reported quality levels,
  while retaining the distinct stored-nonmaskable and recomputed-query paths;
- Direct3D-style AA enable and zero-mask output suppression;
- AD/AF shadow construction exactly as above, including AD allocation extent,
  auxiliary-state-25 AF low field and default, lowerer copies, repeated AD
  packet emission, and one AF packet value.

Native GigaColor still needs render/sample/readback sentinel tests before
public exposure. Multisampling remains unsafe to enable because allocation
factor/layout, sample pattern, nonzero mask semantics, resolve, the hardware
query-counter unit, and present synchronization are not closed.

The query statements are deliberately layer-specific. MxMD performs no
software AA scaling for its D3D type-9 result. MxMGLX does perform an OpenGL
post-scale after aggregating the enabled MxMD results: it multiplies by the
context sample-count dword when the context multisample flag equals one [BIN].
This establishes the vendor's GL result policy without proving the physical
unit counted by one MxMD hardware snapshot.

## Minimal dynamic closure captures

A final capture campaign should hook the already isolated boundaries rather
than repeat broad MMIO logging:

1. At `0x4ee30`, log public format, internal ID, descriptor masks, selector-2
   and selector-8 words for `0x1f`, `0x23`, `0x20`, and `0x21`.
2. At `0xb3910`, log D3DCOLOR input and four output dwords for sentinel colors
   that separate R/B order and 8-to-10 rounding; snapshot the raw BO after
   clear, draw, blend, sample, and lock.
3. At surface allocation return and `0xa6d00`, log width, height, pitch,
   multisample type/quality, allocation `+0x14`, layout class, and selector-8
   descriptor. Sweep types 2, 4, and 8 and quality 0..7.
4. At reserved-opcode-5 parser `0xd0824`, auxiliary handler `0x1051b9`,
   compiler `0x1395a4`, and emitter `0xfbe09`, log the auxiliary ID/value,
   validator `+0x18,+0x8c,+0x168,+0x170,+0x174`, surface internal ID,
   allocation length, final AD, and final AF.
5. Render one covered pixel with a unique color per sample/quality, then map the
   raw backing to recover physical fragment order and sample positions.
6. Break on `0xb21b0`, `0x1b8d0`, and present entry points while resolving an
   AA surface. Record the first call that consumes the AA source and the fence
   observed before destination reuse.
7. Surround an occlusion query with the same primitive at sample types 1/2/4/8
   to determine whether the returned count is fragments, covered samples, or
   pixels.
8. Compare windowed blit present and fullscreen present for both Giga formats;
   separately capture any display-depth, LUT, and dither writes. Do not infer
   these from the WGL diagnostic.

Until those deltas exist, the exact unknowns above should remain capability
gates in Mesa rather than guessed AD/AF field names.
