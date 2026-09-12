Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 (0x2539, LX/SUN90): what makes a draw packet carry more than one primitive

Static reverse engineering of the retained Matrox vendor binaries and the
retained ring captures, aimed at one question: our open driver can only draw
one triangle per draw packet on the P690, while the 0x0528 vendor captures show
eight triangles in a single packet.  This document establishes what the packet
and the surrounding register state actually are, what is and is not chip-class
dependent, and gives an ordered list of hardware experiments with exact dword
values.

No hardware was used.  Every statement is tagged **PROVEN** (read out of
instruction bytes, table bytes or capture dwords) or **INFERRED**.

## 0. Sources and reproduction commands

| tag | file |
|---|---|
| KRN | `/srv/t2/xf86-video-parhelia/matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` |
| PSER | `/srv/t2/xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` |
| CAP | `/home/ai/src/parhelia/xf86-video-parhelia/captures/gl-matrix/*-secs.txt` (0x0528) |
| CAPP | `/home/ai/src/parhelia/xf86-video-parhelia/captures/ioq3-p690-working-draw.txt` (P690) |
| MESA | `/home/ai/src/parhelia/src.mesa.x86-64-desktop.260909.123648.3741772/mesa-26.2.2/src/gallium/drivers/parhelia/reference/` |

```sh
KRN=/srv/t2/xf86-video-parhelia/matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
PSER=/srv/t2/xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll

# the kernel draw-packet builders (one per draw mode)
objdump -dr -M intel --start-address=0x60e0 --stop-address=0x7030 $KRN   # mtx1017, mode 5 = 0x97
objdump -dr -M intel --start-address=0x5d30 --stop-address=0x5db0 $KRN   # mtx3981, mode 3 = 0x8f
objdump -dr -M intel --start-address=0xb610 --stop-address=0xb680 $KRN   # mtx3319, PRIMWORD composer
objdump -dr -M intel --start-address=0x3c2d0 --stop-address=0x3ca00 $KRN # mtx2393, engine prime (LX branches)
objdump -dr -M intel --start-address=0x40a70 --stop-address=0x40b90 $KRN # mtx1916, PCI id -> chip-class bit
objdump -dr -M intel --start-address=0x3c260 --stop-address=0x3c2d0 $KRN # mtx2572, LX guard-band clamp
objdump -dr -M intel --start-address=0x69650 --stop-address=0x69790 $KRN # mtx958,  LX guard-band select

# the two primitive tables, read as raw bytes
objdump -s -j .data --start-address=0x14c20 --stop-address=0x14c90 $KRN

# the Windows P690 driver's own range-packet header builders
objdump -d -M intel $PSER | grep -n -B3 -A6 'or     ecx,0xd7000001'
objdump -d -M intel $PSER | grep -n -B8 -A8 'or     ecx,0x17000001'
```

Capture scan used below (counts every draw packet and its range length):

```sh
cd /home/ai/src/parhelia/xf86-video-parhelia/captures/gl-matrix
python3 - <<'EOF'
import re, glob, collections
for f in sorted(glob.glob('*-secs.txt')):
    dw = []
    for line in open(f):
        if line.startswith('==='):
            dw.append(None); continue
        for t in line.split():
            if re.fullmatch('[0-9a-f]{8}', t): dw.append(int(t, 16))
    stats, i = collections.Counter(), 0
    while i < len(dw) - 2:
        v = dw[i]
        if v is not None and (v >> 24) == 0x97:
            a, b = dw[i+1], dw[i+2]
            stats[(hex(v), (b - a) if (a and b and b > a) else '?')] += 1
            i += 3
            continue
        i += 1
    if stats: print(f, dict(stats))
EOF
```

## 1. Command-stream framing (PROVEN)

Every packet in the secondary command stream starts with one leading dword.

* **Register-list packet** — leading dword has opcode byte 0 and equals
  `payload_dwords - 1`.  Payload is: one tag dword per group of up to four
  registers, followed by that group's values.  A tag dword holds four register
  *indices*, **low byte first**, padded with `0x21` = END (`0x20` = pad inside a
  list).  Register address is `0x1000 + index * 4`.
  Verified against `[2][0x21211000][3][4]` (two registers → payload 3 → leading
  2) and the 12-register stream snapshot `[0x0e][0x03020100]… [0x13121110]…`
  (3 tag dwords + 12 values = 15 → leading 14) in CAP `tri8-secs.txt`.
