# Parhelia indexed draws, vertex streams and attribute descriptors

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Clean-room RE from binaries only (`matroxdriver-x86_64-cvs-20090511/kernel/
mtx_parhelia.o`, unstripped; capture corpus `captures/gl-matrix/*-secs.txt`).
No proprietary source was read.

Evidence tags: **[BIN]** read from a disassembled instruction (symbol +
`.text` offset given), **[HW]** from a hardware capture, **[INF]** inferred.

---

## 0. Answer in one paragraph

**The hardware supports indexed drawing natively, in four flavours.** The
draw packet's opcode field is a 3-bit *draw mode* selector; two of its values
name 16-bit indices and two name 32-bit indices, in an inline form (indices
copied into the command stream) and an address form (indices left in a
GPU-addressable buffer, described by a start/end bus-address pair). Indexed
draws fetch their vertices from up to **8 bound vertex streams**, each with
its own base address and its own **byte stride**, so *strided and interleaved
arrays are fetched by the hardware directly* — no per-draw repacking, as long
as the array memory is GPU-addressable. The vendor GL ICD never exercises any
of this because a GL client array lives in ordinary user memory with no GPU
address, so it falls back to a CPU de-index-and-copy path; that fallback is
the only thing our capture corpus contains, which is why we had never seen an
indexed packet. Every encoding below was then re-derived independently from
the Windows D3D display driver (§9) and confirmed a third time from the GL
ICD's own client side (§10) — three separate codebases writing the same
constants, including the fiddly 16-bit index address arithmetic. The one
thing they do *not* agree on is what an attribute's dword offset means once a
second stream is bound (§3.4), so the recommended first implementation binds
a single interleaved stream, where the two readings coincide.

---

## 1. Draw packet opcode map

Every draw packet header is

```
header = draw_mode | ctx_flags | (dwordsPerVertex-1)<<17 | low16
```

with the opcode byte `(header >> 24) & 0xff` decomposing as

| bits | field |
|---|---|
| [31] | set on every client(GL/D3D)-path draw; clear on kernel-internal draws |
| [30] | vertex-layout flag (`ctx+0x42c`, `0x40000000` when layout byte1 & 0x10) |
| [28:26] | **draw mode** (table below) |
| [25:24] | `0b11` — the draw class; constant `0x03000000` OR'd in by every builder |
| [22:17] | dwords per vertex − 1 (6 bits ⇒ 1..64 dwords/vertex). **Zero on all index packets.** |
| [15:0] | see per-mode table |

### Draw modes [BIN]

| mode | opcode byte | builder (`.text`) | packet | operands |
|---|---|---|---|---|
| 0 | `0x03` | `mtx3063` @`0x6d80` | header + inline vertex dwords | — |
| 1 | `0x07` | `mtx1767` @`0x7b70`, `mtx1796` @`0x74d0` | header + inline **16-bit** indices | — |
| 2 | `0x0b` | `mtx2103` @`0x5f10`, `mtx3311` @`0xa3a0` | header + inline **32-bit** indices | — |
| 3 | `0x0f` | `mtx3981` @`0x5d30` | 3 dwords | first vertex index, **exclusive** end index — **draw from the bound streams, no data in the packet** |
| 4 | `0x13` | — | — | not emitted by any builder; reserved |
| 5 | `0x17` | `mtx1017` @`0x60e0` | 3 dwords | vertex bytes `start`, `end` (bus addresses) — the familiar `0x97080001` |
| 6 | `0x1b` | `mtx1443` @`0x5db0`, `mtx3249` @`0x5e30` | 3 dwords | **16-bit index array** `start`, `end` |
| 7 | `0x1f` | same pair | 3 dwords | **32-bit index array** `start`, `end` |

Bit 28 means **"the two operands are bus addresses"**: modes 5, 6 and 7 carry
`start`/`end` addresses of vertex bytes, 16-bit indices and 32-bit indices
respectively, and mode 4 is the unused fourth slot of that group. Modes 0–2
carry their payload inline behind the header. Mode 3 is the outlier — three
dwords like the address group, but its operands are plain vertex indices into
the already-bound streams, not addresses. Bits [27:26] are therefore not a
single consistent element-kind enum across both halves (inline vertices are
`00`, address-mode vertices are `01`); treat the mode as the flat 0..7 value.
**[BIN]**

**Evidence.** The mode bits are not hard-coded per builder; they arrive from
the client. `ctx+0x428` is written with exactly four values in the module:

| value | site | meaning |
|---|---|---|
| `0x04000000` | `mtx1569` @`0x48d6c` | inline 16-bit indices |
| `0x08000000` | `mtx1569` @`0x492d4` | inline 32-bit indices |
| `0x18000000` | `mtx2661` @`0xbfc2` | 16-bit indices by address |
| `0x1c000000` | `mtx2661` @`0xc007` | 32-bit indices by address |

and `mtx2147` @`0x4b452` copies it **verbatim from the client draw record's
dword 0** (`mov 0x0(%r13),%eax ; mov %eax,0x428(%rbx)`), immediately before
`call *0x460(%rbx)`. Builders `mtx1443`/`mtx3249`/`mtx2103`/`mtx3311` all OR
`ctx+0x428` into `0x03000000`; `mtx1017` hard-codes `0x17000001`, `mtx3981`
hard-codes `0x0f000001`, `mtx3063` uses `0x03000000` alone. **[BIN]**

---

## 2. The four index packets, exactly

### 2.1 Inline 16-bit indices — opcode `0x07`

`mtx1767` @`0x7b70` **[BIN]**:

```
7bef:  cmp  $0xffff,%r12d     ; n > 65535 -> fall back to mtx1796 (splits)
7bf8:  lea  0x3(%rsi),%rax    ; rsi = 2*n bytes
7c0a:  shr  $0x2,%r14         ; r14 = dataDwords = ceil(2n/4)
7c1f:  lea  -0x1(%r12),%eax
7c24:  and  $0xffff,%eax      ; low16 = n - 1
7c29:  or   0x42c(%rbx),%eax
7c2f:  or   $0x7000000,%eax
7c34:  mov  %eax,(%r8)        ; header
       LocMemCpy(r8+4, src, 2*n)
```

* packet = `1 + ceil(2n/4)` dwords;
* **`low16 = nIndices - 1`, not `totalDwords - 2`** — index packets are the
  one exception to the usual framing rule;
* indices are a straight byte copy: two 16-bit indices per dword, first index
  in the low halfword, tail padded to a dword boundary. The alignment dance
  at `0x7c92..0x7d72` copies 1–3 leading bytes by hand purely to align the
  CPU-side source for `LocMemCpy`; it is not a hardware requirement.
* max **65535** indices per packet.

### 2.2 Inline 32-bit indices — opcode `0x0b`

`mtx2103` @`0x5f10` **[BIN]**:

```
5f28:  lea  0x1(%r12),%eax     ; reserve n+1 dwords
5f49:  lea  -0x1(%r12),%eax
5f55:  and  $0xffff,%eax       ; low16 = n - 1
5f5a:  or   0x428(%rbp),%eax   ; 0x08000000
5f64:  or   0x42c(%rbp),%eax
5f6a:  or   $0x3000000,%eax
5f96:  lea  0x0(,%r12,4),%edx  ; LocMemCpy of n*4 bytes right after the header
```

