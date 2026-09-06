# Parhelia draw-packet construction — kernel side (`mtx_parhelia.o`)

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Sub-agent RE, 2026-09-06, from the disassembly only ([BIN] = read from it,
[INF] = inferred). Cross-checked against the live capture
`captures/live-ring-0528/decoded.txt` (the `0x97080001` draw, `0x1000 = 3`,
`0x1040 = 4`, `0x1060 = 0x280cc`, `0x10b8 = 0x20c00000, 0xa1c50020`).

## Call graph [BIN]

| function | .text | role |
|---|---|---|
| `mtx3712` | 0x188f0 | per-draw prep: `ctx+0x42c`, calls `mtx3319`, `mtx2714`, `mtx1035`, `mtx339`, picks the draw builder |
| `mtx3319` | 0xb610 | builds the `0x10b0` primitive word |
| `mtx2714` | 0xb6f0 | packs the `0x10b8` attribute descriptors |
| `mtx1035` | 0xb940 | fills `0x1000/0x1040/0x1060/0x1064` |
| `mtx3355` | 0x417a0 | the only emitter of tags 0x2c, 0x00..0x07/0x10..0x13, 0x19/0x18, 0x2d/0x2e |
| `mtx1017` | 0x60e0 | address-mode builder: copies vertices into the pushbuffer, emits `0x17|flags, start, end` — the captured `0x97080001` path |
| `mtx3063` | 0x6d80 | inline-vertex builder (opcode 0x03) |
| `mtx1767`/`mtx1796` | 0x7b70/0x74d0 | inline 16-bit / 16-or-32-bit index builders (0x07/0x0b) |
| `mtx1443`,`mtx3249`,`mtx2103`,`mtx3311` | 0x5db0.. | address-mode index builders (0x1b/0x1f) |
| `mtx2393` | 0x3c2d0 | kernel "prime the pipe" dummy point draw (`0x17000001`) |
| `mtx1814` | 0x43140 | kernel-internal draw (opcode 0x41), preamble `0x1000=0, 0x1040=4, 0x10b0=0x2305c`, descriptor `0x83c00000` |
| `mtx1937` | 0x32360 | submit: `0x20292820` call packet + `0x408` kick |

Builder dispatch: `fn = table[ctx+0x1cc][prim]`. Index 0 of every table (`ctx+0x1cc == 0`
= "some stream is not GPU-addressable") is always `mtx1017` — the copy path. `ctx+0x1cc`
starts 1 and is cleared when a stream record of kind 1 (user memory, no GPU address) is
seen (`mtx2714+0x23..0xb8`). This is why the GL capture shows copied vertices + `0x97`.

## Draw header layout

Rule for all packets: `low16 = totalDwords - 2`.

**Bits 31/30** come from `ctx+0x42c` (`mtx3712+0x27e`): `0x80000000` if
`(layout.byte1 & 0x10) == 0`, else `0xc0000000`; OR'd into every GL-path draw header.
Bit 31 is always set on the GL path (`0x97` = `0x17 | 0x80`); kernel-internal draws never
set it (`mtx2393` writes bare `0x17000001`). Bit 30 = layout flag 0x10 (`0xd7`).

**Bits [28:24]** — every draw opcode is `0x03` plus mode bits [BIN]:

| byte | built at | meaning |
|---|---|---|
| 0x03 | `mtx3063+0x33c` | inline vertex data |
| 0x07 | `mtx1767+0xbf`, `mtx1796+0x84` | inline 16-bit indices |
| 0x0b | `mtx1796+0x90` | inline 32-bit indices |
| 0x17 | `mtx1017+0x68d`, `mtx2393+0x4da` | vertices by (start,end) address |
| 0x1b | `mtx1443+0x31`, `ctx+0x428 = 0x18000000` | 16-bit indices by address |
| 0x1f | `ctx+0x428 = 0x1c000000` | 32-bit indices by address |
| 0x41 | `mtx1814+0x1ff` | kernel-internal inline draw |

[INF] bit 28 = operands are start/end addresses; bits [27:26] = element kind.

**Bits [22:17]** = dwords per vertex − 1 (`mtx1017+0x67c`: `(Σ stride_i>>2) - 1`;
`mtx3063+0x31a`). Captured 5-float vertex → `0x00080000`. Zero for index packets.

**Low 16**: inline vertex = `nVerts*dwPerVertex - 1`; inline index = `nIndices - 1`
(indices packed 2 per dword, dword-padded; max 0xffff per packet); address mode = 1
(3-dword packet). **Operands** (`mtx1017+0x54d..0x650`): `start` = pushbuffer write
pointer (+ base only if `[buf+0x68] & 0xffffffff0000ff00`) taken before the copy; `end`
= write pointer after the copy — **exclusive**; the header is written *after* the vertex
bytes, so buffer order is `[verts][hdr,start,end]`. Vertices per packet: as many as fit,
rounded per primitive (lists /1, lines /2, triangles /3, quads /4, strips even).

Index address mode (`mtx2661`): low 2 bits of start/end carry half-dword alignment
plus the memory-class bits; the index-mode bits `0x18000000/0x1c000000` are passed
literally by the ICD in its draw record dword 0.

## `0x1000 / 0x1040 / 0x1060 / 0x1064` (state block `[dev+0x40]+0xa78`, emitter `mtx3355`)