* **Opcode packet** — bits [31:24] opcode, bits [22:17] a 6-bit size field,
  bits [15:0] `payload_dwords - 1`.  The arithmetic is literally
  `((n - 1) & 0x3f) << 17` in every builder found:
  KRN `mtx1017+0x67e` (`0x6746`–`0x676d`), KRN `mtx1814` (`0x43335`),
  PSER `0x1804ee`, PSER `0x137825`, PSER `0x135852`.

### 1.1 Draw-mode field

Opcode byte = `0b1mm1_0011` families.  Bit 31 is the client-path flag, bit 30 a
vertex-layout flag, bits [28:26] the draw mode, bits [25:24] a constant `0b11`.
All builders compose it as `<base> | ctx[0x42c]`, where `ctx[0x42c]` is
`0x80000000` (→ `0x97`) or `0xc0000000` (→ `0xd7`).

| mode | base | client | KRN builder | operands |
|---|---|---|---|---|
| 0 | `0x03` | `0x83` | `mtx3063` @0x6d80 | inline vertex dwords |
| 1 | `0x07` | `0x87` | `mtx1767`/`mtx1796` | inline 16-bit indices |
| 2 | `0x0b` | `0x8b` | `mtx2103` @0x5f10 | inline 32-bit indices |
| 3 | `0x0f` | **`0x8f`** | `mtx3981` @0x5d30 | `first`, `last` — **ordinals** into the bound streams |
| 5 | `0x17` | **`0x97`** | `mtx1017` @0x60e0 | `start`, `end` — **bus addresses** of the vertex bytes |
| 6 | `0x1b` | `0x9b` | `mtx1443` @0x5db0 | 16-bit index array, start/end addresses |
| 7 | `0x1f` | `0x9f` | `mtx3249` @0x5e30 | 32-bit index array, start/end addresses |

The 13-entry-per-mode dispatch tables live in KRN `.data` at section offsets
`0x120`, `0x1a0`, `0x220`, `0x2a0`, `0x320`, `0x3a0`, `0x420`, `0x520`
(`objdump -r $KRN` → `RELOCATION RECORDS FOR [.data]`), indexed by primitive
0..12.  Mode 5's table (`.data+0x3a0`..`0x400`) is `mtx1017` thirteen times —
**the same builder for every primitive**.

### 1.2 The 0x97 packet, exactly

```
dword 0 : 0x97000000 | ((dwordsPerVertex - 1) & 0x3f) << 17 | 0x0001
dword 1 : bus address of the first vertex byte
dword 2 : bus address one past the last vertex byte   (EXCLUSIVE)
```

PROVEN twice over:

* KRN `mtx1017` at `0x6746`:
  `eax = [rsp+0x98]; eax--; eax <<= 0x11; eax &= 0x7e0000;`
  `eax |= [r15+0x42c]; eax |= 0x17000001;` then `[r9]=eax`, `[r9+4]=start`,
  `[r9+8]=end`.  `[rsp+0x98]` is accumulated at `0x63b2` as
  `Σ over bound streams of (stride >> 2)`, i.e. dwords per vertex.
* PSER `0x1804ea`: `ecx = [esi+0xf4]; ecx--; ecx &= 0x3f; ecx <<= 0x11;`
  `ecx |= 0xd7000001;` `[eax]=ecx; [eax+4]=edx; [eax+8]=edi`.

The size field is *dwords per vertex*, not a vertex count.  Capture proof
(CAP, the scan above):

| header | byte1 | dwords/vertex | observed range lengths (bytes) |
|---|---|---|---|
| `0x97080001` | 0x08 | 5 (20 B) | 20, 40, 60, 80, 120, 180, 300, 480 |
| `0x970a0001` | 0x0a | 6 (24 B) | 72, 216, 360, 576 |
| `0x970c0001` | 0x0c | 7 (28 B) | 84 |
| `0x970e0001` | 0x0e | 8 (32 B) | 64, 448, 512 |

