# Matrox M9 texture and sampler state

This note joins Direct3D texture/sampler frontend state to the final M9 indexed
packet emitter in Windows `MxMD.dll`.  The emitter structure and constants are
cross-checked where possible against Linux `m9x_drv.so`.  `[BIN]` means direct
binary data flow, `[DER]` a deterministic decode, and `[INF]` a provisional
semantic name.

## Object path and final packet

Frontend parser `0xd0770` calls D3D texture-stage dispatcher `0x105340` at
`0xd0976`.  Fixed TSS objects begin at `ctx+0x2520 -> +0x3c`, with 28 stages at
stride `0x60`.  Programmable stages 257..272 use dispatcher `0x105cf0` and
samplers at `ctx+0x2518 -> +0x14`, stride `0x74`.  Fixed sampler dispatcher
`0x105850` instead uses `ctx+0x2520 -> +0x2c + stage*0x4a` [BIN].

Lowering `0x17faf0` enumerates units into `0x187860`.  Its working record binds
backend at `+0x0c`, sampler at `+0x24`, fixed TSS at `+0x28`, resource at
`+0x1c`, and input descriptor at `+0x14`.  Selector builder `0x187950`, resource
format join `0x187b50`, central lowerer `0x18b4e0`, and filter helper `0x18afb0`
produce the four 0x64-byte backend blocks initialized by `0xf4120` at batch
offsets `+0x4b4,+0x518,+0x57c,+0x5e0` [BIN].

Final emitter `0xf4350` maps each backend exactly:

| dirty group | final indices | backend fields |
|---:|---|---|
| common | dynamic `70 + unit` | `+0x10` |
| `0x02` | `78,79,7a` | `+0x4c,+0x50`, relocated resource `+0x44/+0x48` |
| `0x04` | `7c,7d,87` | `+0x14,+0x18,+0x54` |
| `0x08` | `82,83,84,85` | `+0x2c,+0x30,+0x34,+0x38` |
| `0x10` | `80,81,7e,7f` | `+0x24,+0x28,+0x1c,+0x20` |
| `0x20` | `c0` | resource `+0x3c/+0x40` |

It rewrites `f0` bits `14:13` with the unit and uses relocation selectors
`unit+0x0b` and `unit+0x0f`.  Comparator `0xf41f0` establishes dirty groups;
group `0x08` is conditional on backend `+0x10 & 0x3000 == 0x2000`.  Field
`+0x54`/index `87` is initialized, compared, and emitted, but the central
lowerer never writes it.  Keep it reserved.

## Direct3D sampler shadow

Dispatcher `0x105850` packs these public states [BIN]:

| sampler state | shadow field | dirty group |
|---|---|---:|
| ADDRESSU/V/W | `+0x08` bits `2:0/5:3/8:6` | `0x02` |
| BORDERCOLOR | raw `+0x2e`, clear `+0x3e` | `0x04` |
| MAGFILTER | `+0x08[18:15]` | `0x08` |
| MINFILTER | `+0x08[22:19]` | `0x08` |
| MIPFILTER | `+0x08[24:23]` | `0x08` |
| MIPMAPLODBIAS | float `+0x18` | `0x10` |
| MAXMIPLEVEL | `+0x1c` | `0x10` |
| MAXANISOTROPY | `+0x46[4:0]` | `0x08` |

Mapped HAL/DDI state 29 (`D3DSAMP_SRGBTEXTURE`) sets `+0x46` bit 5; states
33/34 select a special border source through `+0x08` bit 14 and `+0x42`; state
35 sets `+0x46` bit 10; state 36 sets
bit 11; and state 37 sets bit 12, stores low six payload bits at byte `+0x2c`,
bits `7:6` at byte `+0x2d`, and sets bit 13 when byte `+0x2c` is nonzero.
Programmable dispatcher `0x105cf0` mirrors these sampler fields.