One index per dword. Same 65536 ceiling from the 16-bit `low16`.
`mtx3311` @`0xa3a0` is the same builder with the ring kick inlined.

### 2.3 Address-mode indices — opcodes `0x1b` / `0x1f`

`mtx1443` @`0x5db0` is a bare three-dword emitter **[BIN]**:

```
5dc4:  call mtx2661              ; fills the two stack dwords: start, end
5de1:  mov  0x428(%rbp),%eax     ; 0x18000000 or 0x1c000000
5de7:  or   $0x1,%eax            ; low16 = 1  (= totalDwords - 2)
5dea:  or   0x42c(%rbp),%eax
5df0:  or   $0x3000000,%eax
5df5:  mov  %eax,(%rdx)          ; header
5df7:  mov  (%rsp),%eax  -> +4   ; start
5dfd:  mov  0x4(%rsp),%eax -> +8 ; end
```

So the packet is literally `[0x9b000001 | flags, start, end]` (16-bit) or
`[0x9f000001 | flags, start, end]` (32-bit). `mtx3249` @`0x5e30` is the same
plus an immediate submit. **There is no index count in the packet** — the
count is `(end - start) / indexSize`.

#### Operand construction — `mtx2661` @`0xbf50` [BIN]

```
bf7c:  eax = mtx2320(indexDesc)      ; GPU base address OR memory class
bf81:  edx = indexDesc[0x08]         ; index element size in BYTES: 2 or 4
bf84:  eax &= ~3
bf87:  eax += ctx[0x250]             ; byte offset of the first index
       ; A = (gpuBase & ~3) + byteOffset
```

**32-bit path** (`edx == 4`, `0xc007`):

```
start = (gpuBase & ~3) + byteOffset
end   = mtx2320(desc) + (byteOffset & ~3) + 4*n
      = start + 4*n + memclass
```

**16-bit path** (`edx == 2`, `0xbfc0`), with `A` as above and `n` indices:

| `A & 3` | `n` | `start` | `end` |
|---|---|---|---|
| 0 | even | `A` | `A + 2n` |
| 0 | odd | `A \| 1` | `A + 2n + 2` |
| 2 | even | `A \| 1` | `A + 2n + 2` |
| 2 | odd | `A` | `A + 2n` |

and finally `end |= mtx2320(desc) & 3` (`0xbfe5..0xbff0`).

**There is no index-buffer base register and no index-count register.** The
whole index binding travels in the packet's two operands. `mtx3355` is the
only emitter of the vertex-pipeline register tags and it emits exactly
`0x00..0x07` (stream bases), `0x10..0x13` (strides), `0x18`/`0x19`
(`0x1060`/`0x1064`), `0x2c` (`0x10b0`) and `0x2d`/`0x2e`
(`0x10b4`/`0x10b8`) — nothing else. **[BIN]**

Arithmetic on the four rows shows `end` is *always* a multiple of 4, so its
two low bits are free — and they carry the **memory class**, exactly as the
class rides on `end` for the secondary-call packet and on `0x1000` for the
vertex streams. `start`'s two low bits are therefore not a class:

| start bit | meaning | derivation |
|---|---|---|
| 1 | the **first** halfword of the first dword is not an index | `A & 2`, i.e. `byteOffset & 2` |
| 0 | the **last** halfword of the last dword is not an index | set exactly when `(n + (A&2)/2)` is odd, i.e. when the run ends mid-dword and `end` had to be rounded up |

**[BIN]** for the arithmetic, **[INF]** for the halfword-skip naming — it is
the only reading in which the four rows describe one consistent rule.

For 32-bit indices both low bits of `start` are 0 (the offset must be a
multiple of 4).

#### Where `mtx2320` gets the address — `.text+0x1bdf0` [BIN]

```
kind = desc[0x14]
kind 0: obj = desc[0x18];  return obj[0x90]            ; GPU address | class
        (special object flavour: [obj+0x1c8][0x28] -> mem[0x34] | mem[0x48])
kind 2: return desc[0x28]                              ; raw 32-bit GPU address from the client
kind 1: return 0                                       ; user memory, no GPU address
```

`mem[0x34] | mem[0x48]` is a base OR'd with a class word — the same
`base | class` encoding as `0x1000`.

### 2.4 Index-buffer residency decides inline vs address — `mtx2218` @`0xc030` [BIN]

`mtx2218` binds the index (element-array) buffer from the client request's
`[rsi+0xae]` kind and fills the descriptor block at `ctx+0x198`:

| request kind | `ctx[0x198]` | `+0x14` | payload | index size at `+0x08` |
|---|---|---|---|---|
| 0 — buffer object | `((obj[0x1ad] & 0xf) != 1) ? 2 : 1` (`0xc1e4..0xc1f9`) | 0 | `+0x18` = object pointer, `+0x04` = `obj[0x1e8]` (size) | `[rsi+0xb6]` |
| 1 — raw GPU address | **2** | 2 | `+0x28` = 32-bit GPU address | `[rsi+0xba]` |
| 2 — user pointer | 1 | 1 | `+0x20` = 64-bit user pointer | `[rsi+0xba]` |
| none | 0 | 2 | — | 0 |

`obj[0x1ad] & 0xf` is the object's memory kind, and `1` is the same "user
memory, no GPU address" value that clears `ctx[0x1cc]` for vertex streams
(`mtx2714` @`0xb794`). So the rule for both index and vertex data is one
rule: **GPU-addressable ⇒ the hardware fetches it; user memory ⇒ the CPU
copies it.**

and then, at `0xc0a1`:

```
ctx[0x458] = (ctx[0x198] == 2) ? mtx1067 : mtx1060
```

* `mtx1067 = { mtx2841, mtx2855 }`, `mtx2855` = 13 × `mtx1443` → **address-mode
  index packets**;
* `mtx1060 = { mtx2841, mtx1147 }`, `mtx1147` = `mtx1796`/`mtx1767` per
  primitive → **inline index packets**;
* `mtx2841` = 13 × `mtx1017` — entry 0 of *every* table, taken when
  `ctx[0x1cc] == 0`, i.e. when some vertex stream is not GPU-addressable.
  `mtx1017` reads the indices itself (`movzwl (%rdx),%eax` at `0x6908` and
  `0x6c4b`) and copies the *dereferenced vertices* into the pushbuffer as a
  plain `0x17` packet. **This CPU de-index fallback is what the whole
  `captures/gl-matrix` corpus shows.**

The outer index of these tables is `ctx[0x1cc]` (0 = copy, 1 = direct fetch)
and the inner index is the primitive; `mtx3712` @`0x18ead` does the double
dereference into `ctx+0x460` (generic draw builder) and `ctx+0x468` (index
draw builder). **[BIN]**

---

## 3. Vertex streams: base + stride, fetched by the hardware

### 3.1 Eight streams, eight strides — `mtx3355` @`0x417a0` [BIN]

The state emitter builds one 16-dword register-write packet:

```
0x0000000e            ; opcode 0, low16 = 16-2
0x03020100  s[0x08] s[0x0c] s[0x10] s[0x14]    -> 0x1000 0x1004 0x1008 0x100c
0x07060504  s[0x18] s[0x1c] s[0x20] s[0x24]    -> 0x1010 0x1014 0x1018 0x101c
0x13121110  s[0x28] s[0x2c] s[0x30] s[0x34]    -> 0x1040 0x1044 0x1048 0x104c
```