| state | reg (tag) | value |
|---|---|---|
| +0x08..+0x24 | `0x1000..0x101c` (tag `0x03020100/0x07060504`) | per-stream base address |
| +0x28..+0x2e (8×u16) | `0x1040..0x104c` (tag `0x13121110`) | per-stream stride in bytes, two per register |
| +0x38 | `0x1064` (idx 0x19) | `ctx+0x254` = max over streams of first valid index |
| +0x3c | `0x1060` (idx 0x18) | `ctx+0x258` = min over streams of `(bufSize - offset) / stride` |
| +0x84 | `0x10b0` | `mtx3319` |
| +0x40/+0x44.. | count / `0x10b8` descriptors | `mtx2714` |

So **`0x1064` = first usable vertex index, `0x1060` = number of vertices addressable in
the bound buffer** (`0x280cc` = 163020 = bytes free / 20). Direct-fetch path
(`ctx+0x1cc == 1`): `0x1000+id*4 = GPU address of the buffer (mtx2320, [obj+0x90]) +
(offset & ~3)` — low 2 bits = memory class — and the real stride goes into `0x1040`.
**Copy path (the capture):** `0x1000 = 2` and `0x1040 = 4` are constant placeholders;
the per-vertex size travels in header bits [22:17]. Immediately before each `0x17`
packet `mtx1017+0x4bf` re-emits `0x1000 = [[chan+0x58]+0x34]` — **the pushbuffer
object's memory-class word (the "3")** — and `0x1040 = 4`. Kernel-internal draws use
`0x1000 = 0, 0x1040 = 4`. `0x1000`'s low value is the memory class of the buffer holding
the vertex bytes, not a format code.

## `0x10b8` attribute descriptors (`mtx2714+0xbb..0x178`)

Input: ICD 4-byte records `r[0..3]`; output dword `d`:

| d bits | source |
|---|---|
| [2:0] | `r[1] & 7` — stream index |
| [11:4] | `r[0]` — offset in **dwords** within the vertex |
| [16:12] | `r[1] >> 3` — field A (destination slot: 0 = position, 5 = second attribute) |
| [19:17] | `r[2] & 7` — field B (format/type code) |
| [25:22] | `(r[2] >> 3) & 0xf` — field C [INF: component mask, xy=3 xyz=7 xyzw=0xf] |
| [26] | `r[2] >> 7` |
| [29:27] | `r[3] & 7` — field D (4 on every ICD attribute, never on kernel ones) |
| [31] | last-attribute flag |

Copy path second pass: every attribute is rebased onto stream 0 with the offset
accumulated across streams. Captured: `0x20c00000` = stream 0, offset 0, A=0, C=0b0011
(`{0x00,0x00,0x18,0x04}`); `0xa1c50020` = offset 2 dwords, A=5, B=2, C=0b0111, last
(`{0x02,0x28,0x3e,0x04}`). So the 5-float vertex was declared as `xy` at dword 0 plus a
3-component attribute at dword 2 — not as one xyzw. Kernel-internal `0x83c00000` = one
4-float attribute at dword 0, last. Formulas: `[x,y,r,g,b]` → `0x20c00000`,
`0xa1c00020 | A<<12 | B<<17`; `[x,y,z,w,u,v]` → `0x23c00000`, `0xa0c00040 | A<<12 | B<<17`.
A/B for colour/texcoord need the ICD side.

## `0x10b0` primitive word (`mtx3319`)

`word = (word & ~0x7f) | mtx734[prim]; bits[14:12] = mtx3178[prim]; bit 17 always set;
bit 18 = (ctx+0x430 == 1)`. Tables (`.data+0x40/+0x80`):

| p | mtx734 | mtx3178 | primitive |
|---|---|---|---|
| 0,1 | 0x20 | 0 | points |
| 2 | 0x3d | 1 | lines |
| 3,4 | 0x3d | 2 | line loop / strip |
| 5 | 0x5c | 3 | **triangles** → `0x0002305c` |
| 6 | 0x5c | 4 | triangle strip |
| 7,8 | 0x5c | 5 | fan / polygon |
| 9 | 0x5c | 6 | quads |
| 10 | 0x5c | 7 | quad strip |
| 11 | 0x3d | 1 | lines variant |
| 12 | 0x5c | 3 | triangles variant |

Bit 18 (`0x0006305c`): `ctx+0x430 == 1` from context init, set to 2 at `mtx3712+0xa6d`
when the packet draw tables are installed; mode 1 is the legacy per-vertex-register path
(`0x1160..0x119c` planes, no `0x97` packets). [INF] bit 18 ≈ legacy path enabled.

## Around the draw

- `0x1090/0x1094` (tag `0x21212425`, `mtx1017+0x1ba`): `0x1090 = ++sequence` (0 skipped),
  `0x1094 = channel id`; then the `0x408` kick; `0x384 = 0` first if the put pointer
  sits in `0x80..0x407`.
- Submit (`mtx1937`): `0x20292820, 0, start|[chan+0x88], end|memclass, 0` — every builder
  sets `[chan+0x88] = 2`, so **`0x10a0 = start|2`, `0x10a4 = end|class`**; if class == 0
  it first polls `0x31c/0x318` for `(val & 0x57) == 0x50` or `& 1`.
- `mtx2393` prime sequence and `mtx1814` internal draw preambles as in the audit doc.