Direct3D 9 maps public sampler states to legacy HAL/DDI texture-stage-state
IDs, so public `D3DSAMP_SRGBTEXTURE` arrives as state 29 rather than enum value
11.  Pixel/fixed handler `0x105b82` and vertex handler `0x105fa9` both store its
Boolean at `+0x46 bit 5` and mark dirty group `0x08`.  State-block paths
`0x12fe37`/`0x130b79` and normalized-state output `0x102a3c` preserve/query it.
The pinned final filter helper `0x18afb0..0x18b4d7` and central lowerer
`0x18ba27..0x18c22b` do not consume bit 5, so native decode remains unjoined
[BIN][DER].

The apparent state-11 entry at `0x105c04` is unreachable legacy residue.  Main
dispatcher `0x105340` intercepts state 11 as `D3DTSS_TEXCOORDINDEX` at
`0x105648`; compiler `0x17f91d` consumes its three-bit-per-unit field as a
texture-coordinate record index.  It is unrelated to sampler sRGB.

## Final filter/wrap word: index `7c`

Backend `+0x14`, emitted at index `7c`, is packed exactly as follows [BIN][DER]:

| bits | source |
|---:|---|
| `2:0` | mapped MIN filter |
| `6:3` | mapped MAG filter |
| `8:7` | mapped MIP filter |
| `10` | target/fast-path condition, exact name unresolved |
| `12` | forced one |
| `13` | input-descriptor condition |
| `15:14` | resource conditions |
| `17:16` | mapped anisotropy |
| `21:18` | compiler input descriptor `+0x0c` low nibble |
| `24:22` | mapped ADDRESSU |
| `27:25` | mapped ADDRESSV |
| `30:28` | mapped ADDRESSW |
| `31` | border/color conversion condition |

The literal tables are:

```text
MIN/MAG D3D enum 0..8 -> 0,0,2,3,0,0,4,4,7
MIP     D3D enum 0..2 -> 0,1,2
WRAP    D3D enum 0..6 -> 0,0,4,1,7,5,6
ANISO   0..16          -> 0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2,3
```

Thus anisotropy 1..3 selects 0, 4..7 selects 1, 8..15 selects 2, and 16 selects
3.  Wrap values identify D3D WRAP=0, MIRROR=4, CLAMP=1, BORDER=7, and
MIRRORONCE=5.  Windows table addresses are `0x1f6300` MIN/MAG, `0x1f6348` MIP,
`0x1f639c` wrap, and `0x1f63b8` anisotropy.

Linux 1.15 contains byte-identical tables at file offsets near `0x10723a0`,
`0x1072480`, and `0x10724a0` (VA plus `0x200000`).  No usable live code xrefs
were found, so this corroborates constants rather than a Linux frontend path.

## LOD, border, bump, and compare

Backend `+0x18`/index `7d` is built by `0x1366f0` and `0x18ba27` [BIN]:

| bits | value |
|---:|---|
| `9:0` | signed LOD bias clamped to `[-512,511]` |
| `18:10` | first 9-bit bound clamped to `[0,511]` |
| `27:19` | second 9-bit bound clamped to `[0,511]` |

Backend `+0x10` bit 19 enables this control.  Sources include sampler LOD bias
`+0x18`, max level `+0x1c`, and derived bounds `+0x24/+0x28`; orientation of the
two bounds remains unresolved.

At `0x18be15`, border color from sampler `+0x2e` or special source `+0x42` is
converted through `0x187bc0/0x1897e0` into backend `+0x1c,+0x20,+0x24,+0x28`.
Those four dwords are emitted at indices `7e,7f,80,81`; wrap outputs at least 6
gate their comparison/emission.

Indices `82..85` are definitively `D3DTSS_BUMPENVMAT00/01/10/11`, converted at
`0x18c148` from fixed-stage floats `+0x08,+0x10,+0x0c,+0x14`.  They are not
dimensions.  `BUMPENVLSCALE/OFFSET` at stage `+0x18/+0x1c` are consumed by a
different path.

Shadow comparison at `0x18bf38` requires sampler `+0x46` bit 8 and sampler
`+0x08` bit 13, sets backend `+0x10` bit 26, and puts this mapped D3D comparison
in bits `29:27` [BIN][DER].  Both fields, and this exact function mapping, are
CAPTURED live through `GL_ARB_shadow` in index `0x70 + unit`; a comparing fetch
additionally moves bits `11:8` from `1` to `2`.  See
`parhelia-m9-openbits-gl-capture.md`.

