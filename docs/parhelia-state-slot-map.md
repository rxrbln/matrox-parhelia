# Parhelia state-slot → hardware-register translation

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** The `reg = 0x1000 + slot*4` rule is refuted.
How a GL state change travels from the ICD's region-2 record stream to a
hardware register, recovered from `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`
(unstripped, `mtxNNNN` symbols; disassembly in `scratchpad/mtxk_r.asm`).

This is the hop that was missing. The short answer: **there is no slot →
register table.** The kernel keeps a bit-packed software shadow of GL state,
then a validation pass *computes* hardware register values from it. Most slots
feed a register only through arithmetic that mixes several slots; some slots
feed no register at all.

---

## 1. The mechanism

Five stages, all inside the kernel object:

```
region 2 record          [key][count] + count*(slot,value)          ICD writes
      |
      |  mtx558 @0x44ef0   record dispatcher, jump table .rodata+0x1b08 (82 ops)
      v
per-key slot handler      key 0x05 -> mtx1734 @0x1abf0  (29 slots, table .rodata+0x1100)
                          key 0x0c -> mtx2617 @0x19950  (172 slots, table .rodata+0xba0)
      |
      |  writes a bit-packed GL-state SHADOW + sets a DIRTY bit
      v
GL state shadow           HWSTATE + 0x0c00 .. 0x2980   (see §3)
      |
      |  mtx3712 @0x188f0 "validate" walks the dirty words and calls
      |  per-group validators (mtx3654, mtx740, mtx2497, mtx2926, ...)
      |  which COMPUTE hardware values (see §6)
      v
HW register file          HWREG, 0x180 entries x 16 bytes, + a dirty bitmap
      |                   two identical banks, 0x1a30 apart          (see §4)
      |
      |  mtx1342 @0x90010 walks the bitmap and emits DMA packets
      v
DMA ring                  opcode 0x41 block-load packet              (see §5)
```

### Where the state lives

`mtx558` is called with `rdi = <driver object>`; every handler does
`rdi -> [rdi+0x30]; +0x78`. Call that pointer **HWSTATE**. All shadow offsets
below are relative to HWSTATE.

HWSTATE also carries two *slot value caches* used to suppress redundant work
and to answer `glGet`:

| key | slot cache | rebuilt from the shadow by |
|---|---|---|
| 0x0c | `HWSTATE + 0x10 + slot*4` (172 dwords, ends 0x2bc) | `mtx1900` @0x17830 |
| 0x05 | `HWSTATE + 0x3a0 + slot*4` (29 dwords, ends 0x414) | `mtx1938` @0x17dd0 |

A record entry whose value equals the cached value is dropped before the
handler runs, so **a slot that is written with its current value produces no
hardware traffic at all**. This is why a capture can show a `glEnable` with no
ring activity.

`mtx1900`/`mtx1938` are the machine-checkable inverse of the handlers and were
used to verify every row in §3.

### The record grammar, re-confirmed

`mtx558` @0x44f92:

```
eax = [r13+0]          ; key
rbp = r13 + 8          ; payload
if (key > 0x51) -> skip record
jmp .rodata+0x1b08[key*8]
```

Per-record advance for keys 0x05 and 0x0c is `r13 += count*8 + 8`
(`0x453c0: lea r13,[r13+rax*8+0x8]`), i.e. **8-byte (slot, value) entries** —
exactly the grammar the three earlier RE passes agreed on. CONFIRMED.

### Ground truth that anchors everything

`captures/bankb-polyoffset-0528.txt`, op `polyoffset_3_7`, region 2:

```
0x10 00000005   key  = 0x05
0x14 00000003   count= 3
0x18 00000015   slot 0x15
0x1c 40400000     = 3.0f      (glPolygonOffset factor)
0x20 00000016   slot 0x16
0x24 40e00000     = 7.0f      (glPolygonOffset units)
0x28 00000019   slot 0x19
0x2c 00000001     = 1         (GL_POLYGON_OFFSET_FILL enabled)
```

The full chain for those three slots is resolved end to end in §6.1.

---

## 2. Dispatcher map (`mtx558`, .rodata+0x1b08)

82 opcodes; 44 of them share the "ignore this record" target 0x452b8.
Handlers that matter for state:

| key | handler | entry size | what it is |
|---|---|---|---|
| 0x00 | inline | 8 | skip (advance only) |
| 0x05 | `mtx1734` @0x1abf0 | 8 | **GL state slots, table A** (29 slots) |
| 0x06 | `mtx3081` @0x1ae50 | 8 | (state, not decoded here) |
| 0x07 | 0x45d02 -> `mtx651` | 24 | |
| 0x08 | `mtx2530` @0x4bf10 | 32 | |
| 0x0a | inline + `mtx1744` | 8+ | flush/sync then state |
| 0x0c | `mtx2617` @0x19950 | 8 | **GL state slots, table B** (172 slots) |
| 0x0d | `mtx3074` @0x4a770 | 32 | |
| 0x0e | inline @0x459dc | 8 | per-unit `(u16 unit, u16 sub, u32 val)` texture-stage state |
| 0x12 | inline @0x45f5f | 16 | 4-dword records into shadow 0xcd9..0xce8 |
| 0x15 | inline @0x45d83 | 36 | fence + `mtx3513` |
| 0x18 | `mtx?` @0x45fe8 | | |
| 0x19 | `mtx790` @0x4c010 | 32 | |
| 0x1a | `mtx1314` @0x48660 | 24 | |
| 0x1b | `mtx2196` @0x4acb0 | 40 | |
| 0x1c | `mtx1385` @0x4a270 | 40 | |
| 0x20..0x27 | various | | textures / programs / matrices |
| 0x25 | inline @0x457c1 | 20 | 6 slots x 4 dwords -> shadow 0x1da0+i*0x10 |
| 0x26 | inline @0x45727 | 16 | 4-dword record -> shadow 0xcc1..0xcd0 |
| 0x27 | inline @0x4567e | 8 | 2 floats -> shadow 0x1c00/0x1c04 (+ 0xd8c) |
| 0x35 | `mtx1922` @0x1b290 | 16 | |
| 0x36 | `mtx1688` @0x47170 | 8 | |
| 0x37 | `mtx2824` @0x4a700 | 16 | |
| 0x40 | `mtx2166` @0x8ac40 | var | |
| 0x41 | `mtx2488` @0x8b260 | 4 | |
| 0x42 | inline @0x45462 | var | **direct HW-register upload** (see §5.2) |
| 0x43 | inline @0x453d1 | 0x678 | |
| 0x44 | `mtx631` @0x49870 | 48 | |
| 0x45 | `mtx3443` @0x1b3b0 | 0x670 | |
| 0x46 | `mtx3577` @0x74160 | 0x678 | |
| 0x47 | `mtx2833` @0x73ff0 | 8 | |
| 0x48 | inline @0x452ff | 24 | |
| 0x49 | inline @0x452c8 | 20 | |
| 0x50 | `mtx3996` @0x4bcf0 | 16 | |
| 0x51 | `mtx4017` @0x4bcd0 | 8 | |