`s[0x28..0x37]` is a `u16[8]` in the state block, so the four stride
registers hold **two 16-bit byte strides each — eight streams**. `mtx3712`
@`0x18e99` refuses the packet path outright when `ctx[0x1c8] > 7`
(`cmpl $0x7,0x1c8(%r12) ; ja`), confirming the ceiling of 8. **[BIN]**

The same function also emits:

```
0x00000001 0x2121212c s[0x84]                  -> 0x10b0  primitive word
0x00000002 0x21211819 s[0x38] s[0x3c]          -> 0x1064, 0x1060
0x00000001 0x2121212d 0x00000000               -> 0x10b4  attr write index = 0
0x00000004 0x2e2e2e2e d0 d1 d2 d3   (repeated) -> 0x10b8  attribute descriptors
```

### 3.2 Base and stride — `mtx1035` @`0xb940` [BIN]

Per bound stream slot (`ctx+0x1d0 + 16*i`, count `ctx+0x1c8`):

```
buf     = slot[0x00]                     ; buffer record
regIdx  = slot[0x08]                     ; stream number 0..7
offset  = slot[0x0c]                     ; byte offset of the array inside the buffer
stride  = ctx[0x404] ? ctx[0x404] : buf[0x08]     ; 16-bit BYTE stride
size    = buf[0x04]                      ; buffer size in bytes

first   = ceil(-offset / stride)   (0 when offset > 0)
count   = (size - offset) / stride

ctx[0x254] = max over streams of first     -> 0x1064  (fetch floor)
ctx[0x258] = min over streams of count     -> 0x1060  (fetch limit)
```

and then, on the **direct-fetch** path (`0xbaf0`):

```
s[0x08 + 4*regIdx]        = mtx2320(buf) + (offset & ~3)   -> 0x1000 + 4*regIdx
s[0x28 + 2*regIdx] (u16)  = stride                          -> 0x1040 stride slot
```

versus the **copy** path (`0xba6d`): `s[0x08] = 2`, `s[0x28] = 4` — the
constant placeholders the capture shows, because the per-vertex size then
travels in the header's bits [22:17] instead.

**So yes: the hardware fetches strided, interleaved arrays directly.** Base
is `gpuAddress | memoryClass` (low 2 bits, `0` local VRAM / `2` AGP /
`3` PCI-system), stride is a plain 16-bit byte stride, and `0x1060`/`0x1064`
clamp the vertex index the fetch unit is allowed to use. No repacking, no
"registration" — a system-memory vertex array at any bus-physical address
below 4 GiB works under class 3.

### 3.3 Attribute descriptors `0x10b4` / `0x10b8` — `mtx2714` @`0xb6f0` [BIN]

`0x10b4` is the write index (reset to 0), `0x10b8` the data port, one dword
per attribute, written four per register-write packet. The client hands the
kernel one **4-byte record `r[0..3]` per attribute** and a 5-bit count
(`[rsi+0x10c][0] & 0x1f` at `0xb724`) — **at most 31 attributes**.

| dword bits | source in the record | field | evidence |
|---|---|---|---|
| [2:0] | `r[1] & 7` | **stream index** (0..7) | `0xb737`, merged at `0xb7b1` |
| [11:4] | `r[0]` | **offset within the vertex, in DWORDS** (0..255) | `0xb7c1` `movzbw`, `shl $4`, mask `0xf00f` |
| [15:12] | `(r[1] >> 3) & 0xf` | reserved — **0 in every descriptor ever captured** | `0xb7db..0xb7f1`, mask `0xf` |
| [16] | `r[1] >> 7` | **1 on every non-position attribute**, 0 on the position | `0xb7f6..0xb816` |
| [19:17] | `r[2] & 7` | **attribute semantic**: 0 position, 1 normal, 2 colour, 3 texcoord 0 | same |
| [21:20] | — | 0 | |
| [25:22] | `(r[2] >> 3) & 0xf` | **component mask** — `xy`=`0b0011`, `xyz`=`0b0111`, `xyzw`=`0b1111` | `0xb81b`, mask `0xfc3f` |
| [26] | `r[2] >> 7` | ? — 0 throughout | `0xb83c` |
| [29:27] | `r[3] & 7` | **origin**: `4` on every ICD attribute, `0` on kernel-internal and ICD-synthesised ones | `0xb83c..0xb863`, mask `0x43` |
| [31] | — | **last attribute** | `orb $0x80,0x47(%r9,%rax,4)` at `0xb87b` |

The earlier reading of `[16:12]` as one 5-bit "destination slot" field is
wrong. It is arithmetically equivalent on the captured data — `[15:12]` never
leaves 0, so a 5-bit read just yields 0 or 16 — but it hides the structure:
the nibble `[19:16]` takes exactly 0, 3, 5 and 7, and those decompose as
`bit16 = (not the position)` plus a 3-bit semantic `[19:17] ∈ {0,1,2,3}`.
Three independent lines of evidence agree on the split:

* **The kernel's own packing** takes bit 16 from `r[1]` and bits [19:17] from
  `r[2]` — two different record bytes, so they are two different fields. The
  ICD's `r[1]` also carries the stream in its low 3 bits and `r[2]` also
  carries the component mask in its bits [6:3]; on that reading `r[2]` is
  cleanly `semantic | mask<<3` and `r[1]` is `stream | flag<<7`. **[BIN]**
* **The ICD's declaration slot enumeration** (`libGL.so.1.3.0` `0xd15d0`)
  numbers its attributes 0 = position, 1 = normal, 2 = primary colour,
  4 = secondary colour, 7 = fog, 8..11 = texcoord 0..3. The first three land
  on `[19:17]` unchanged. **[BIN]**
* **The corpus** (below) correlates each value with a component count that
  only makes sense for that semantic. **[HW]**

`docs/parhelia-warp-binding.md` reads `[19:16]` as a WARP input register
index; that is the same four values seen from the program's side, and the two
readings are not in conflict — the semantic is what selects the input port.

### Every descriptor in the capture corpus, decoded [HW]

Extracted by walking all 37 `captures/gl-matrix/*-secs.txt` streams, decoding
opcode-0 register-write packets and collecting the writes to `0x10b8`. These
five distinct descriptors are the complete set:

| descriptor | stream | offset (dw) | `[19:16]` | semantic `[19:17]` | mask | origin | last | seen in |
|---|---|---|---|---|---|---|---|---|
| `0x20c00000` | 0 | 0 | 0 | 0 position | `xy` | 4 | no | every variant |
| `0xa1c50020` | 0 | 2 | 5 | 2 **colour** | `xyz` | 4 | yes | untextured variants (5-dword vertex) |
| `0x21c50020` | 0 | 2 | 5 | 2 **colour** | `xyz` | 4 | no | textured variants (7-dword vertex) |
| `0xa0c70050` | 0 | 5 | 7 | 3 **texcoord 0** | `xy` | 4 | yes | textured variants |
| `0xa1c30020` | 0 | 2 | 3 | 1 **normal** | `xyz` | 4 | yes | `light` only |
| `0x03c00000` | 0 | 0 | 0 | 0 position | `xyzw` | 0 | no | `linesmooth` / `lwidth` / `psize` |
| `0x83c50040` | 0 | 4 | 5 | 2 colour | `xyzw` | 0 | yes | the same three (8-dword vertex) |
| `0x83c00000` | 0 | 0 | 0 | 0 position | `xyzw` | 0 | yes | the kernel's own internal draws |