```text
NEVER LESS EQUAL LEQUAL GREATER NOTEQUAL GEQUAL ALWAYS
    0    1     3      2       6        4     5      7
```

## Texture operation and resource dimension

Selector builder `0x187950` puts a four-bit texture-operation/resource-dimension
code in backend `+0x10[11:8]`.  The complete static decision tree is now known
[BIN][DER].  Before resource adjustment it selects:

| condition, in priority order | base code |
|---|---:|
| input descriptor `+0x0c` bit 4 and a bound resource | `1` if resource method `+0x48` returns 1, otherwise `5` |
| input descriptor `+0x0c` bit 6 | `d` |
| input descriptor `+0x0c` bit 8 | `9` |
| input descriptor `+0x0c` bit 7 | `8` |
| otherwise | `1` |

Resource adjustment runs when input descriptor `+0x08` bit 5 is set, or when
`+0x08[11:9] >= 6`.  Resource method `+0x48` returns a dimension kind: kind 0
is ordinary 2D, kind 1 is cube, and kind 2 is volume/3D.  This identification
is direct rather than inferred from the arithmetic: the ordinary constructor
at `0x51990` stores 0 at object `+0x258`, the six-face loop at
`0x53f22..0x54033` stores 1, and the three-dimension mip constructor at
`0x53220..0x532be` stores 2 [BIN][DER].  For a base other than 8, volume adds
one and cube adds two.  Base 8 is special: cube maps it to `c`, while volume
leaves it at 8.  Consequently the reachable families are:

```text
base 1: 2D -> 1, volume -> 2, cube -> 3
base 5: 2D -> 5, volume -> 6, cube -> 3
base 8: 2D/volume -> 8, cube -> c
base 9: 2D -> 9, volume -> a, cube -> b
base d: 2D -> d, volume -> e, cube -> f
```

This corrects the earlier interpretation of `1,8,9,c,d` as bare texture
targets.  They encode a compiler texture-operation family combined with a
resource class.  The input flags originate in the 20-byte compiler record made
by `0x14c6e0`: source token bits 4/6/7/8/9 become record `+0x0c` bits
4/6/7/8/9 at caller `0x14f2ee..0x14f375`.  Static code does not expose names
for those internal token bits, so labels such as projective, explicit-LOD,
cube, or volume remain hypotheses rather than register definitions.

The live resource vtable is `0x1f3a90`, installed at `0xa3323/0xa347a` [BIN].
Relevant slots are:

| slot | function | result |
|---:|---:|---|
| `+0x38` | `0xa6660` | storage/layout class from `+0xb0[11:8]` |
| `+0x3c` | `0xa6d30` | normalized format ID at resource `+0x90` |
| `+0x48` | `0xa6d40` | dimension kind from `+0xb0[20:17]`: 0=2D, 1=cube, 2=volume/3D; values `0..4` unchanged and larger values folded to 5 |
| `+0x54` | `0xa7a60` | resource `+0xb0` bit 7 |
| `+0x5c` | `0xa6740` | relocation plus descriptor pair, return `0x10` |
| `+0x64` | `0xa7a40` | resource `+0x68` |
| `+0x74` | `0xa5e80` | further resource property |
| `+0x78` | `0xa7a10` | resource `+0xd0` |
| `+0x7c` | `0xa7a20` | resource `+0xc8` |

Method `0xa6740(selector=2, reloc, idx78, idx79)` selects cached descriptor
triple 2.  Cache builder `0x467e0` and format helper `0x47350` establish the
ordinary descriptor layout exactly [BIN][DER]:

| output | bits | source |
|---|---:|---|
| `idx78` | `15:0` | cache source `+0x184` |
| `idx78` | `18:16` | source `+0x138[2:0]` |
| `idx78` | `20:19` | zero for selector 2 |
| `idx78` | `21` | source flags dword 0 bit 3 |
| `idx78` | `22` | zero for selector 2 |
| `idx78` | `23` | source `+0x14c` bit 2 |
| `idx78` | `24` | zero for selector 2 |
| `idx78` | `31:25` | source `+0x140[6:0]`, hardware format code |
| `idx79` | `15:0` | source `+0x12c[15:0]`, ordinary vertical extent |
| `idx79` | `27:16` | source `+0x130[11:0]`, or `+0x134` for format `0x3f/0x7f` |