Every length is an exact multiple of the stride implied by byte 1.  The low 16
bits are `1` in every single 0x97 packet in every capture: a 0x97 packet always
carries exactly **one** range.

### 1.3 There is no primitive count and no end-of-primitive marker

Searched and not found: no count field beyond the two above, no terminator
dword, no per-primitive register.  The number of primitives is
`(end - start) / stride` divided by the primitive's vertex count, and the
primitive type comes solely from PRIMWORD.  **PROVEN by construction** — the
builders write exactly three dwords and nothing else.

Mode 3 (`0x8f`) is the one opcode that can carry several ranges: the ICD
appends `(first,last)` pairs into one open header and the flush decrements once,
so `count = 2N - 1` for N ranges.  KRN's own `mtx3981` never uses that: it emits
`header = ctx[0x42c] | 0x0f000001`, `first = 0` (hardcoded, `0x5d6e`),
`last = arg`.  Its size field is **zero** — mode 3 takes its stride from
register `0x1040`, not from the header.

## 2. The per-draw register state, and what it is not

### 2.1 What the vendor writes per draw (PROVEN, CAP + KRN)

A complete vendor draw in CAP `tri8-secs.txt` around `0x19520da4`:

```
00000002 21211000 00000003 00000004      ; 0x1000 = 3   0x1040 = 4
<120 bytes of vertex data, 6 x 20 B>     ; lives INLINE in the same buffer
97080001 19520db4 19520e2c               ; 6 vertices = TWO triangles
00000001 2121212c 0006305c               ; 0x10b0 = TRIANGLES, for the next draw
0000000e 03020100 00000002 ...           ; 8 stream bases + 4 strides snapshot
```

That is the whole per-draw state: **`0x1000`, `0x1040`, `0x10b0`**.  KRN
`mtx1017` writes only `[2][0x21211000][class][4]` (`0x659f`–`0x65ae`), where
`class` is the command buffer object's memory class at `[r10+0x34]`; KRN
`mtx2393` writes the three-register form `[3][0x212c1000][class][4][primword]`
(`0x3c6bc`–`0x3c6d0`); KRN `mtx693`/`mtx1814` write the same triple with
`0x0002305c` (`0x4389c`, `0x434e7`).

`0x1040` is **4** in every single vendor draw on the copy path — 1311 of 1311
writes in CAP `tri8-secs.txt`.  It is a selector, not a byte stride; the byte
stride travels in the packet header's size field.  Our driver already does this
(MESA `p3d_state.c:832`).

### 2.2 PRIMWORD (register `0x10b0`, index 0x2c) — complete decode (PROVEN)

KRN `mtx3319` @`0xb610`, three instructions and two tables:

```
b616: edx = mtx734[p];   word = (word & ~0x7f) | (edx & 0x7f)
b62b: edx = mtx3178[p];  word = (word & ~0x7000) | ((edx & 7) << 12)
b640: byte2 |= 2                              -> bit 17 ALWAYS set
b64e: if (ctx[0x430] == 1) byte2 |= 6 else byte2 &= ~4   -> bit 18 conditional
```

Tables, read as bytes (`objdump -s -j .data --start-address=0x14c20 …`):

```
14c20  20000000 20000000 3d000000 3d000000   mtx734  (A, low 7 bits)
14c30  3d000000 5c000000 5c000000 5c000000
14c40  5c000000 5c000000 5c000000 3d000000
14c50  5c000000
14c60  00000000 00000000 01000000 02000000   mtx3178 (B, bits 14:12)
14c70  02000000 03000000 04000000 05000000
14c80  05000000 06000000 07000000 01000000
14c90  03000000
```

`PRIMWORD = A[p] | (B[p] << 12) | 0x20000 | (bit18 ? 0x40000 : 0)`