Two independent cross-checks fall out of this table and neither has an
exception:

1. **The masks add up to the draw header standing next to them.**
   `xy(2) + xyz(3)` = 5 dwords = `0x97080001`; `xy + xyz + xy` = 7 =
   `0x970c0001`; `xyzw + xyzw` = 8 = `0x970e0001`. So `[25:22]` is a mask —
   not a count code — and its population count is the number of consecutive
   floats fetched from the given dword offset.
2. **The semantic matches what the variant is actually drawing.** The `light`
   capture is the only one that turns on lighting, and it is the only one
   whose second attribute is `[19:17] = 1` with a 3-component mask — a
   normal. Every ordinary variant has `[19:17] = 2` with 3 components — an
   RGB colour. The four `tex*` variants add a third attribute with
   `[19:17] = 3` and 2 components — a texcoord. The three line/point-size
   variants take the expansion path, whose position and colour are both
   4-component. This is the correlation that identifies the field.

**[HW]**

The companion stream state in the same corpus is uniformly the copy-path
placeholder — `0x1000` ∈ {2, 3}, `0x1040` = 4 (64246 writes, no other
value), `0x1044` = 0, `0x1064` ∈ {0, 0xffffffff}, `0x1060` = the pushbuffer
capacity in vertices (`0x280cc`, `0x1c9b6`, `0x19080`). **No capture ever
programmed a real stride**, which is the corpus-level restatement of §4.
**[HW]**

Two more facts fall out of `mtx2714`:

* **stream discovery** — attributes are walked in order; whenever `r[1]&7`
  differs from the previous attribute's stream, a new stream slot is appended
  at `ctx+0x1d0 + 16*i` with `slot[0x08] = streamIndex` and
  `slot[0x00] = ctx + 48*stream + 0x18` (the buffer record). So *attributes
  select their stream, and the stream set is whatever the attributes name*.
* **`ctx[0x1cc]`** starts at 1 and is cleared (`0xb78b..0xb7a2`) as soon as
  any named stream's buffer record has kind `1` (user memory, no GPU
  address). That single bit is what forces the whole draw onto the
  CPU-de-index-and-copy `mtx1017` path.

A second pass at `0xb888` (taken only when `ctx[0x1cc] == 0`) rebases every
attribute onto stream 0 with an accumulated offset — i.e. the copy path
flattens all streams into one tightly packed interleaved stream. That is the
layout our captures show, and it is *not* what the hardware requires.

---

### 3.4 Multi-stream offsets — the one place the two vendor drivers differ

`mtx2714`'s **second pass** (`0xb888..0xb92f`) is gated on
`cmpb $0x0,0x1cc(%rdi) ; jne` — it runs **only on the copy path**. What it
does there:

```
for each attribute a:
    sum = 0
    for each bound stream slot s in order, until slot[0x08] == a.stream:
        sum += (ctx[0x404] ? ctx[0x404] : slot->buf[0x08]) >> 2   ; stride in dwords
    a.offset = (a.offset + sum) & 0xff        ; b900..b91f, bits [11:4]
    a.stream = 0                              ; andb $0xf8 at b925
```

i.e. it flattens every stream into stream 0 at a running dword offset —
correct, because on the copy path `mtx1017` really has concatenated all the
streams into one packed vertex in the pushbuffer.

**On the direct-fetch path this pass does not run**: descriptors keep their
own `bits[2:0]` stream and their own intra-stream dword offset. **[BIN]**

The Windows display driver `MTXPARD.dll` has an analogous accumulation at
VMA `0x12de70` which, per an independent read, rewrites `bits[11:4]` to a
global dword offset across the concatenated streams **while leaving
`bits[2:0]` naming the stream**. If that read is right, the hardware forms
one logical vertex by concatenating one stride's worth from each bound stream
and the descriptor offset indexes that concatenation — which is *not* the
Linux direct-fetch reading.

**This is the one open question that can change a multi-stream draw from
working to silent.** It cannot be settled from the binaries; the two vendor
stacks disagree. Mitigation, and the recommended first step: **bind a single
stream**. With one stream the two readings coincide exactly (there is no
preceding stream to accumulate), so a single interleaved buffer is
unambiguous. Only after that draws should a second stream be tried, first
with the Linux reading (per-stream offsets), then with the global one.

---

## 4. Capture evidence: the vendor GL never emits an indexed packet

Every dword in `captures/gl-matrix/*-secs.txt` whose top byte matches a draw
opcode was enumerated. Only three draw headers exist in the whole corpus
**[HW]**:

| header | count | decode |
|---|---|---|
| `0x97080001` | 28673 | mode 5 (`0x17`), 5 dwords/vertex |
| `0x970c0001` | 7946 | mode 5, 7 dwords/vertex |
| `0x970e0001` | 1536 | mode 5, 8 dwords/vertex |
| `0xd7080001` | 775 | the same with layout bit 30 set |

No `0x87`/`0x8b`/`0x8f`/`0x9b`/`0x9f` header appears anywhere. This is the
expected consequence of §3.3: an OpenGL client array is plain user memory,
`ctx[0x1cc]` goes to 0, and every draw — indexed or not — is de-indexed and
copied by `mtx1017`. **The absence of indexed packets in the corpus is
evidence about the ICD, not about the hardware.**

---

## 5. Limits and alignment

| question | answer | evidence |
|---|---|---|
| indices per **inline** packet | ≤ 65535 (`low16 = n-1`); `mtx1767` @`0x7bef` rejects `n > 0xffff` and hands off to `mtx1796`, which splits at `0xffff` **rounded down to the primitive's index granularity** and, for strips/fans, re-emits the trailing 1–2 indices at the head of the next packet | [BIN] `0x769e..0x76b8`, `0x7a22`, `0x7a52`, `0x7970` |
| index granularity per primitive | from the jump table at `.rodata+0x338` (13 entries, indexed by `ctx+0x41c`): points 1, lines 2, line loop 1 + restart, triangles **3**, tri-strip 1 + restart, fan/polygon 1 + restart (3 per primitive), quads/quad-strip handled by the caller. Triangle lists therefore split at 65535, which is already a multiple of 3 | [BIN] |
| indices per **address-mode** packet | no packet-level limit — the count is `(end-start)/size` over 32-bit addresses | [BIN] `mtx1443` |
| vertices per **inline vertex** packet | `low16 = nVerts*dwPerVertex - 1` ⇒ ≤ 65536 dwords of vertex data | [BIN] `mtx3063` @`0x7087` |
| vertices per **`0x17`** packet | bounded only by pushbuffer space; `mtx1017` chunks and rounds down to a whole primitive (lists /1, lines /2, triangles /3, quads /4) | [BIN] |
| dwords per vertex | 1..64 (6-bit field `[22:17]`) | [BIN] mask `0x7e0000` |
| vertex streams | 8 | [BIN] `mtx3355`, `mtx3712` @`0x18e99` |
| attributes | ≤ 31 (5-bit count) | [BIN] `mtx2714` @`0xb724` |
| attribute offset within a vertex | ≤ 255 dwords (1020 bytes) | [BIN] 8-bit field |
| vertex stride | ≤ 65535 bytes (16-bit field) | [BIN] `mtx1035`, `mtx3355` |
| addressable vertices | `0x1060` = min over streams of `(size-offset)/stride`; `0x1064` = first legal index | [BIN] `mtx1035` |
| **index buffer alignment** | base is masked `& ~3` ⇒ **4-byte aligned**. 16-bit indices may start on any halfword (`start` bit 1 says so); 32-bit indices need the byte offset to be a multiple of 4 | [BIN] `mtx2661` @`0xbf84`, `0xc01f` |
| **vertex buffer alignment** | base + `(offset & ~3)` ⇒ 4-byte aligned | [BIN] `mtx1035` @`0xbb0e` |
| index buffer residency | any memory class: 0 local VRAM, 2 AGP, 3 PCI/system bus-physical; the class rides in the low 2 bits of the packet's **end** operand | [BIN] `mtx2320`, `mtx2661` |