Selector 2 leaves `idx78` bits 19, 20, 22, and 24 clear: its cache entry is
zeroed at `0x46829..0x46833`, and none of its reachable branches writes those
bits.  This corrects an earlier misread that assigned `+0x13c` to bits 20:19.
That assignment belongs to other descriptor selectors: selector 3 writes
class 2 as binary `10`, selectors 5--7 copy `+0x13c[1:0]`, and selectors
18--19 also copy it.  Selectors 5--7 additionally copy `+0x14c` bits 0/1 to
`idx78` bits 22/24.  They are cached alternate views, whereas the live 3D
sampler requests selector 2 [BIN][DER].

Descriptor class 1 replaces `idx79[27:16]` with 1.  For nonzero layout
selector `+0x138` and descriptor class
`+0x13c == 3`, it instead writes `idx78[15:0] = +0x16c`,
`idx79[15:0] = ceil(+0x128 / +0x16c)`, clears `idx79[27:17]`, and forces bit 16.
This is the exact class-3 blocked descriptor formula [BIN][DER].
The triple's first dword is the BO address with low-two attribute bits, followed
by `idx78` and `idx79`.  With no backing, the relocation fallback uses tag
`0x20000000 | resource+0x70`.

Fields `+0x184` and `+0x130/+0x134` are size/layout derived, while `+0x13c`
values 1..3 distinguish descriptor storage/layout classes.  Method `0xa6660`
returns `+0xb0[11:8]` for values 1..4 (and forces 3 when resource `+0x70` is
nonzero).  Constructor path `0xa4010` maps source `+0x1e8` values `{3,4}->1`,
`2` and out-of-range values to 2, `1->3`, and `{0,5,6}->4`; alternate path
`0xa42a0` maps its source values `0->4`, `1->3`, and all others to 2.  These
numeric joins are high confidence, but the source enums remain unnamed.

The `+0x184/+0x188` pair is now resolved as aligned horizontal and vertical
allocation extents [BIN][DER].  The simple constructor path sets
`+0x184 = align(+0x128, 8)` and aligns `+0x188` from `+0x12c` according to the
format.  Slice-offset helpers at `0x470c4..0x47183` multiply
`+0x184 * +0x188`, apply the format element size, and scale by selected
slice/subresource index `+0x150`.  Thus
`idx78[15:0]`, copied from `+0x184`, is the aligned horizontal row span in
format elements.  It is width-like/pitch-in-elements, not byte pitch; whether
hardware documentation names it width or pitch still requires an MMIO capture.
Index `79[15:0]` is the unaligned vertical extent from `+0x12c`; the
`+0x130/+0x134` value copied to `idx79[27:16]` remains the depth/layer-like
extent outside the special class-3 layout.  Defaults before resource fill are `idx78=0xcc010001` and
`idx79=0x00010001`; function `0x18b7f4` may clear `idx78[20:19]` based on the
combined operation/resource code.

### Block geometry and the class-3 boundary

Constructor `0x45eb0` derives an element-size category `+0x168` and reads its
horizontal and vertical allocation quanta from tables at `0x1f19a0` and
`0x1f1a78` [BIN][DER].  The horizontal quantum at `+0x16c` normally uses table
index `6 * category + device_mode + 3`; the three entries are:

| category | element bits at `+0x164` | `+0x16c` for device modes 0/1/2 |
|---:|---:|---:|
| 0 | 128 | `10 / 8 / 8` |
| 1 | 64 | `20 / 10 / 10` |
| 2 | 32 | `20 / 20 / 20` |
| 4 | 16 | `20 / 20 / 20` |
| 5 | 8 | `20 / 20 / 20` |
| 6 | 8 | `40 / 20 / 20` |
| 7 | 4 | `40 / 20 / 20` |
| 8 | 1 | `100 / 100 / 100` |