| p | A | B | primitive | bit17 only | bit17+18 |
|---|---|---|---|---|---|
| 0,1 | 0x20 | 0 | POINTS | `0x00020020` | `0x00060020` |
| 2 | 0x3d | 1 | LINES | `0x0002103d` | `0x0006103d` |
| 3,4 | 0x3d | 2 | LINE_LOOP / LINE_STRIP | `0x0002203d` | `0x0006203d` |
| **5** | 0x5c | 3 | **TRIANGLES** | `0x0002305c` | `0x0006305c` |
| **6** | 0x5c | 4 | **TRIANGLE_STRIP** | `0x0002405c` | `0x0006405c` |
| **7** | 0x5c | 5 | **TRIANGLE_FAN** | `0x0002505c` | `0x0006505c` |
| 8 | 0x5c | 5 | POLYGON (= fan) | `0x0002505c` | `0x0006505c` |
| **9** | 0x5c | 6 | **QUADS** | `0x0002605c` | `0x0006605c` |
| 10 | 0x5c | 7 | QUAD_STRIP | `0x0002705c` | `0x0006705c` |
| 11 | 0x3d | 1 | decomposed lines | — | — |
| 12 | 0x5c | 3 | decomposed triangles | — | — |

Capture cross-check (CAP `tri8-secs.txt`, scan of every `0x10b0` write):
`0x0006305c` ×412, `0x0006405c` ×315, `0x0006505c` ×148, `0x0006605c` ×406,
`0x0002305c` ×25.  All four triangle-class topologies really are used and all
four match the table exactly.  This settles question 5: strips are
`0x0006405c`, fans `0x0006505c`, quads `0x0006605c`, and **the composer has no
chip-class test whatsoever** — `mtx3319` contains no reference to the
device word.

The low byte is a rasteriser class (0x20 point, 0x3d line, 0x5c triangle); bits
[14:12] the topology within that class; bit 17 unconditional; bit 18 tied to
`ctx[0x430] == 1`, and `mtx3661` @`0xbef0` sets `ctx[0x430] = 2` for the
packet/direct-fetch context, so that context emits `0x0002305c`.

### 2.3 `0x1060`/`0x1064` — the same registers MESA calls VTXCAP

MESA `reference/parhelia3d.h:39-40` names `0x1060` `P3D_R_VTXCAP` and `0x1064`
`P3D_R_VTXCAP_HI`.  These are the "bound range" / fetch floor and limit
registers; there is no separate pair.  (Note for the task brief: the offsets
`0x1100/0x1104/0x1108/0x110c/0x1114` used there are *not* the stream registers.
`0x1100`/`0x1104` are the WARP program entry pair and `0x1114` is the
undecoded attribute-map register `ATTR_MAP0`, baseline `0x00287210`.  The
vertex-fetch registers are `0x1000`, `0x1040`, `0x1060`, `0x1064` and PRIMWORD
is `0x10b0`.)

Vendor values in CAP, per capture:

| capture | stride | `0x1060` | `0x1064` |
|---|---|---|---|
| `base-secs.txt`, `tri8-secs.txt` | 20 B | `0x000280cc` (×27) and `0` | `0xffffffff` and `0` |
| `idx16tri8-secs.txt` | 24 B | `0x00021600` (×6) and `0` | `0xffffffff` and `0` |

`0x21600 × 24 = 0x321000`, `0x280cc × 20 = 0x31c2f0` — both are
`stagingBufferBytes / stride`.  Our `p3d_vtxcap()` (MESA `p3d_state.c:219`)
returns `0x321000 / vertexBytes`, which reproduces the 24-byte case exactly.
These are stream-binding state, not per-draw state, and the vendor re-emits them
only when a stream rebinds.

### 2.4 `0x1130` PIPECTL

Sole emitter is KRN `mtx3400` @`0x439c0`; it writes `[1][0x2121214c][value]`
where `value = shadow[0x5f0] | (0x200 if the WARP program could not be made
memory resident)`.  That condition is `[dev+0xa3114] == 0 && [dev+0xa3121] != 0`,
and `0xa3121` is set at KRN `0x9f6f9` when the WARP program heap would exceed
`0x17ff` bytes — **a program-size condition, not a chip-class condition**.
`0x1130` therefore contributes nothing to the multi-primitive question.

## 3. Chip class: what "LX" is, and where it is tested (PROVEN)

KRN `mtx1916` @`0x40a70` maps the PCI id (and a sub-variant selector) to a
single bit stored at `dev+0x20`, mirrored at `dev+0x2440`:

| PCI id `0x102b:` | variant | class bit |
|---|---|---|
| 0x0525 / 0x2527 | any | `0x00000001` |
| 0x0527 | 0 / 1 / 2 / 3 / else | `0x02` / `0x04` / `0x08` / `0x20` / `0x10` |
| 0x0528 | 1 / 2 / 6 / else | `0x40` / `0x80` / `0x200` / `0x100` |
| 0x2537 | 1 / 2 / else | `0x400` / `0x800` / `0x1000` |
| 0x2538 | 0 / 1 / else | `0x2000` / `0x4000` / `0x8000` |
| **0x2539** | 0 / 1 / else | **`0x10000` / `0x20000` / `0x40000`** |

The mask `0x7fc00` (bits 10..18) is therefore exactly
`{0x2537, 0x2538, 0x2539}` — the P650/P750/**P690** family.  56 sites in KRN
test it.  Mapping every site to its enclosing function:

```
mtx2372 mtx1538 mtx1858 mtx2954 mtx2060 mtx1602 mtx2137 mtx1375 mtx3574 mtx1389
mtx398  mtx1855 mtx1214 mtx2572 mtx2393 mtx2663 mtx1381 mtx1876 mtx1712 mtx1219
mtx625  mtx3721 mtx2744 mtx1464 mtx3952 mtx3890 mtx3222 mtx2400 mtx958  mtx3726
mtx1918 mtx2579 mtx1414 mtx2124 mtx1723 mtx1567 mtx2051 mtx2696
```

### 3.1 Answer to question 3: the draw builders are class-independent

**None** of `mtx1017` (0x60e0–0x7030), `mtx3981` (0x5d30), `mtx1443` (0x5db0),
`mtx3249` (0x5e30), `mtx2103` (0x5f10), `mtx3063` (0x6d80), `mtx1580` (0x5d20)
or `mtx3319` (0xb610) contains a chip-class test.  Neither does the
vertices-per-packet arithmetic: the split tables at KRN `.rodata+0x198`
(how many vertices fit the remaining command space, rounded down to the
primitive size) and `.rodata+0x200` (per-primitive advance) are indexed by
primitive only:

```
.rodata+0x198 : p0 6c2c  p1 6c2c  p2 6bf4  p3 6c2c  p4 6bca  p5 6bd9  p6 6ba7
                p7 6c2c  p8 6c2c  p9 6c0f  p10 6bf4 p11 6bf4 p12 6bd9
```

`0x6bd9` (TRIANGLES) is `verts = (space / (3*dwPerVertex)) * 3`; `0x6c0f`
(QUADS) rounds to 4; `0x6ba7` (TRIANGLE_STRIP) rounds down to even; `0x6bca`
(LINE_STRIP) subtracts one; `0x6c2c` (points, line loops, fans, polygons) does not
round.  There is **no maximum-vertices-per-packet constant at all** in the
kernel builder — the only bound is the remaining room in the command buffer
(remaining command dwords minus 7, KRN `0x655f`).

The Windows P690 driver behaves the same way: PSER `0x137701`–`0x13784b` copies
a whole run of vertices in a loop and then emits **one** `0x17000001`-family
header covering all of them, with `[edi+0x37c]` supplying the client/layout
bits.  There is no class test in that loop either.

**Conclusion (PROVEN for the code, INFERRED for the silicon):** the vendor does
*not* fall back to one primitive per packet on the LX family.  The same code
emits multi-primitive ranges on 0x2539 as on 0x0528.  Our P690 failure is a
state or memory-framing problem, not a missing chip capability.

### 3.2 The retained P690 vendor draw

CAPP, the only draw in the trace (line 1262):

```
SUBMIT 22 REGION off=0x8993000 changed=24 span=0x0-0x18
  8f000001  00000000  00000004        ; mode 3, ordinals [0,4)
  00000002  21212425  00000002  1a9bc000  ; 0x1094 = fence 2, 0x1090 = write-back addr
```

Four vertices from the bound streams.  Four is not a multiple of three, so this
retired P690 draw was **not** a plain triangle list; with POINTS it is four
primitives, with LINES two, with TRIANGLE_STRIP or TRIANGLE_FAN two triangles,
with QUADS one quad.  In every reading except LINE_STRIP-as-one-primitive it is
more than one primitive, or at least more than three vertices, in one packet on
a P690.  PRIMWORD is not visible in this trace, so this is **INFERRED**, but it
is the strongest single piece of evidence that the P690 consumes whole ranges.

## 4. LX-only state the vendor programs that our driver does not

Scanning every `0x7fc00`-gated branch for register-list tag dwords turns up a
small set of registers the vendor treats differently on the LX family.  Only
one of them is in the geometry/raster path and is a value we get wrong.

### 4.1 Guard band `0x12f4` is clamped down by one unit on LX — PROVEN

KRN `mtx2572` @`0x3c270`:

```
3c273: test DWORD PTR [rax+0x2440],0x7fc00     ; LX family only
3c27d: je   3c2b0                              ; non-LX: leave the value alone
3c27f: rax = [rdi+0x50]
3c283: test BYTE PTR [rax+0x80],0x3
3c28c: edx = [rax+0x7f8]                       ; the 0x12f4 shadow
3c292: cmp edx,0x42000000  -> 3c2b6: [rax+0x7f8] = 0x41f80000   ; 32.0f -> 31.0f
3c29a: cmp edx,0x41800000  -> 3c2a6: [rax+0x7f8] = 0x41700000   ; 16.0f -> 15.0f
```

KRN `mtx958` @`0x69650` chooses, at `0x6974a`, the unclamped value, also only on LX:
`[rbx+0x12c] < 3 ? 0x42000000 : 0x41800000`.
KRN `mtx2393` @`0x3c538` emits it: on LX `[2][0x2121bdc0][0x002010cc][0x41f80000]`
→ `0x1300 = 0x002010cc`, `0x12f4 = 0x41f80000`; the non-LX branch at `0x3c8d8`
emits only `[1][0x212121c0][0x000010cc]` and never touches `0x12f4`.
KRN `mtx2663` @`0x3d911` has a second LX-only site writing
`[0x12f4] = 0x41800000` together with `0x1300`.

The 0x0528 capture writes `0x12f4 = 0x42000000` (32.0f) — CAP `tri8-secs.txt`,
`[1][0x212121bd][42000000]` — and MESA `p3d_state.c:617` copies that value
verbatim.  **On the P690 the vendor would program `0x41f80000` (31.0f).**
This is the only proven LX-only value in the raster path that our driver gets
wrong.  Whether a 32.0 guard band makes the setup unit drop primitives after
the first is not established.

### 4.2 DWGCTL `0x1300` bit 21

LX `0x002010cc` vs non-LX `0x000010cc` in `mtx2393`.  Our `p3d_dwgctl()` base
`0x00a60ec4` already has bit 21 set, so this one is already satisfied.

### 4.3 Ganged texture pipes are not involved

The LX-only branch in KRN `mtx1381` @`0x3f8da` emits an eight-register opcode-9
packet where the non-LX branch at `0x3faf0` emits four registers, and the
LX-only combiner merge (`0x1264`/`0x126c`) is the known ganged-pipe fix.  All of
that is fragment/texture state and cannot gate primitive assembly.

## 5. What multi-primitive packets look like on the 0x0528 (PROVEN, CAP)

Full scan of the capture set (command above).  Range length divided by the
stride implied by the header gives the vertex count:

| capture | header | range | vertices | triangles | count |
|---|---|---|---|---|---|
| `idx16tri8-secs.txt` | `0x970a0001` | 576 | 24 | **8** | 6 |
| `idx16tri8-secs.txt` | `0x970a0001` | 72 | 3 | 1 | 185 |
| `tri8-secs.txt` | `0x97080001` | 480 | 24 | **8** | 6 |
| `tri8-secs.txt` | `0x97080001` | 120 | 6 | **2** | 406 |
| `flat-secs.txt` | `0x97080001` | 480 | 24 | **8** | 404 |
| `flat-secs.txt` | `0x97080001` | 300 | 15 | 5 | 2 |
| `texmipnear-secs.txt` | `0x970a0001` | 576 | 24 | **8** | 122 |
| `lines-secs.txt` | `0x970e0001` | 512 | 16 | 8 lines | 158 |

The 2-triangle packet dissected above is the minimal working case and the
direct analogue of our failing `PROBE_GROUP=2`:

```
00000002 21211000 00000003 00000004     ; 0x1000 = 3 (PCI bus master), 0x1040 = 4
<0x78 bytes: 6 vertices x 20 bytes>     ; at 0x19520db4 .. 0x19520e2b
97080001 19520db4 19520e2c              ; end == address of this very packet
```

Note that `end` equals the address of the draw packet itself: the vertices sit
immediately in front of the packet, in the same buffer, and the exclusive end is
the next byte.  KRN `mtx1017` builds it that way on purpose — it `LocMemCpy`s the
vertices into the command buffer at the current cursor (`0x66e4`), takes `start`
from the cursor before the copy (`0x6654`) and `end` from the cursor after
(`0x6730`).  **Our driver instead points `start`/`end` at a separate pinned
CREATE_VERTEX object.**  That is the largest structural difference between our
packet and every vendor packet that has ever been observed to carry more than
one primitive.

## 6. What is not determined

* Why the P690 stops after the first primitive of a range.  Nothing in the
  binaries limits it, and nothing in the binaries programs a per-packet
  primitive count.
* Whether the retained P690 `0x8f000001, 0, 4` draw was a quad, a strip or a
  fan.  The trace does not contain the matching `0x10b0` write.
* Whether the guard-band clamp of §4.1 has any effect on primitive assembly.
* What `[rax+0x80] & 3` (the second condition on the guard-band clamp) selects.
* What `0x1040 = 4` actually means to the fetch unit.  It is constant in every
  copy-path draw on both families, so it cannot be a byte stride, but no code
  derives it from anything.

## 7. Hardware experiments, most likely first

All variants below keep the established single-triangle batch state and change
only what is named.  Baseline, as MESA `p3d_emit_draws()` emits it today
(`reference/p3d_state.c:824-846`):

```
00000001 2121212c 0006305c              ; 0x10b0 = TRIANGLES
00000002 21211000 00000003 00000004     ; 0x1000 = 3, 0x1040 = 4
97080001 <start> <end>                  ; 5-dword vertices, end - start = 60*N
```

### V1 — Discriminate "one primitive" from "three vertices": draw one QUAD

Evidence: strong.  The PRIMWORD table (§2.2) is byte-exact and the 0x0528
capture uses `0x0006605c` 406 times.  A quad is **one** primitive spanning four
vertices and two triangles of screen area, so this separates the two competing
explanations in a single run.

```
00000001 2121212c 0006605c              ; 0x10b0 = QUADS
00000002 21211000 00000003 00000004
97080001 <start> <start+0x50>           ; 4 vertices x 20 bytes
```

* Full quad lit → the P690 limit is *per primitive*, and the fix is state that
  restarts primitive assembly, not vertex fetch.
* Only the first half lit, or a single triangle → the limit is in vertex fetch
  at three vertices, and V3/V5 become the priority.

Run the same test with `0x0006505c` (TRIANGLE_FAN, 4 vertices = 2 triangles) and
with `0x00060020` (POINTS, 4 vertices = 4 points, `end - start = 0x50`).  Four
points appearing would prove the fetch reads the whole range and the problem is
triangle assembly specifically.

### V2 — Put the vertices inline in the command stream, immediately before the packet

Evidence: strong and structural.  Every vendor packet that carries more than one
primitive has `end` equal to the address of its own header (§5), because
`mtx1017` copies the vertices into the command buffer.  Our separate
CREATE_VERTEX object is the one framing difference.

Emit, inside one secondary section, in this order: the
`[2][0x21211000][class][4]` register packet, then `N*3*5` vertex dwords, then
`97080001 <addr of first vertex dword> <addr of the 0x97 header>`.  `class` is
the memory class of the command buffer itself.  The kernel already relocates
0x97 address pairs (`parhelia_submit.c`, `case 0x97`), so this needs a
relocation that names the section's own base plus an offset rather than a GEM
handle.

### V3 — Give `0x1060`/`0x1064` the exact bound range for the packet

Evidence: medium.  The registers are proven to be fetch limit/floor and the
vendor's values are `bufferBytes/stride` and `0`/`0xffffffff` (§2.3); ours is a
constant derived from a different buffer size.  Cheap to falsify.

```
00000002 21211918 <limit> 00000000      ; 0x1060 = limit, 0x1064 = 0
```

with, in three separate runs, `limit = 3*N` (exact vertices in the packet),
`limit = (end-start)/stride`, and `limit = 0xffffffff`.
Tag byte order note: index 0x18 → `0x1060`, index 0x19 → `0x1064`, low byte
first, so the tag dword is `0x21211918`.

### V4 — Guard band 31.0 instead of 32.0

Evidence: PROVEN LX-only vendor behaviour (§4.1), unproven relevance.
One register, one run.

```
00000001 212121bd 41f80000              ; 0x12f4 = 31.0f  (was 0x42000000)
```

If the batch also runs with `0x11e8` in its Windows form, try
`0x12f4 = 0x41700000` (15.0f) as the second variant, matching the
`mtx958`/`mtx2572` pair for the other branch.

### V5 — Pad the vertex to a power-of-two stride

Evidence: INFERRED.  The 0x0528 carried 16 vertices in one packet at a 32-byte
stride (`0x970e0001`, 512-byte range, `lines-secs.txt`), and a 32-byte stride is
the only one for which a range never straddles a cache line partially.  Pad the
5-dword vertex to 8 dwords and re-run `PROBE_GROUP=2` and `=4`:

```
00000001 2121212c 0006305c
00000002 21211000 00000003 00000004
970e0001 <start> <start+0xC0>           ; 6 vertices x 32 bytes, 2 triangles
```

The attribute descriptors must be re-emitted for the padded layout; the
descriptor dword offset field is bits [11:4] (MESA `p3d_state.c:224-226`).

### V6 — Slack after the range

Evidence: INFERRED, explains the N=4 hang.  If the fetch unit prefetches past
`end`, a range that ends near the end of a mapped page would fault or stall.
Allocate the vertex object with at least 4 KiB of mapped slack after the last
vertex, place the range so that `end + 0x400` is still inside the object, and
re-run `PROBE_GROUP=4`.  If N=4 stops hanging, the problem is prefetch framing,
not primitive assembly.

### V7 — PRIMWORD bit 17 clear

Evidence: weak.  `mtx3319` always sets bit 17, so `0x0004305c` is not a value
the kernel can produce; it is, however, what the ICD's own composition
(`0x00040000 | topology<<12 | A`) yields and it is the one triangle PRIMWORD
variant not yet tried on the P690 (bits 17+18 and bit 17 alone both were).

```
00000001 2121212c 0004305c              ; bit 18 only
00000001 2121212c 0000305c              ; neither bit
```

### V8 — Triangle strip with the smallest useful vertex count

Evidence: weak, but it bounds the hang.  The six-vertex strip hangs; try four
and five vertices with `0x0006405c` to find the exact threshold, and the same
counts with `0x0006505c` (fan).  A hang at four vertices but not at three would
put the boundary at "more than one primitive" rather than at a byte count.

### V9 — Mode 3 with the vendor's own shape

Evidence: medium; it is literally what the retained P690 vendor draw did (§3.2).
Bind the stream properly (`0x1000 = base|3`, `0x1040 = real byte stride`,
`0x1064 = 0`, `0x1060 = vertices in the object`), emit the full eight-base /
four-stride snapshot, `0x10b0 = 0x0002605c` (QUADS) and

```
8f000001 00000000 00000004
```

`first` must be 0 — `mtx3981` hardcodes it and the P690 capture shows 0.  If a
four-vertex quad renders completely through mode 3 but not through mode 5, the
difference is in the address-range fetch path, not in primitive assembly.

### Order of execution

V1 first: it is one register change and it tells the next three experiments
apart.  Then V2 (structural, the largest remaining difference from every
known-good vendor packet), then V3 and V4 (one register each), then V5/V6 for
the hang, then V7–V9.