`0x1090`/`0x1094` are **not** part of the vertex or index fetch path. They are
written under tag `0x21212425` inside `mtx1017` as `0x1090 = ++sequence`
(0 skipped) and `0x1094 = channelId`, a fence/write-back tag emitted
immediately before the ring kick — see `docs/parhelia-draw-builders-kernel.md`
§"Around the draw". Nothing in `mtx1035`, `mtx2714`, `mtx2661` or `mtx3355`
touches them.

---

## 6. What this means for ioquake3

ioquake3's OpenGL1 renderer draws exclusively with
`glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, indexes)` over
`glLockArraysEXT`-locked client arrays: position (3 float), colour (4
`GL_UNSIGNED_BYTE`), and up to two texcoord sets (2 float), each a separate
array with its own pointer and stride.

The natural mapping is therefore:

1. **Allocate the locked vertex arrays and the index array in GPU-addressable
   memory** — a class-3 `GFP_DMA32` system buffer is enough (no registration,
   see `docs/parhelia-vertex-fetch-and-memory-classes.md` §1.7). On
   `glLockArraysEXT` copy the client arrays in once; on `glUnlockArraysEXT`
   drop them. This is the single decision that flips `ctx[0x1cc]` and unlocks
   the whole indexed path.
2. **Start with ONE stream**, not four. §3.4 leaves a genuine ambiguity about
   what the descriptor's dword offset means once a second stream is bound, and
   a single interleaved stream makes the two candidate readings identical.
   So: `0x1000 = bus | 3`, stride slot 0 = the interleaved vertex size in
   bytes, and one `glLockArraysEXT`-time pass that interleaves ioquake3's
   `xyz[]`, `color[]`, `texCoords[0][]`, `texCoords[1][]` into that buffer.
   The colour array has to be expanded to floats anyway (§8), so the pass is
   not extra work — it is the same pass.
   Once a single-stream indexed draw draws, try four streams
   (`0x1000 + 4*s = bus_s | 3`, stride slot `s` = that array's byte stride) to
   remove the interleave; the hardware is built for it and the vendor's own
   D3D driver uses it.
   Either way set `0x1064 = 0` and `0x1060 = min over streams of
   (bytes / stride)`. Both are stream-binding state, not per-draw state — the
   Windows driver recomputes them only when a stream binding changes
   (`0x12df80`, dirty bit 9).
3. **One `0x10b8` descriptor per attribute**: `bits[2:0]` = its stream,
   `bits[11:4]` = its dword offset inside the interleaved vertex,
   `bit 16` = 1 for everything but the position, `bits[19:17]` = the semantic
   (0 position, 1 normal, **2 colour**, **3 texcoord 0**), `bits[25:22]` = the
   component mask, `bits[29:27] = 4`, and bit 31 on the last one. Reset
   `0x10b4 = 0` first; descriptors go out four per register-write packet under
   tag `0x2e2e2e2e`. For ioquake3's single-texture pass that is exactly
   `0x20c00000` (position `xy`… or `0x23c00000` for `xyzw`),
   `0x2?c?0020`-shaped colour and texcoord entries — build them from the
   fields rather than copying the captured constants, since the offsets
   differ.
   ioquake3 wants a second texcoord for its multitexture path; texture unit 1
   is presumably semantic 4, but only unit 0 was ever captured — verify before
   relying on it.
4. **Emit `0x9f000001, start, end`** — mode 7, 32-bit indices by address —
   with `start` = index array bus address (4-byte aligned) and
   `end = start + 4*nIndices | 3`. `SHADER_MAX_INDEXES` = 98304 needs **no
   splitting** in this mode; the 65535 ceiling only applies to the inline
   modes.
5. `0x10b0` gets the triangle-list primitive word `0x0002305c`
   (see `docs/parhelia-draw-builders-kernel.md`).

If `GL_UNSIGNED_SHORT` is ever preferable (ioquake3 can be built with a
16-bit `glIndex_t`), use mode 6 and set `start` bit 1 when the array starts on
an odd halfword and bit 0 when the index run ends mid-dword.

**There is no base-vertex / index-bias field anywhere** — not in the packet,
not in a register. `0x1064` is a *floor* the fetch unit clamps against, not a
bias that is added. Both vendor drivers express the concept as address
arithmetic instead:

* a non-zero **first index** advances the index-packet `start`
  (`ctx[0x250]` in `mtx2661`; `MTXPARD.dll` `0x12df60` computes
  `offset + indexSize * StartIndex` for exactly this);
* a non-zero **base vertex** advances each stream's base
  (`mtx1035` @`0xbb0e` adds the array's byte offset into `0x1000 + 4*s`;
  `MTXPARD.dll` `0x12def0` computes
  `byteOffset[i] + BaseVertexIndex[i] * stride[i]`).

D3D *has* a `BaseVertexIndex` in its DDI and the vendor driver still folds it
into an address, which settles the question. **[BIN]×2**

---

## 7. PROVEN vs INFERRED

**PROVEN [BIN] — read directly from instructions**

* The eight draw modes and their opcode bytes, and that the mode field is
  supplied by the client in draw-record dword 0.
* The three-dword framing of modes 3/5/6/7 and the `low16 = 1`.
* `low16 = nIndices - 1` for the inline index modes, and the 65535 ceiling.
* `mtx2661`'s complete start/end arithmetic for both index widths.
* `mtx2320` returns `gpuAddress | memoryClass`.
* Eight streams, `0x1000..0x101c` bases and `0x1040..0x104c` 16-bit byte
  strides; the `> 7` refusal in `mtx3712`.
* `0x1060`/`0x1064` = fetch limit / fetch floor and their formulae.
* The complete `0x10b8` bit layout and the 31-attribute ceiling.
* `ctx[0x1cc]` semantics and the `mtx1017` de-index-and-copy fallback.
* `mtx2218`'s index-buffer residency kinds and the resulting builder choice.

**PROVEN [HW] — from the capture corpus**

* Only `0x97080001`/`0x970c0001`/`0x970e0001`/`0xd7080001` draw headers exist
  in `captures/gl-matrix`; no index packet was ever emitted by the vendor GL.
* The eight distinct `0x10b8` descriptors and their decode, cross-checked
  against the `dwordsPerVertex` in the draw header standing next to them.
* `0x1040` = 4 and `0x1000` ∈ {2,3} in every one of 64246 / 64365 writes —
  the corpus never leaves the copy path.

**INFERRED [INF]**

* The naming of `start` bits [1:0] in mode 6 as first/last halfword skips.
  The arithmetic is proven; only the hardware's name for it is a reading.
* That mode 3's `first` operand can be non-zero. `mtx3981` hard-codes `0` and
  the ICD folds `glDrawArrays`' `first` into the stream offsets instead, so
  the hardware's tolerance for a non-zero first operand is untested — even
  though the ICD's own record carries `(first, first+count)`.
* `bits[29:27] = 4` as an "origin" marker. Its *position* is proven; that it
  distinguishes client-declared from driver-synthesised attributes is read
  off the correlation (ICD attributes 4, kernel and line/point-expansion
  attributes 0).
* That mode 4 (`0x13`) is reserved — it is simply never emitted.
* That descriptor bits [15:12] are unused. They are always 0 in the corpus
  and the kernel sources them from the same record byte as bit 16.

**CORROBORATED — the same fact read independently out of a second codebase**

From the OpenGL ICD `libGL.so.1.3.0` (§10): the mode constants
`0x18000000`/`0x1c000000` written literally into the client draw record and
selected by `GL_UNSIGNED_INT`; the mode-3 value `0x0c000000` for a
non-indexed draw, which is what turns §1's mode-3 row from inference into
fact; the attribute offset being in **dwords**; the declaration slot
enumeration that identifies descriptor `bits[19:17]`; per-array base+stride
direct fetch implemented but gated on a "GPU-addressable" byte; the stream
count being a device capability; and — from `glDrawRangeElements` throwing
its `start`/`end` away — that no per-draw index range reaches the hardware.

**CORROBORATED — and a third time out of `MTXPARD.dll`**

The header formula, all seven emitted draw modes with their literal
constants, the three-dword framing of the address modes, `low16 = nIndices-1`
for the inline index modes with a zero count field, the 65535 ceiling rounded
to a whole primitive, the 6-bit `dwordsPerVertex`, the eight stream bases and
four packed stride registers in one 16-dword packet, `0x1060`/`0x1064` as
limit/floor under tag `0x21211819`, the `0x10b0`/`0x10b4`/`0x10b8` roles and
their tags, the memory class in the low 2 bits of a base, and — the strongest
single item — the four-case 16-bit index `start`/`end` table of §2.3,
reproduced instruction for instruction at a different VMA in a different
compiler's output. See §9.

**STILL OPEN AFTER BOTH DRIVERS**

* Whether the descriptor's dword offset is per-stream or global across
  concatenated streams when more than one stream is bound (§3.4). The two
  drivers appear to disagree; only hardware can settle it.
* Whether a packed-ubyte4 (or any non-float) attribute format exists (§8,
  §10.5). The ICD has a 4-byte packed-colour format code and the Windows FVF
  decoder a matching component-count table, but neither could be traced into
  a descriptor. **One capture of a `glColor4ub` app settles it.**
* The meaning of header bit 30. All three stacks derive it from the vertex
  declaration/layout and none reveals what the hardware does with it.
* Mode 4 (`0x13`): no stack emits it.
* Semantic values above 3 in descriptor `bits[19:17]` — the ICD numbers
  secondary colour 4, fog 7 and texcoord 0..3 as 8..11, which cannot all fit
  in three bits, so there is a compression the corpus never exercised. Only
  position, normal, colour and texcoord 0 are confirmed.

---

## 8. The attribute *type* field — probably absent, not certainly

Worth stating explicitly, because it decides one line of the ioquake3 port:
**no component-type field has been identified** in the descriptor. The fields
that are identified are stream, dword offset, a non-position flag, the
attribute semantic, the component mask, an origin marker and the last flag.
Bits [15:12], [21:20] and [26] are zero in every descriptor the vendor ever
emitted, and `mtx2714` sources them from record bits the ICD always leaves
clear. The component mask counts *components*, and the offset is expressed in
*dwords* — a granularity that cannot address a 1- or 2-byte attribute at all.

The reading *from the corpus alone* would therefore be "the vertex fetch unit
fetches 32-bit floats only" — 37 capture variants covering colours, lighting,
fog, point size, line width and four texture formats produced exactly eight
descriptors, all float-shaped.

**But that is almost certainly too strong.** §10.5 shows the vendor ICD
carries a **4-byte packed `GL_UNSIGNED_BYTE` colour** as its own colour
format code 2, one dword wide, and the Windows FVF decoder has a matching
component-count table. Neither could be traced all the way into a descriptor,
so the encoding — presumably one of `[15:12]`, `[21:20]` or `[26]`, the three
bit groups that are zero in every capture — is not identified. The corpus is
silent because every variant in it uses `glColor3f`.

**Cheap experiment that settles it**: capture the vendor GL running any app
that calls `glColor4ub` or `glColorPointer(4, GL_UNSIGNED_BYTE, …)` and
decode `0x10b8`. A descriptor with an `xyzw` mask occupying a **single dword**
proves a packed format and names the bit.

Consequence for ioquake3 today: `glVertexPointer`, `glTexCoordPointer` and
`glNormalPointer` arrays are already `GLfloat` and bind straight through with
their natural stride. `glColorPointer(4, GL_UNSIGNED_BYTE, …)` — the
`tess.svars.colors[]` array — is the **one** array to expand to four floats
during `glLockArraysEXT` until the experiment above says otherwise. That is a
single pass over the locked range, not a per-draw repack, and it leaves the
index path untouched.

---

## 9. Independent cross-check: the Windows display driver

`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` (PE32 i386 XDDM display
driver, 1.9 MB, stripped, no export directory — entry `0x104a0` is
`DrvEnableDriver`). VMAs below, ImageBase `0x10000`. This is an entirely
separate codebase from the Linux kernel module, so agreement between the two
is the strongest evidence available short of hardware.

### 9.1 The header formula, as one function

`0x2c950` is a generic header builder taking
`(payloadDwords, modeBits, dwordsPerVertex, ctxFlag)`:

```
2c950: mov 0xc(%esp),%eax     ; dwordsPerVertex
2c958: dec %eax
2c959: and $0x3f,%eax
2c95c: or  $0x180,%eax        ; 0x180 << 17 == 0x03000000
2c961: shl $0x11,%eax
2c964: dec %ecx               ; payload dwords
2c965: and $0xffff,%ecx
2c96b: or  %ecx,%eax
2c96d: or  0x8(%esp),%eax     ; mode bits
2c971: or  0x10(%esp),%eax    ; ctx flag
```

That is §1's formula written out by the vendor: base `0x03000000`, a 6-bit
`dwordsPerVertex-1` at [22:17], a 16-bit low field, plus the mode bits and the
context flag. **[BIN]**

### 9.2 Point-by-point agreement

| claim | Linux `mtx_parhelia.o` | Windows `MTXPARD.dll` |
|---|---|---|
| base opcode `0x03000000` | every builder | `or $0x180` pre-shift @`0x2c95c` |
| mode 1 = `0x04000000` (inline i16) | `mtx1569` @`0x48d6c` | `0x132feb` |
| mode 2 = `0x08000000` (inline i32) | `mtx1569` @`0x492d4` | same site |
| mode 3 = `0x0f`, operands `0`, `N` | `mtx3981` @`0x5d79` | `0x1328f4` `or $0xf000001` |
| mode 5 = `0x17000001` | `mtx1017` @`0x676d` | `0x13394b`, `0x135925` |
| mode 6 = `0x18000000` (addr i16) | `mtx2661` @`0xbfc2` | `ctx[0x378]` @`0x12e17f` |
| mode 7 = `0x1c000000` (addr i32) | `mtx2661` @`0xc007` | `ctx[0x378]` @`0x12e1ce` |
| index packet = 3 dwords, count field 0 | `mtx1443` | `0x132920`, `0x132990` |
| inline index `low16 = nIndices-1` | `mtx1767` @`0x7c24` | `0x133128` |
| inline i16 packed 2/dword, low half first | `mtx1767` memcpy | `0x133219` `shl $0x10` |
| max 65535 indices, rounded to a whole primitive | `0x7bef`, `0x769e` | `0x1330e2..0x1330f9` |
| `dwordsPerVertex ≤ 64` | mask `0x7e0000` | `and $0x3f` |
| 8 stream bases + 4 packed stride regs in one 16-dword packet | `mtx3355` @`0x41925` | flusher `0xc95b0`, dirty bit 8 |
| `0x1064` floor / `0x1060` limit, tag `0x21211819` | `mtx3355` @`0x4181c` | `0xc96e1` |
| `0x10b4` index reset (tag `0x2d`) then `0x10b8` ×4 (tag `0x2e2e2e2e`) | `mtx3355` @`0x41858`, `0x41880` | flusher dirty bit 10 |
| `0x10b0` primitive word, tag `0x2121212c` | `mtx3355` @`0x417ca` | `0xc95b0` step 1 |
| memory class in the low 2 bits of a base | `mtx2320` `[0x34]\|[0x48]` | `0x173d8b` `mov 0x28(%ecx),%eax ; or 0x1c(%ecx),%eax` |
| stream base and offset forced `& ~3` | `mtx1035` @`0xbb0e` | `0x12e06b`, `0x12e074` |

### 9.3 The 16-bit index start/end arithmetic, reproduced independently

`MTXPARD.dll` `0x12e150` computes, in the same order and with the same
special cases as `mtx2661`:

```
eax  = alloc(ib)                 ; address | class
edi  = (eax & ~3) + ctx[0x1dc]   ; class stripped, byte offset added
16-bit:  if ((edi&3)!=0) XOR (count odd):  start = edi|1 ; end = edi + 2*count + 2
         else                            :  start = edi   ; end = edi + 2*count
         end |= alloc(ib) & 3     ; the class lands on END
32-bit:  start = edi ; end = (base & ~3) + offset + 4*count
```

Two independently written drivers producing the same four-case table is what
promotes §2.3 from "the arithmetic says so" to settled. **[BIN]×2**

### 9.4 What the Windows side adds

* **`StartIndex` and `BaseVertexIndex` are folded into addresses, not
  registers.** `0x12df60`: `ctx[0x1dc] = indices_offset + indexSize *
  startIndex`. `0x12def0`: `streamOffset[i] = byteOffset[i] +
  baseVertexIndex[i] * stride[i]`, clamped to the buffer size, and that
  offset is what lands in `0x1000 + 4*s`. This is direct confirmation of
  §6's "there is no base-vertex field": D3D *has* the concept and the driver
  still expresses it as address arithmetic. **[BIN]**
* **Header bit 31 is unconditional, bit 30 is a real second flag.**
  `0x b0220`: `edi = ((decl[0] & ~0xfff) | ~0x1fff) << 18` → bit 31 always 1,
  bit 30 = bit 12 of the vertex declaration's first dword. So bit 30 is a
  property of the *vertex layout*, matching the Linux `ctx+0x42c` derivation
  (`0xc0000000` when layout byte1 & 0x10). Its meaning is still unknown.
  **[BIN]**
* **`0x1060`/`0x1064` are stream-size bounds, not per-draw min/max index** —
  `0x12df80` computes them from the *stream bindings* (running max of the
  per-stream first fetchable index, running min of `(size-offset)/stride`)
  and marks dirty bit 9. They can be set once per binding, not per draw.
  **[BIN]**
* **Descriptor packing** from a source dword `S` (`0x12dd00`):
  `dst[2:0] = S[10:8]` (stream), `dst[11:4] = S[7:0]` (dword offset),
  `dst[19:12] = S[18:11]`, `dst[29:22] = S[26:19]`, `dst[31]` set explicitly
  on the last element (`orl $0x80000000` @`0x12de33`), preserve mask
  `0x40300008` — i.e. bits 3, 20, 21 and 30 are **never written**, matching
  the corpus where [21:20] and bit 3 are always 0. The Windows routine packs
  [19:12] and [29:22] as single 8-bit moves, so it neither confirms nor
  refutes the internal boundaries; the Linux `mtx2714` split ([19:16] WARP
  register, [25:22] mask, [29:27] origin) plus the corpus decode of §3.3 is
  the finer-grained evidence. **[BIN]**
* **The FVF path was not resolved.** The FVF decoder at `0x172860` builds
  per-attribute words in a *different* layout (`bits[7:0]` dword offset,
  `bits[19:15]` a 5-bit format code, `bits[27:24]` a component mask taken
  from a 32-entry table at `0x19e7a0` = `0,1,3,7,0xf,1,…`), and the
  translation from that into the `S` words consumed by `0x12dd00` was not
  found. So D3D's `D3DFVF_DIFFUSE` (a packed `D3DCOLOR`) could not be traced
  to a descriptor, and §8's question — whether a packed-BGRA fetch format
  exists — remains open. The `0,1,3,7,0xf` head of that table is itself
  suggestive: it is a *component-count → mask* table, the same mask alphabet
  as descriptor bits [25:22]. **[BIN][INF]**
* Not found: `D3dDrawPrimitives2` itself. `DdGetDriverInfo` @`0x3e770`
  answers `GUID_D3DCallbacks3` from `*(pdev+0x6f10) + 0xa8`, so
  `D3dDrawPrimitives2 = *(blob + 0xbc)` — but the constructor at `0x86760`
  only zeroes those slots, so the pointer is installed elsewhere and the
  `D3DDP2OP_*` dispatch was not located (303 jump tables checked, none with a
  DP2-shaped bound). The packet emitters were reached bottom-up instead,
  which is why the encodings above are nonetheless solid.

---

## 10. Independent cross-check: the vendor OpenGL ICD

`matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` (ELF64,
stripped; function boundaries recovered from `.eh_frame`, class names from
the RTTI/vtable tables). This is the third independent codebase and it is the
one that decides *when* the indexed path is used at all.

### 10.1 The ICD literally names the same mode constants

`DrawElements` is `0xd3cc0..0xd3fa7` (`glDrawElements` @`0x1ab100` is a
pointer-to-member thunk onto it):

```
d3cee  rax = ctx->0xc9b0 + 0x178          ; the element-array descriptor
d3cfb  cmpb $0x0,0x24(%rax)               ; GPU-addressable? no -> software path
d3d05  cmp  $0x2,%esi                     ; GL_LINE_LOOP -> software
d3d13  call c39f0                         ; GL mode -> hw prim code, round the count
d3d77  ebx = 0x18000000
d3d7c  cmpl $0x1405,0x180(%rax)           ; GL_UNSIGNED_INT?
d3d86  cmove 0x1c000000 -> ebx
d3da9  call 1c9c50(cmdbuf, opcode=0x19, size=0x20, count=1)
```

and the 32-byte record it then fills:

| offset | value | meaning |
|---|---|---|
| `0x00` | `0x18000000` / `0x1c000000` | the draw mode bits — **the same constants, written by the client** |
| `0x04` | count | index count, after primitive rounding |
| `0x08` | prim code | hardware primitive |
| `0x10` | qword | index buffer address |
| `0x18` | dword | element-array stride field |
| `0x1c` | dword | element-array aux field |

This closes the loop with `mtx2147` @`0x4b452`, which copies dword 0 of the
client draw record straight into `ctx+0x428`, and with `mtx2661`, which
re-derives the same constant. Three codebases, one constant. **[BIN]×3**

**And it confirms mode 3.** The ICD's *non-indexed* draw (`MultiDrawArrays`
`0xd3fc0`) goes through a different record (opcode `0x18`) whose mode word is
**`0x0c000000`** — exactly `0x0f000000 - 0x03000000`, i.e. the mode-3 value
`mtx3981` hard-codes. So mode 3 is "draw a vertex range from the bound
streams, no indices", confirmed from the client side. **[BIN]**