CONFIRMED (read from the relocation table and each handler's advance code).

---

## 3. Slot → shadow field

Legend: `[X]` = HWSTATE+X. "dirty" is the flag word/bit the handler sets;
the validator in §6 keys off it.

### 3.1 Key 0x05 — 29 slots, `mtx1734`

CONFIRMED for every row (read instruction by instruction; the 13 rows marked
`rb` are additionally verified against `mtx1938`'s read-back).

| slot | shadow write | dirty | rb |
|---|---|---|---|
| 0x00 | — NO-OP — | | |
| 0x01 | `[0x1be8]` bit0 = v&1 | `[0x1bee] \|= 0x08` | rb |
| 0x02 | `[0x1be8]` bit1 = v&1 | `[0x1bee] \|= 0x10` | rb |
| 0x03 | — NO-OP — | | |
| 0x04 | `[0x1be1]` bit2 = v&1 | `[0x1bed] \|= 0x80` | rb |
| 0x05 | `[0x1be8]` bits5-6 = v&3 | `[0x1bee] \|= 0x20` | rb |
| 0x06..0x08 | — NO-OP — | | |
| 0x09 | DWORD `[0xc3f]` = v | `[0xc08] \|= 0x80` | rb |
| 0x0a | DWORD `[0xc43]` = v | `[0xc08] \|= 0x80` | rb |
| 0x0b | DWORD `[0xc47]` = v | `[0xc08] \|= 0x80` | rb |
| 0x0c | DWORD `[0xc4b]` = v | `[0xc08] \|= 0x80` | rb |
| 0x0d | `[0x1be8]` bit4 = v&1 | `[0x1bef] \|= 0x04` | |
| 0x0e..0x11 | — NO-OP — | | |
| 0x12 | `[0x1be8]` bit2 = v&1 | `[0x1bec] \|= 0x20` | rb |
| 0x13 | DWORD `[0x12fb]` = **(u8)v** | `[0xdbb] \|= 0x01` | rb |
| 0x14 | `n=v+1`; if `[0x12ef]!=n` call `mtx424(ring, n, HWSTATE+0x12ff)`, `[0x12ef]=n` | `[0xdbb] \|= 0x01` (only when changed) | rb |
| 0x15 | DWORD `[0x1e48]` = v (raw bits) | `[0x1bef] \|= 0x01` | |
| 0x16 | DWORD `[0x1e4c]` = v (raw bits) | `[0x1bef] \|= 0x01` | |
| 0x17 | `[0x1be1]` bit3 = v&1 | `[0x1bef] \|= 0x01` | |
| 0x18 | `[0x1be1]` bit4 = v&1 | `[0x1bef] \|= 0x01` | |
| 0x19 | `[0x1be1]` bit5 = v&1 | `[0x1bef] \|= 0x01` | |
| 0x1a | DWORD `[0xd84]` = v | `[0xd54] \|= 0x04` | rb |
| 0x1b | DWORD `[0x1e50]` = v | `[0x1bef] \|= 0x02` | rb |
| 0x1c | — NO-OP — | | |

Transforms in this table: slot 0x13 truncates to 8 bits; slot 0x14 stores
`v+1` and the read-back does **not** remove the bias.

### 3.2 Key 0x0c — 172 slots, `mtx2617`

Prologue (CONFIRMED):

* `id > 0x200` -> entry dropped.
* `0x100 < id <= 0x200` -> sets/clears bit `(id-0x101)` of a **lock bitmap** at
  `HWSTATE+0x414`. No state write. This is how the ICD pins a slot.
* `id <= 0x100` -> if lock bit `(id-1)` is set the entry is dropped; else
  compare/update the slot cache and dispatch.
* `id == 0` reads out of bounds before dispatching — **never emit slot 0**.

NO-OP slots (dispatch target 0x199b0, cache updated but no shadow effect):
`0x00, 0x04, 0x0b, 0x1e, 0x1f, 0x20, 0x21, 0x28, 0x2a, 0x32, 0x33, 0x3d, 0x3f,
0x60-0x7f, 0x8a, 0x95, 0x96, 0x99, 0xa2, 0xa3, 0xa4, 0xa5, 0xa9`.

Simple stores — CONFIRMED:

| slot | shadow | dirty |
|---|---|---|
| 0x07 | DWORD `[0xc9b]` | `[0xc87] \|= 0x04` |
| 0x0c | DWORD `[0xc29]` | `[0xc08] \|= 0x10` |
| 0x0e | BYTE `[0xc9f]` | `[0xc87] \|= 0x04` |
| 0x0f | BYTE `[0xc31]` | `[0xc08] \|= 0x40` |
| 0x13 | DWORD `[0xc3f]` **and** `[0xc43]` | `[0xc08] \|= 0x80` |
| 0x14 | DWORD `[0xc47]` **and** `[0xc4b]` | `[0xc08] \|= 0x80` |
| 0x17 | DWORD `[0xca0]` | `[0xc87] \|= 0x04` |
| 0x18 | DWORD `[0xc32]` | `[0xc08] \|= 0x40` |
| 0x19 | DWORD `[0xc36]` | `[0xc08] \|= 0x40` |
| 0x1a | BYTE `[0xc20]` | `[0xc08] \|= 0x02` |
| 0x1b | BYTE `[0xc3a]` | `[0xc08] \|= 0x80` |
| 0x22 | DWORD `[0xd6c]` | `[0xd54] \|= 0x01` |
| 0x23 | DWORD `[0xd70]` | `[0xd54] \|= 0x04` |
| 0x24 | DWORD `[0xd78]` | `[0xd54] \|= 0x04` |
| 0x25 | DWORD `[0xd7c]` | `[0xd54] \|= 0x04` |
| 0x26 | DWORD `[0xd80]` | `[0xd54] \|= 0x04` |
| 0x27 | BYTE `[0x1263]` | `[0x11e9]\|=0x10, [0xdd1]\|=0x10, [0xdbb]\|=0x08` |
| 0x2b | DWORD `[0xdeb]` | `[0xdd9]\|=1, [0xdd1]\|=1, [0xdbb]\|=8` |
| 0x2e | DWORD `[0xdef]` | `[0xdd9]\|=4, [0xdd1]\|=4, [0xdbb]\|=8` |
| 0x34 | BYTE `[0xca4]` | `[0xc87] \|= 0x08` |
| 0x35..0x3b | DWORD `[0xca5],[0xca9],[0xcad],[0xcb1],[0xcb5],[0xcb9],[0xcbd]` | `[0xc87] \|= 0x08` |
| 0x40..0x5f | DWORD `[0x1b48 + (slot-0x40)*4]` (32 dwords, ends 0x1bc4) | `[0x1b44] \|= 0x01` |
| 0x8c | DWORD `[0xd74]` | `[0xd54] \|= 0x04` |
| 0x9a | DWORD `[0x1e30]` | `[0x1bee] \|= 0x04` |
| 0x9b | DWORD `[0x1e34]` | `[0x1bee] \|= 0x04` |
| 0x9e | DWORD `[0x1e3c]` | `[0x1bee] \|= 0x04` |
| 0x9f | DWORD `[0x1e40]` | `[0x1bee] \|= 0x04` |
| 0xa0 | DWORD `[0x1e44]` | `[0x1bee] \|= 0x04` |
| 0xa6 | DWORD `[0x1e38]` | `[0x1bee] \|= 0x04` |
| 0xaa | DWORD `[0x1e2c]` | `[0x1bee] \|= 0x02` |
| 0xab | DWORD `[0xc3b]` | `[0xc08] \|= 0x80` |

Bitfield stores — CONFIRMED:

| slot | field | dirty |
|---|---|---|
| 0x08 | `[0x1be4]` bits 4-5 = v&3 | `[0x1bec] \|= 0x80` |
| 0x09 | `[0x1be4]` bits 6-7 = v&3 | `[0x1bed] \|= 0x01` |
| 0x10 | `[0x1be0]` bit1 | `[0x1bed] \|= 0x02` |
| 0x16 | `[0x1be5]` bits 0-1 = v&3 | `[0x1bed] \|= 0x04` |
| 0x1c | `[0xd68]` bit0 | `[0xd54] \|= 0x02` |
| 0x1d | `[0xdcb]` bit0 | `[0xdbb] \|= 0x10` |
| 0x29 | `[0xdcb]` bit1 | `[0xdbb] \|= 0x40` |
| 0x2c | `[0xde7]` bits 5-7 = v&7 | `[0xdd8]\|=0x80, [0xdd0]\|=0x80, [0xdbb]\|=8` |
| 0x2d | `[0xde8]` bits 0-2 = v&7 | same |
| 0x30 | `[0xd68]` bit1 | `[0xd54] \|= 0x02` |
| 0x31 | `[0xde8]` bits 15-16 = v&3 | `[0xdd9]\|=2, [0xdd1]\|=2, [0xdbb]\|=8` |
| 0x88 | `[0x1be0]` bit2 | **none — no dirty flag set** |
| 0x89 | `[0x1be0]` bit3 | `[0x1bed] \|= 0x10` |
| 0x8d | `[0x1be0]` bit6 | `[0x1bee] \|= 0x01` |
| 0x8e | `[0x1be0]` bit4 | `[0x1bed] \|= 0x40` |
| 0x8f | `[0x1be0]` bit5 | `[0x1bed] \|= 0x80` |
| 0x90 | `[0xdcb]` bit2 | `[0xdbb] \|= 0x40` |
| 0x91 | `[0x1be6]` bits 1-2 | `[0x1bee] \|= 0x01` |
| 0x92 | `[0x1be6]` bits 3-4 | `[0x1bee] \|= 0x01` |
| 0x93 | `[0x1be6]` bits 5-6 | `[0x1bee] \|= 0x01` |
| 0x94 | WORD `[0x1be6]` bits 7-8 (crosses into 0x1be7 bit0) | `[0x1bee] \|= 0x01` |
| 0x98 | `[0x1be7]` bits 1-6 = v&0x3f | `[0x1bec] \|= 0x10` |
| 0x9c | `[0x1be1]` bit0 | `[0x1bee] \|= 0x04` |
| 0x9d | `[0x1be1]` bit1 | `[0x1bee] \|= 0x04` |
| 0xa7 | `[0x1be0]` bit7 | `[0x1bee] \|= 0x02` |

Mutually exclusive pair on `[0xc08]`: slot 0x0d writes `[0xc25]` and sets bit3 /
clears bit2; slot 0xa8 writes `[0xc21]` and sets bit2 / clears bit3.

Slots with a **value transform** — CONFIRMED:

| slot | transform |
|---|---|
| 0x01 | `[0xde3] = v`; always `[0xdd8]\|=8,[0xdd0]\|=8,[0xdbb]\|=8`; **additionally** `[0xdd8]\|=0x10,[0xdd0]\|=0x10` when `(old==0) != (v==0)` |
| 0x02 | 3-way remap into `[0xc2d]`: `1->2`, `2->3`, else `0`; `[0xc08]\|=0x20` |
| 0xa1 | same field `[0xc2d]`: `(u8)v!=0 -> 3`, else `0`; `[0xc08]\|=0x20` |
| 0x2f | `[0x1be1]` bits3,4 := 0, bit5 := (v!=0); `[0x1e4c] = (float)(u32)v`; `[0x1e48] = (float)(u32)v * 0.25f`; `[0x1bef]\|=1` |
| 0x3c | packed ARGB -> 8 float RGBA quads: `R=((v>>16)&0xff)/255`, `G=((v>>8)&0xff)/255`, `B=(v&0xff)/255`, `A=(v>>24)/255`; written to `[0xe27+i*0x82]`,`[0xe2b+..]`,`[0xe2f+..]`,`[0xe33+..]` for i=0..7; `[0xdd9+i*0x82]\|=0x20`, `[0xdd1]\|=0x20`, `[0xdbb]\|=8` |
| 0x8b | same ARGB unpack, single destination `[0x1e1c..0x1e28]`; `[0x1bed]\|=0x20` |
| 0x97 | 3-bit field `[0x1be4]` bits 14-16: `v<=0xfe -> v&7`; `v==0xff -> 4`; `v==0x100 -> 5`; `v>0x100 -> field unchanged, dirty set anyway`; `[0x1bee]\|=2` |
| 0x0a | one value fanned to three places: `[0x1bc8]`, `[0x12eb]`, `[0x1e54]`; `[0x1b44]\|=2, [0xdbb]\|=0x81, [0x1bef]\|=4` |
| 0x05 | bit 0 of the 24-bit field `S = ([0xdc8]>>27)&0xffffff` := (v!=0); `[0xdbb]\|=0x20` |
| 0x06 | `S` bit1 := (v!=0); `[0xdbb]\|=0x20` |
| 0x80..0x87 | `S` bits `3k..3k+2` = v&7 for k=0..7; `[0xdbb]\|=0x20`. Note 0x80 aliases the booleans of 0x05/0x06. |

Slots that call out instead of writing a plain shadow:

| slot | call | effect |
|---|---|---|
| 0x11 | `mtx1529(HWSTATE, v)` | `WORD [0xde8]` bits 6-8 := `v!=2 ? 1 : ([HWSTATE+0xd4]>1 ? 3 : 2)` |
| 0x12 | `mtx3859(HWSTATE, v)` | own table `.rodata+0xb20`, 7 entries; writes `BYTE [0xde9]` bits 1-6: `1->1, 2->(>1?3:2), 3->9, 4->(>1?11:10), 5->17, 6->(>1?19:18)`, `0->no-op` |
| 0x15 | `mtx968(HWSTATE, v)` | table `.rodata+0xb58`, 9 entries; writes constant tuples into `[0xe03],[0xe0b],[0xe0f],[0xe13],[0xe1b],[0xe1f]`; also `[0xe85]=1` |
| 0x3e | `mtx696([HWSTATE+0x8], (u8)v)` | pure action: sets `[obj+0x8360]`, and on 0 clears global `mtx2299` / bumps a counter, on non-0 arms a callback. **No shadow, no dirty, no register.** |

### 3.3 Cross-key aliasing (matters when you emit records yourself)

* key 0x05 slots `0x09/0x0a/0x0b/0x0c` own `[0xc3f]/[0xc43]/[0xc47]/[0xc4b]`
  individually; key 0x0c slots `0x13`/`0x14` overwrite them **in pairs**.
* key 0x0c slot `0x2f` overwrites the entire group owned by key 0x05 slots
  `0x15,0x16,0x17,0x18,0x19` — and stores **floats converted from an integer**,
  where key 0x05 stores the client's raw bits. Do not mix the two keys for
  polygon offset.
* key 0x0c `0x9c/0x9d` and key 0x05 `0x04` share byte `[0x1be1]`; each does a
  proper read-modify-write, so only the dirty surface is shared.

### 3.4 Read-back asymmetries found by the `mtx1900`/`mtx1938` cross-check

These are real driver bugs/quirks; they matter if you plan to trust `glGet`
round-trips as an oracle.

* **slot 0x0d (key 0x0c) writes `[0xc25]`, `mtx1900` reads `[0xc21]`** — which is
  slot 0xa8's field. `[0xc25]` has no reader anywhere in the object.
* **slot 0x06** read-back is `(q>>27)&2`, i.e. 0 or **2**, not 0/1.
* **slots 0x24, 0x25, 0x26, 0x9a** store the raw dword but are read back with
  `movss`+`cvttss2si` — float in, truncated int out.
* **slot 0x97** read-back goes through `.data+0xa40 = {0,1,2,3,0xff,0x100,0,0}`.
* **slots 0x11/0x12** read back the remapped hardware field, never `v`.
* **slots 0x02/0xa1** alias `[0xc2d]`; only 0x02's cache entry is rebuilt.
* **slots 0x13/0x14** read back only the second of their two writes.
* Handled but never read back: `0x0a, 0x0c, 0x15, 0x3e, 0xa1, 0xab` (key 0x0c)
  and `0x0d, 0x15, 0x16, 0x17, 0x18, 0x19` (key 0x05).

### 3.5 Dirty-flag word map

`[0xc08] [0xc6b] [0xc87] [0xd01] [0xd54] [0xd98] [0xdbb] [0xdbf] [0xdcf]`,
`[0xdd7 + k*0x82]` for k=0..9 (per texture unit),
`[0x1ad8] [0x1adc]`, `[0x1ae4 + j*0x14]` for j=0..3,
`[0x1b44] [0x1bd4] [0x1bec] [0x1bf0] [0x1bf4] [0x1e9c] [0x2978]`.

That list is exhaustive — `mtx412` @0x18720 clears exactly these, and
`mtx?` @0x18162 sets them all to `0xffffffff` for a full re-validate.
CONFIRMED.

---

## 4. The hardware register file (HWREG)

`mtx3712` (validate) fetches `HWREG = [[HWSTATE+0x8] + 0x50]` and hands it to
the per-group validators. HWREG contains **two identical banks**, 0x1a30 apart:

```
HWREG + 0x07e8   bank A dirty bitmap      \
HWREG + 0x0a18   bank A registers         |  0x1a30 bytes
                 0x180 entries x 0x10     /
HWREG + 0x2218   bank B dirty bitmap      \
HWREG + 0x2448   bank B registers         |  0x1a30 bytes
                                          /
HWREG + 0x3c48.. pointer table into bank A (see below)
```

* **register storage = bitmap_base + 0x230 + index*0x10** — CONFIRMED three
  independent ways:
  * `mtx1342` @0x9005c: `lea rcx,[r11+0x230]` with `r11` = bitmap base;
  * `mtx1785` @0x8ffb0 / `mtx3105` @0x8ffe0: array walk from `base+0x1a30`
    down to `base+0x230` in steps of `0x10` (0x1800/0x10 = **0x180 entries**);
  * `mtx1342` @0x90076: `cmp r15d,0x17f` bounds the scan at index **0x17f**.
* **each index is 4 dwords**, and the four dwords hold four *different* values
  (not a replicated scalar) — e.g. index 0x17d's power-on defaults are
  `{8388608.0f, 0.05f, 31.0f, 0.032258f}`.
* **dirty bitmap** = `{ DWORD summary; DWORD word[12]; ... }`. `mtx2702`
  @0x8f560 is `MarkRange(bitmap, firstIndex, lastIndex)`: it sets bits
  `first..last` in `bitmap[4 + (i>>5)*4]` and ORs a per-word presence mask into
  `bitmap[0]`. `lastIndex == 0` means "same as first".
* Validators mark **both** banks (`bitmap A` then `bitmap B`) and write **both**
  register copies. Which bank a flush uses is chosen by `[HWREG+0x7a0]`
  (`mtx3995` @0x851c3: non-zero -> bank A at 0x7e8, zero -> bank B at 0x2218).

### 4.1 Pointer table — independent confirmation of `0xa18 + index*0x10`

`mtx2209` (device/context init) caches pointers to hot registers. Every entry
satisfies `ptr = HWREG + 0xa18 + index*0x10`:

| pointer slot | value | index |
|---|---|---|
| `[HWREG+0x3c48]` | `HWREG+0x1f58` | 0x154 |
| `[HWREG+0x3c50]` | `HWREG+0x2018` | 0x160 |
| `[HWREG+0x3c58]` | `HWREG+0x2028` | 0x161 |
| `[HWREG+0x3c60]` | `HWREG+0x1ef8` | 0x14e |
| `[HWREG+0x3c68]` | `HWREG+0x2138` | 0x172 (and 0x173 at +0x10) |
| `[HWREG+0x3c70]` | `HWREG+0x2168` | 0x175 |
| `[HWREG+0x3c78]` | `HWREG+0x1f68` | 0x155 |
| `[HWREG+0x3c80]` | `HWREG+0x1ee8` | 0x14d |
| `[HWREG+0x3c88 + k*8]`, k=0..11 | `HWREG+0x1818 + (k*0x70)` | 0xe0 + k*7 (texture stages, 7 regs each) |
| `[HWREG+0x3ce8]` | `HWREG+0x1d98` | 0x138 |
| `[HWREG+0x3cf0]` | `HWREG+0x1d58` | 0x134 |
| `[HWREG+0x3cf8]` | `HWREG+0x1f78` | 0x156 |
| `[HWREG+0x3d00 + k*8]`, k=0..5 | `HWREG+0x20d8 + k*0x10` | 0x16c..0x171 |
| `[HWREG+0x3d30]` | `HWREG+0x2198` | 0x178 |

Every one of those indices is independently confirmed by the `mtx2702` call
that follows the store with the matching immediate. CONFIRMED.

---

## 5. HWREG → DMA

### 5.1 The block-load packet — `mtx1342` @0x90010

`mtx1342(bitmapBase, ring)` scans indices 0..0x17f, coalesces each run of
consecutive dirty indices (max **0x3f** per packet -- the count field is six
bits and is clamped at 0x901d9/0x901fd; an earlier 0x40 here was wrong) and
appends:

```
dword 0 : 0x41000000 | ((count << 17) & 0x007e0000) | ((count*4 + 3) & 0xffff)
dword 1 : count                     (number of register entries)
dword 2 : start index               (0 .. 0x17f)
dword 3 : 1
dword 4 : 0
then    : count * 16 bytes copied verbatim from bitmapBase + 0x230 + start*0x10
ring    : advanced by (count*4 + 5) * 4 bytes
```

CONFIRMED from the instruction stream (`0x901e5`..`0x9023d`). The same ring is
the one that carries the classic `0x2121xxxx`-tagged byte-index packets — see
`mtx693` @0x436a0, which builds both kinds into `[ring+0x60]`.

**This is why raster state never shows up in an mmiotrace.** The kernel does
not poke a register per slot; it DMAs a block of the state file.

### 5.2 The byte-index packet (the other namespace)

Unchanged from what the repo already documents, and re-confirmed here:
`mtx558` op 0x43 emits `[0x21212425][fenceId][fenceVal]` and op 0x0a/0x15 emit
`[4][0xc0afafc0][a][b][c][d]` — one dword of four register index BYTES, then one
data dword per non-pad index, `reg = 0x1000 + index*4`, `0x21` = pad/terminate.
`mtx558` op 0x42 (`0x45462`) lets the client upload straight into this
namespace: it clamps the index to `0..0xff` (`cmp ecx,0x100; cmovae ecx,0xff`),
records `(index, count, pointer)` in a slot ring at `[HWSTATE+0x1e60]+0xb0+n*0x40`
and marks bits in a 256-bit bitmap at `+0x18/+0x1c`.

**These two namespaces are different.** The byte-index one is 256 single-dword
registers (`0x1000..0x13fc`). The block one is 0x180 four-dword entries fed by
opcode 0x41. A state slot resolves into the **block** namespace.

### 5.3 What is still open: the absolute base of the block namespace

The kernel never converts a block index to an MMIO address — the DMA engine
does. So the object gives the index, not the address. Two candidates, neither
proven:

* `MMIO = index * 0x10` (component c at `index*0x10 + c*4`), which would place
  the file at `0x0000..0x17ff` of the 0x2000-byte BAR-1 window; or
* `MMIO = 0x1000 + index*0x10`, i.e. dword-register `4*index + c`.

Against a linear mapping: the guard-band constant `31.0f` is a power-on default
of **block index 0x17d dword 2**, while the repo has `31.0f` CONFIRMED at MMIO
`0x12f4` (byte index 0xBD) from two independent binaries. Those cannot both be
the same storage under either linear rule, so the block file is more likely an
**internal indexed state RAM that is not MMIO-mapped at all** — reachable only
through opcode 0x41. Treat §6 as "slot -> block index", and resolve the block
index to hardware only by experiment (write one index, dump `0x0000..0x1ffc`).

**Do not publish an MMIO address for any of these indices yet.**

---

## 6. Slot → hardware register index

The validators live at 0x85000..0x8f000. Each takes
`rdi = HWREG`, `rsi = HWSTATE + 0x1bd8` (the "raster" state group; add 0x1bd8
to convert an offset here into an HWSTATE offset), `rdx`/`rcx` = surface and
context objects. Rows below are CONFIRMED unless marked.

### 6.1 Polygon offset — the anchor, resolved end to end

`mtx740` @0x864f0, at 0x865e6:

```
xmm3 = raster[0x270]                    ; = HWSTATE[0x1e48] = key 0x05 slot 0x15 (factor)
xmm2 = raster[0x274]                    ; = HWSTATE[0x1e4c] = key 0x05 slot 0x16 (units)
xmm2 *= [depthSurface + 0x15]           ; per-depth-format unit scale
if !(raster[0x08] & 1) {                ; HWSTATE[0x1be0] bit0
    xmm3 *= [HWREG+0x6f4]
    xmm2 *= (float)[HWREG+0x6f0] * [HWREG+0x6f4]
}
p = [HWREG+0x3d30]                      ; = HWREG + 0x2198 = block index 0x178
p[0] = xmm3                             ; and mirror HWREG+0x3bc8 (bank B)
p[1] = xmm2                             ; and mirror HWREG+0x3bcc
mtx2702(HWREG+0x07e8, 0x178, 0)         ; mark dirty, bank A
mtx2702(HWREG+0x2218, 0x178, 0)         ; mark dirty, bank B
```

| key | slot | GL | block index | dword | transform |
|---|---|---|---|---|---|
| 0x05 | 0x15 | `glPolygonOffset` factor | **0x178** | 0 | `factor * [HWREG+0x6f4]` (unscaled when `[0x1be0]` bit0 set) |
| 0x05 | 0x16 | `glPolygonOffset` units | **0x178** | 1 | `units * depthFormatScale * (float)[HWREG+0x6f0] * [HWREG+0x6f4]` |
| 0x05 | 0x1b | (shadow `[0x1e50]`) | **0x178** | 2 | clamped, `mtx3654` @0x8c715: if `raster[0x278] < .LC40` use `.LC38`, else pass through |
| 0x05 | 0x17/0x18/0x19 | `GL_POLYGON_OFFSET_POINT/LINE/FILL` | — | — | enable bits in `[0x1be1]` bits 3/4/5; they gate whether `mtx740` runs, they are not a register |

That closes the controlled experiment: `glPolygonOffset(3.0, 7.0)` lands in
block register 0x178 dwords 0 and 1, scaled — which is exactly why the raw
floats `40400000`/`40e00000` never appear in the ring.

### 6.2 Other resolved slots

| key | slot | shadow | block index | dword | transform | tag |
|---|---|---|---|---|---|---|
| 0x0c | 0x9e | `[0x1e3c]` | 0x172 | 0 | `v * (.LC33 / [depthSurface+0x39])` | CONFIRMED (`mtx3654` @0x8bb29) |
| 0x0c | 0x9f | `[0x1e40]` | 0x172 | 1 | same scale | CONFIRMED (@0x8bb43) |
| 0x0c | 0xa0 | `[0x1e44]` | 0x172 | 2 | same scale | CONFIRMED (@0x8bb5e) |
| 0x0c | 0x9b | `[0x1e34]` | 0x173 | 0 | `v * [depthSurface+0x39]` | CONFIRMED (@0x8bbd7) |
| 0x0c | 0xa6 | `[0x1e38]` | 0x173 | 1 | `v * [depthSurface+0x39]`, clamped to `.LC39` when greater | CONFIRMED (@0x8bbfe) |
| 0x0c | 0x9a | `[0x1e30]` | 0x172 | — | `mtx2497` @0x867a0 marks 0x172 after reading `raster[0x258]` | CONFIRMED index, INFERRED dword |
| 0x0c | 0xaa | `[0x1e2c]` | 0x155 | 3 | compared against `[HWREG+0x3c78][3]`, written on change | CONFIRMED (`mtx3654` @0x8b74d/0x8d230) |
| — | depth range | (from the depth surface record, not a slot) | 0x14e | 0,1 | `scale = .LC33/(far-near)`, `bias = -near*scale` | CONFIRMED (`mtx3654` @0x8b6f2) |
| 0x0c | 0x9c | `[0x1be1]` bit0 | — | — | copied to the software flag `[HWREG+0x6e9]`, gates the 0x172 path | CONFIRMED (@0x8bab9) |

Validators seen marking further indices, whose slot inputs are not yet
disentangled (leads, not results): `mtx1322`->0x134, `mtx2588`->0x138,
`mtx2441`->0x134/0x156, `mtx2926`->0x160/0x161, `mtx3654`->0x149, 0x14d, 0x154,
0x155, 0x175, 0x17e, `mtx867`/`mtx2209`->0x172, 0x173, 0x174, 0x175, 0x178,
0x179, 0x17d, 0x17e, 0x160, 0x14d.

### 6.3 Register defaults recovered from `mtx2209`

Useful for a from-scratch driver that must initialise the block file:

> **CORRECTED 2026-09-05 — four rows here were wrong and they are in the
> minimum constant set.** This table read the register `ebx` as 1.0 where it
> holds 0.0. `docs/parhelia-block-state-map.md` carries the corrected values and
> both vendor binaries agree with them. The rows are marked inline below; do not
> program the struck values.

| index | dword 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| 0x14d | — | — | — | `0x42000000` |
| 0x172 | **1.0f** | **0** | **0** | **1.0f** (was `{0,1,1,0}`) |
| 0x173 | -0.5f | -0.5f | r14 | r14 |
| 0x174 | `0x45ff0000` | `0x45ff0000` | `0xc5ff0000` | `0xc5ff0000` |
| 0x175 | **`0x43000000`** | **0** | **0** | **0** (was `{128,1,1,1}`) |
| 0x178 | **0** | **0** | **0.5f** | **`0x49400000`** (was `{1,1,0,786432}`) |
| 0x17d | `0x4b000000` (2^23) | `0x3d4ccccd` (0.05) | `0x41f80000` (31.0) | `0x3d042108` (1/31) |
| 0x17e | **0** | **`0x3d800000`** (0.0625) | **0** | **0** (was `{1,0.0625,1,1}`) |

(`r14`/`1.0f` per the init register allocation; CONFIRMED for the literals.)

---

## 7. Verdict on the register names already published

`docs/parhelia-gl-feature-map.md` §3 and the `docs/parhelia-gl-driver.md`
rasterizer table derive their addresses from `reg = 0x1000 + slot*4` applied to
region-2 slot ids. **Every such address is refuted.** Not "unproven" — refuted,
for a structural reason: a region-2 slot does not name a register at all. It
names a bit or a dword in a software shadow that is later combined with other
slots to compute an entry of a different, 4-dword-wide indexed file.

| claim (file:line) | claimed reg | slot | verdict |
|---|---|---|---|
| depth test enable (feature-map:173) | 0x101c | key 0x0c 0x07 | **REFUTED** — slot 0x07 writes DWORD `[0xc9b]`, dirty `[0xc87]` bit2 |
| depth func (feature-map:175) | 0x105c | 0x17 | **REFUTED** — DWORD `[0xca0]`, dirty `[0xc87]` bit2 |
| depth mask (feature-map:177) | 0x1038 | 0x0e | **REFUTED** — BYTE `[0xc9f]`, dirty `[0xc87]` bit2 |
| blend enable (feature-map:187) | 0x106c | 0x1b | **REFUTED** — BYTE `[0xc3a]`, dirty `[0xc08]` bit7 |
| blend factors (feature-map:190-193) | 0x1024/0x1028/0x102c/0x1030 | key 0x05 0x09-0x0c | **REFUTED** — DWORDs `[0xc3f]/[0xc43]/[0xc47]/[0xc4b]`, all on `[0xc08]` bit7. Note key 0x0c slots 0x13/0x14 write the same four fields **in pairs**, which is the real reason the sweep saw them move together |
| alpha test enable (feature-map:208) | 0x103c | 0x0f | **REFUTED** — BYTE `[0xc31]`, dirty `[0xc08]` bit6 |
| alpha func (feature-map:210) | 0x1064 | 0x19 | **REFUTED** — DWORD `[0xc36]`, dirty `[0xc08]` bit6 |
| alpha ref, 8-bit (feature-map:211) | 0x1060 | 0x18 | address **REFUTED** — DWORD `[0xc32]`, same dirty group as 0x19. The 8-bit encoding claim stands |
| stencil enable (feature-map:219) | 0x10d0 | 0x34 | **REFUTED** — BYTE `[0xca4]`, dirty `[0xc87]` bit3 |
| stencil ref / mask / op (feature-map:221-223) | 0x10e4/0x10e8/0x10dc | 0x39/0x3a/0x37 | **REFUTED** — slots 0x35..0x3b are a contiguous run of DWORDs `[0xca5]`..`[0xcbd]`, all on `[0xc87]` bit3. The *ordering* the doc inferred is right; the addresses are not |
| "0x10d8 is not part of a contiguous stencil block" (feature-map:389) | 0x10d8 | 0x36 | **the contradiction is dissolved** — 0x36 *is* `[0xcb1]`, squarely inside the stencil run. The two-float payload the doc saw was a record header, not this slot |
| cull face (feature-map:247, gl-driver:112) | 0x1058 | 0x16 | **REFUTED** — `[0x1be5]` bits 0-1, dirty `[0x1bed]` bit2. A 2-bit field, consistent with `1=off, 2/3=winding` |
| polygon mode (feature-map:249, gl-driver:114) | 0x1020 | 0x08 | **REFUTED** — `[0x1be4]` bits 4-5, dirty `[0x1bec]` bit7. Again a 2-bit field |
| front face (gl-driver:113) | 0x1014 | key 0x05 0x05 | **REFUTED** — `[0x1be8]` bits 5-6, dirty `[0x1bee]` bit5 |
| fog enable (feature-map:256) | 0x1070 | 0x1c | **REFUTED** — `[0xd68]` bit0, dirty `[0xd54]` bit1 |
| fog mode (feature-map:258) | 0x1230 | 0x8c | **REFUTED** — DWORD `[0xd74]`, dirty `[0xd54]` bit2 (the doc already voided this one) |
| lighting enable (feature-map:263) | 0x1224 | 0x89 | **REFUTED** — `[0x1be0]` bit3, dirty `[0x1bed]` bit4 |
| blend equation (gl-driver:110) | 0x12ac | 0xab | **REFUTED** — DWORD `[0xc3b]`, dirty `[0xc08]` bit7 |
| colour mask (gl-driver:111) | 0x12a0 | 0xa8 | **REFUTED** — DWORD `[0xc21]`, dirty `[0xc08]` bit2 (and clears bit3) |
| "glCullFace = 0x1024" (gl-driver:116) | 0x1024 | 0x09 | **REFUTED twice** — slot 0x09 of key 0x0c is `[0x1be4]` bits 6-7, and of key 0x05 is `[0xc3f]`. Neither is cull |
| scissor/viewport (gl-driver:117) | 0x1280/0x1284 | 0xa0/0xa1 | **REFUTED** — 0xa0 is DWORD `[0x1e44]` (block reg 0x172 dword 2), 0xa1 is the `[0xc2d]` remap. The `raster-clip.md` correction (scissor is 0x1328/0x132c) stands |
| select fragment program (feature-map:514) | 0x1050 | 0x14 | **REFUTED as an address.** Key 0x0c slot 0x14 writes `[0xc47]` *and* `[0xc4b]`; key 0x05 slot 0x14 is the one that means "program": it does `[0x12ef] = v+1` and calls `mtx424`. The *semantics* the doc inferred are right, the register is not |
| depth range -> 0x1268/0x126c/0x1298 (gl-driver:120) | — | 0x9a/0x9b/0xa6 | **REFUTED as addresses**, but the grouping is right: 0x9b and 0xa6 land in block reg **0x173** dwords 0 and 1, scaled by the depth-format factor (§6.2). "CPU-applied" is wrong — the kernel applies it |
| "polygon offset is software-handled, no register" (gl-driver:124-128) | — | 0x15/0x16 | **REFUTED** — see §6.1, block register 0x178 |

**Accidentally right:** none of the addresses. What survives untouched is every
*value-level* claim — the compare-func enum `1..8 = NEVER..ALWAYS`, the blend
factor enum, blend equation `1..5`, the 8-bit alpha reference, colour mask
`bit0=R..bit3=A`, cull `1=off / 2,3=winding`, front face `2=CW / 3=CCW`,
polygon mode `1=point 2=line 3=fill`. Those describe the value the client puts
in the record, which this analysis does not touch. Several are further
corroborated here by field width: cull, polygon mode, front face and slots
0x91-0x94 are all 2-bit fields, exactly as the enums require.

**Unaffected:** everything justified by the DMA byte-index namespace
(`reg = 0x1000 + index*4` for ring tag bytes) — 0x1080/0x1084/0x1090/0x1094
pads and fences, 0x1300 DWGCTL, 0x1324, 0x1328/0x132c scissor, 0x12f4 guard
band, 0x1220-0x123c surfaces, the vertex planes, the texture registers taken
from `CDPSTextureEngine::UpdateHardware`, and the fragment-combiner registers.
That rule is correct for that namespace and is re-confirmed in §5.2.

### 7.1 On "bank A / bank B"

The docs use "bank" for the record key (`key 0x0c = main bank`,
`key 0x05 = second bank`). That naming is now misleading: both keys write the
*same* shadow structure — they are two slot namespaces over one state block,
and they overlap (§3.3). The word "bank" is better spent on the two HWREG
copies at `+0x7e8` and `+0x2218` (§4), which really are banks.

Consequently there is no "bank B absolute base" to find: key 0x05's slots are
not offsets into a register window at all.


### 7.2 Resolving the bank attribution for blend factors and polygon mode

`docs/parhelia-draw-recipe.md` §3.2 attributes both to **key 0x0c**;
`docs/parhelia-gl-limits.md` §2.2 attributes both to **key 0x05**.  The slot
tables in §3.1 and §3.2 above settle it, and it is a split decision -- each doc
is right about one and wrong about the other.

**Blend factors -> key 0x05, slots 0x09..0x0c (gl-limits is right).**
Those four slots write the four shadow DWORDs individually:

    key 0x05  0x09 -> [0xc3f]      key 0x05  0x0b -> [0xc47]
    key 0x05  0x0a -> [0xc43]      key 0x05  0x0c -> [0xc4b]

Key 0x0c slot 0x09 is `[0x1be4]` bits 6-7 and is not a blend factor at all.
What key 0x0c *does* have is slots **0x13** and **0x14**, which write the same
four fields **in pairs** (`0x13 -> [0xc3f] and [0xc43]`, `0x14 -> [0xc47] and
[0xc4b]`).  That is the real reason a differential sweep saw the four factors
move two at a time, and it is what made the key 0x0c attribution look plausible.

**Polygon mode -> key 0x0c, slot 0x08 (draw-recipe is right).**
`key 0x0c 0x08 -> [0x1be4]` bits 4-5, dirty `[0x1bec] |= 0x80`.  Key 0x05 slot
0x08 cannot be it: slots 0x06..0x08 of key 0x05 are NO-OPs.

The general lesson is the one gl-limits states as its own thesis -- the same
small index means different things under different keys -- but it cuts both
ways, and neither doc's blanket attribution survives.  Emit against the table in
§3.1/§3.2, not against either summary.

---

## 8. Slots the kernel consumes itself (no register)

* **Every NO-OP slot** in §3.1 and §3.2 — the value is cached for `glGet` and
  discarded. Key 0x05: `0x00, 0x03, 0x06, 0x07, 0x08, 0x0e, 0x0f, 0x10, 0x11,
  0x1c`. Key 0x0c: `0x00, 0x04, 0x0b, 0x1e, 0x1f, 0x20, 0x21, 0x28, 0x2a, 0x32,
  0x33, 0x3d, 0x3f, 0x60-0x7f, 0x8a, 0x95, 0x96, 0x99, 0xa2-0xa5, 0xa9`.
* **key 0x0c slot 0x3e** — pure control action (`mtx696`): arms/disarms a
  callback and touches a global. No shadow, no dirty bit, no register.
* **key 0x05 slot 0x14** — program/buffer selection: `[0x12ef] = v+1` plus a
  call to `mtx424(ring, v+1, HWSTATE+0x12ff)`. The kernel binds a resource; no
  register value is produced by the slot itself.
* **key 0x05 slot 0x13** — `[0x12fb] = (u8)v`, dirty `[0xdbb]` bit0. Consumed by
  the record dispatcher itself: `mtx558` op 0x49 (0x452c8) tests
  `[HWSTATE+0x12fb] == 1` and skips the whole record when so, and op 0x46
  (0x453d1) compares `[HWSTATE+0x12ef]` against the payload. These two are the
  kernel's own program/buffer-selection state.
* **key 0x0c slot 0x88** — writes `[0x1be0]` bit2 but sets **no dirty flag**, so
  it can never reach hardware on its own; it is read directly by the validators.
* **key 0x0c slots 0x9c/0x9d** and **key 0x05 slots 0x17/0x18/0x19** — predicate
  bits. They gate which validator path runs (`mtx3654` @0x8bab9 copies
  `[0x1be1]` bit0 into the software flag `[HWREG+0x6e9]`); they are not values.
* **Slot ids 0x101..0x200 on key 0x0c** — not state at all: they set/clear the
  lock bitmap at `HWSTATE+0x414` that suppresses slot `id-0x101`.

---

## 9. How to finish the map

The remaining work is mechanical and the tooling is now obvious:

1. For a shadow field `[X]`, convert to a raster-group offset `X - 0x1bd8`
   (or find the group by its dirty word in §3.5).
2. `grep` the validators (0x85000..0x8f000) for a read of that offset off the
   group base register.
3. In the same basic block, find the pointer load `[HWREG+0x3cXX]` or the
   literal `HWREG+0x?` store, and the following `mtx2702(bitmap, idx, ...)`.
   `idx = (ptr - HWREG - 0xa18) / 0x10`; the dword is the store displacement/4.
4. The value transform is whatever arithmetic sits between the two.

The one hop that cannot be closed from the binary is §5.3 — the absolute
location of the 0x180 x 16-byte block file. That needs one hardware
experiment: submit an opcode-0x41 packet with a known index and payload, then
dump the whole 0x2000-byte register window and diff.

---

Copyright (c) 2026 René Rebe <rene@exactco.de>