All values in that table are hexadecimal element counts.  The `device_mode`
index is the remapping made at `0x4602b..0x4604a`: device property `+0x24`
values 0 and 1 become indices 2 and 1, with other values becoming index 0.
The three 192-byte Windows geometry tables beginning at `0x1f19a0`,
`0x1f1a78`, and `0x1f1b50` occur byte-for-byte in Linux 1.15 at file offsets
`0xe9b720/0xe9b800/0xe9b8e0`, with two further identical copies [BIN].
Category 3 takes a separate branch.  Its table indices prove `+0x16c = 8` for both local-layout
states and all three device modes.  The vertical quantum `+0x170` is also 8
for both local-layout states and all three device modes.  The class-3 branch
then computes:

```text
+0x184 = align(raw_width, 8)
+0x188 = align(raw_height + 8, vertical_quantum)
idx78[15:0] = 8
idx79[15:0] = ceil(raw_width / 8)
idx79[27:16] = 1
```

This resolves the visible class-3 block width and allocation-height rule.  The
CPU mapping path uses a separate linear staging allocation and the GPU upload
receives logical rectangles plus source/destination descriptors; neither path
contains the physical address permutation within a block.  See
`parhelia-m9-blocked-layout-boundary.md` for the exact producer/consumer trace
and the minimal coordinate-coded VRAM capture needed to recover it.

Compression is proven upstream.  Format helpers near `0x4ee70/0x4fd10` map
FourCC DXT1 to internal 0, DXT2 to 2, DXT3 to 3, DXT4 to 4, and DXT5 to 5.
Descriptor initialization near `0x4fdf0` assigns DXT1 type 1/code `0x10` and
auxiliary size field 4; DXT2/3 code `0x15` and auxiliary field 8; and DXT4/5
code `0x17` and auxiliary field 8.  Those values are not byte counts; they are
likely dword or half-block units [INF].
Layout helper `0xa3ba0` applies minimum-four block geometry and the format's
width/stride transforms.  Constructor `0x45eb0` stores the format descriptor's
code in cache source `+0x140`, and ordinary selector 2 copies it unchanged to
`idx78[31:25]`.  Thus DXT1 reaches the hardware field as `0x10`, DXT2/3 as
`0x15`, and DXT4/5 as `0x17`; common non-DXT codes are joined below.

Constructor `0x45eb0` also stores two caller dimensions at source
`+0x128/+0x12c` and aligned/layout-adjusted forms at `+0x184/+0x188`.  The
simple path aligns the first dimension to eight and the second to a
format-dependent one/four boundary.  Selector 2 copies `+0x184` to `idx78`
low 16 and the original second dimension `+0x12c` to `idx79` low 16; selector 1
uses the aligned second dimension `+0x188`.  The custom value at
`+0x130/+0x134` reaches `idx79[27:16]`; ordinary 2D constructor caller
`0x51bd2` supplies 1, strongly suggesting depth/layer count.

### Uncompressed hardware format codes

The non-DXT format gap can be closed for the common Direct3D formats.  Function
`0x4ee30` maps public `D3DFORMAT` values to the driver's internal IDs; the
86-entry descriptor initializer `0x4fe00` maps those IDs to the seven-bit value
that `0x45eb0` and selector 2 place in `idx78[31:25]` [BIN][DER].  This is the
resulting joined table; all numeric entries are hexadecimal:

| Direct3D format | internal ID | `idx78[31:25]` |
|---|---:|---:|
| `R8G8B8`, `X8R8G8B8` | `0d` | `66` |
| `A8R8G8B8` | `0c` | `66` |
| `R5G6B5` | `0a` | `4e` |
| `X1R5G5B5` | `09` | `4a` |
| `A1R5G5B5` | `08` | `4a` |
| `A4R4G4B4` | `06` | `46` |
| `X4R4G4B4` | `07` | `46` |
| `A8` | `36` | `33` |
| `A2B10G10R10` | `10` | `68` |
| `A8B8G8R8` | `0e` | `64` |
| `X8B8G8R8` | `0f` | `64` |
| `G16R16` | `0b` | `78` |
| `A2R10G10B10` | `11` | `6a` |
| `A16B16G16R16` | `12` | `38` |
| `L8` | `33` | `30` |
| `A8L8` | `37` | `51` |
| `A4L4` | `34` | `31` |
| `V8U8` | `3a` | `59` |
| `L6V5U5` | `3b` | `55` |
| `X8L8V8U8` | `3c` | `71` |
| `Q8W8V8U8` | `3d` | `65` |
| `V16U16` | `3e` | `79` |
| `A2W10V10U10` | `3f` | `6d` |
| `L16` | `35` | `50` |
| `Q16W16V16U16` | `40` | `39` |
| `R16F` | `17` | `54` |
| `G16R16F` | `19` | `74` |
| `A16B16G16R16F` | `15` | `3c` |
| `R32F` | `18` | `75` |
| `G32R32F` | `1a` | `3b` |
| `A32B32G32R32F` | `16` | `34` |
| `UYVY`, `YUY2` | `43`, `44` | `58` |

The same frontend explicitly rejects `R3G3B2`, `A8R3G3B2`, `A8P8`, `P8`,
`D32F_LOCKABLE`, `D24FS8`, and `CxV8U8` by returning internal ID `-1`.
Depth/stencil descriptor codes are also recoverable (`D16=5c`, `D32=7c`,
`D15S1=5d`, and the D24 family=`7d`).  `5c`, `7c` and `7d` are now CAPTURED
from the live Linux vendor ICD sampling `GL_DEPTH_COMPONENT16/32/24` with
comparison, so depth texture sampling is proven, not merely tabulated — see
`parhelia-m9-openbits-gl-capture.md`.  In the Linux packet the field sits in
index `79` bits `31:25`, with `GL_RGBA8` landing on `64` as the control.  A
depth surface rendered by the GPU samples back complemented (`1 - z`), which
ordinary depth testing hides but a shadow sampler does not.

### Channel routing

The live D3D9 selector-2 descriptor has no independent channel-swizzle field
[BIN][DER].  Constructor `0x45eb0` copies the format descriptor's seven-bit
hardware code to `+0x140`; builder `0x467e0` copies that code to
`idx78[31:25]` and leaves the otherwise plausible routing bits 22/24 clear.
Distinct channel orders already have distinct format codes where required:
for example A8R8G8B8 is `66`, A8B8G8R8 is `64`, and the two 10:10:10:2 orders
are `68` and `6a`.  Format-descriptor component masks used by CPU conversion
and border-color helpers do not flow into indices 78/79.

Therefore a driver implementing this D3D path must choose the matching format
code rather than program a separate sampler swizzle.  This does not prove that
M9 has no channel-routing mechanism in another packet or shader instruction.
The compiler input nibble copied to index `7c[21:18]` is separate sampler or
coordinate control; static data flow does not identify it as component routing.

### Mipmap and address layout boundary

The final sampler bind emits one relocated base address plus indices `78` and
`79`; there is no per-level address array in this packet [BIN].  Mipmap LOD and
bounds travel separately through index `7d`.  The allocation helper computes
slice offsets from aligned horizontal/vertical extents, a selected subresource
index, and special DXT block geometry.  The hardware sampler therefore receives
one root address and one compact layout descriptor for the active resource.

The volume-texture constructor at `0x531e0` proves the ordinary per-mip storage
recurrence [BIN][DER].  For level `L`, it creates a separate subresource with:

```text
width[L]  = max(base_width  >> L, 1)
height[L] = max(base_height >> L, 1)
depth[L]  = max(base_depth  >> L, 1)
size[L]   = subresource +0x1bc
offset[0] = 0
offset[L] = offset[L-1] + size[L-1]       for L > 0
allocation_size = offset[last] + size[last]
```

Here `+0x1bc` is a byte size, not an inferred count: the subresource embeds the
`0x45eb0` layout object at `+0x3c`, and `0x46470..0x464d9` combines aligned
geometry with the element bit count, converts bits to bytes, aligns the result,
and stores it at layout `+0x180`, which is subresource `+0x1bc`.
The constructor saves sizes at `0x534b6..0x534c3`, accumulates offsets at
`0x534d0..0x534e5`, and forms the final size at `0x5355f..0x53570`.