### 10.2 The ICD's declaration, and where the descriptor fields come from

The ICD ships a **24-byte vertex declaration** (command opcode 7, emitted at
`0xc5100` from `this+0xa6d8`):

```
+0x00  u32 formatWord      ; per-attribute format code, packed bitfields
+0x04  u32 streamWord0     ; 3-bit stream index for attributes 0..7,  at bit 4k
+0x08  u32 streamWord1     ; 3-bit stream index for attributes 8..11
+0x0c  u8  offset[12]      ; attribute k's offset within the vertex, IN DWORDS; 0xff = absent
```

`offset[]` really is in dwords: `0xd1938` computes it as
`byteSize >> 2` accumulated into a running total, and the consumer `0xd2180`
multiplies by 4 to get a byte address. That is `r[0]` → descriptor
`bits[11:4]`, confirmed from the producing side. **[BIN]**

Declaration slot enumeration (from which `offset[]` byte and which stream
nibble each attribute writes):

| slot | attribute | format field |
|---|---|---|
| 0 | position (`glVertexPointer`) | `formatWord[1:0]` |
| 1 | normal | `[3:2]` |
| 2 | primary colour | `[6:4]` |
| 3 | unused by fixed function | `[7]` |
| 4 | secondary colour | `[10:8]` |
| 5, 6 | never written | — |
| 7 | fog coord | `[30:29]` |
| 8..11 | texcoord 0..3 | shifts `{0x10,0x13,0x16,0x19}` |

Slots 0, 1 and 2 are exactly the values `[19:17]` takes in the corpus for
position, normal and colour. Texcoord 0 is slot 8 in the ICD and `3` in the
descriptor, so the kernel/ICD boundary compresses the texture units into the
3-bit field; only unit 0 was ever captured. **[BIN][INF]**

### 10.3 Direct fetch vs copy — the ICD decides, per draw

`GLPrimitiveHardware` vtable slot [35] (`0xd23f0..0xd2599`) picks the path:

* it counts the streams the draw needs — one per *enabled* client array
  (vertex, colour, secondary colour, fog, normal, **edge flag**, texcoord
  0..3), plus one more for a packed constant buffer if a *disabled* array
  still has to supply its current value (`0xd24d4`);
* it ANDs each enabled array's **`desc+0x24` "GPU-addressable" byte** into a
  running flag — a single ineligible array drops the whole draw to the copy
  path. This is the ICD-side twin of the kernel's `ctx[0x1cc]`;
* `0xd24f1`: `mov 0xc9e0(%rdi),%rax ; cmp %r9d,0x14(%rax) ; jb` — **the
  maximum stream count is a device capability field**, not a compile-time 8.
  Exceeding it silently falls back to the copy path.

On the direct path (`0xd15d0..0xd2178`) each enabled array becomes its own
stream with **declaration offset 0** and the array's own base and stride:

```
strm[i].base   = desc[0x18]     ; the client pointer
strm[i].stride = desc[0x0c]     ; the client stride, in bytes
```

**So the vendor's own GL does implement per-array base+stride direct fetch.**
It just never gets to use it with ordinary client arrays, because those never
set `desc+0x24`. **[BIN]**

Client array descriptor (0x28 bytes, from its consumers):

```
+0x00 u8   enabled        +0x0c u32  stride in bytes
+0x04 u32  size           +0x18 void* base pointer
+0x08 u32  GL type enum   +0x20 u32  aux
                          +0x24 u8   GPU-addressable / hardware-fetchable
```

### 10.4 What the ICD adds about limits

* `glDrawRangeElements` (`0xd3fb0`) is an 8-byte thunk —
  `mov %r8d,%edx ; jmp 0xd3cc0` — that **throws `start` and `end` away**.
  No min/max index ever reaches the hardware, which independently confirms
  that `0x1060`/`0x1064` are stream bounds and not a per-draw range.
* **No maximum index or vertex count check exists in any draw builder.**
  `0xc39f0` only *rounds* the count down to a whole primitive (triangles /3,
  lines /2, quads /4) and rejects a draw that is below one primitive.
* 12 declaration slots; 4 texture units (`0xd1be0: cmp $0x3,%r14d`).
* The ICD 4-byte-aligns its scratch index allocations (`0xd97c6:
  add $0x3 ; and $~3`), matching the `& ~3` the kernel applies to the index
  base.
* `GL_LINE_LOOP` is always forced to the software path.

### 10.5 The lead on packed colours

The ICD's colour format codes and their byte sizes (size table at `.rodata`
`0x36fc80` = `{0,12,4,8,8,16,0,0}`, accessor `0x1ce490`):

| code | colour format | bytes |
|---|---|---|
| 0 | absent | 0 |
| 1 | 3 × float | 12 |
| **2** | **4 × `GL_UNSIGNED_BYTE`** | **4** |
| 3 | `GL_SHORT` | 8 |
| 4 | `GL_UNSIGNED_SHORT` | 8 |
| 5 | 4 × float | 16 |

selected from the array's type at `0xd17fb..0xd2166`
(`GL_UNSIGNED_BYTE → 2`, `GL_SHORT → 3`, `GL_UNSIGNED_SHORT → 4`,
`GL_FLOAT → 1` or `5`), and immediate mode does the same
(`0x158d47: or $0x20` = packed ubyte4).

So the ICD carries a **4-byte packed ubyte colour** through its declaration
and its packed vertex — one dword, not four floats. That is a strong hint
that §8's conclusion ("float only") is too strong and that a packed-colour
fetch format does exist, encoded in one of the descriptor bit groups our
corpus only ever saw as zero (`[15:12]`, `[21:20]`, `[26]`).

**This is directly testable and cheap**: run any app that calls
`glColor4ub`/`glColorPointer(4, GL_UNSIGNED_BYTE, …)` under the vendor GL
with `tools/capture-gl-matrix.sh`, and decode `0x10b8`. If a descriptor
appears with a mask of `xyzw` at a **1-dword-wide** slot, the packed format is
real and one of those bit groups is non-zero. Every variant in the present
corpus uses `glColor3f`, which is why the question is still open. Until then,
expanding ioquake3's `color[]` to floats is the safe path.