Small levels use an exact packed tail [BIN][DER].  The trigger is width and
height, after the constructor's small-format adjustment, at most 16 and
adjusted depth at most 15 (`0x53388..0x533b9`).  The first tail level sets
layout flags `0x18`; later tail levels retain bit `0x08`.  Bit `0x08` makes
layout constructor `0x45eb0` force minimum aligned allocation extents 32 by 16
(`0x4634d..0x4637b`, `0x4644a..0x4646a`).  The five relative tail levels occupy
this atlas:

| relative level | X | Y | minimum logical rectangle | linear texel offset |
|---:|---:|---:|---:|---:|
| 0 | 0 | 0 | 16x16 | 0 |
| 1 | 16 | 0 | 8x8 | 16 |
| 2 | 24 | 0 | 8x8 | 24 |
| 3 | 16 | 8 | 8x8 | 272 |
| 4 | 24 | 8 | 8x8 | 280 |

The X table is at `0x1f10cc`, Y at `0x1f1700`, linear offsets at `0x1f1714`,
and minimum rectangles at `0x1f1728`.  Every linear offset is exactly
`Y * 32 + X`.  Constructor `0x52f00` stores the relative level at subresource
`+0x2e8`, X/Y at `+0x2ec/+0x2f0`, the upload byte offset at `+0x2f4`, and marks
the tail through `+0x2e0 = 1` (`0x538d3..0x53962`).  Later levels reuse the
first tail allocation rather than extending the ordinary offset recurrence.

For uncompressed formats the byte offset is the table's texel offset times
bytes per texel.  For block-compressed formats it is
`((X >> 2) + 8 * (Y >> 2)) * bytes_per_block`, yielding DXT1 byte offsets
`0,32,48,160,176` and 16-byte-block DXT byte offsets
`0,64,96,320,352`.  Upload helpers `0x56a10` and `0x56c60` add the saved X/Y
coordinates to an upload box and enforce the saved minimum rectangle.  Helper
`0x56910` creates the shared 32x16 view only for relative level zero and stores
it at `+0x2fc`.  This recovers allocation and upload placement without a
capture.  `tools/m9-texture-tail-evidence.py` verifies the tables, their
coordinate relation, compressed offsets, and constructor/helper call joins
against the saved binary.

Function `0x467e0` builds more than twenty cached relocation/descriptor triples,
and `0xa6740` selects among them.  These are descriptor *views* selected by the
caller; they must not be interpreted as mip levels.  The 3D lowerer always asks
for selector 2.  Consequently a new driver should not manufacture one packet
or one relocation per mip level from this table without capture evidence.

## Confidence and remaining capture points

High confidence: final index placement; `idx78` format field; the joined common
format codes above; aligned horizontal/vertical allocation extents; all sampler
filter, wrap, anisotropy, compare, border, bump, and LOD bit positions; and the
complete numeric transition tree and 2D/cube/volume dimension join for backend
`+0x10[11:8]`.  Selector-2 bits 19/20/22/24 being zero and the ordinary volume
mip offset recurrence are also high confidence.  The packed-tail 32x16 atlas,
per-level coordinates, and uncompressed/DXT upload offsets are exact.

Medium confidence: `idx78[15:0]` is best named row span/pitch in format elements,
and `idx79[27:16]` is depth or layer count.  Both names follow construction and
size arithmetic, but the binary contains no symbol names.

Unresolved: semantic names for compiler-record bits 4/6/7/8/9 and therefore
for operation families `1/5/8/9/d`; the hardware-defined physical address
permutation within class-3 blocks; whether a non-D3D path exposes programmable
channel swizzle;
and the exact meaning of the nonzero descriptor storage/layout classes.
Focused MMIO captures of otherwise identical projected/LOD samples and
coordinate-coded class-3 blocks are the shortest way to settle these
boundaries.  Packed-tail allocation and upload placement no longer require a
capture.
