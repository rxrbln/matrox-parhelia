Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# Our own WARP vertex program: the annotated vendor Gouraud program, our
# replacement for it, and the exact binding recipe

This document closes the last dependency of the 3D path on vendor microcode for
the *per-vertex* stage.  It identifies which of the eight live program slots is
the Gouraud (position + per-vertex colour) program, annotates it bundle by
bundle from the live capture, and derives a shorter replacement that uses
**absolute constant indices only**, so no vendor control program has to be run
first to install base registers or `a0`.

Sources: `captures/live-ring-0528/warp-programs/` (live class-3 heap dump),
`captures/live-ring-0528/decoded.txt` + `secs.txt` (the live ring),
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (binary only, via
`objdump`).  Tags: **[HW]** live capture, **[BIN]** read out of a binary at the
quoted offset, **[INF]** inference.

Prerequisites: `docs/parhelia-warp-isa-complete.md` (bit fields),
`docs/parhelia-warp-programs-decoded.md` (the eight slots),
`docs/parhelia-block-file-constants.md` (the constant file),
`docs/parhelia-warp-binding.md` (the register file).

---

## 0. Headline

1. **The Gouraud program is slot `16c06200`, entry `0x07957783`, bundle 96** —
   the only one of the eight live slots that is a per-vertex program (`f2b=1`,
   reads `v#`, writes `o0/o1/o2/o3..o6/o11`).  The other seven are control /
   clipper class (`f2b=0`, `ldreg`/`streg`/`br`).  It is also the *last*
   `0x1100` write before the draw, with MODE `0x20000002` — the per-vertex
   bind — while the control programs are bound with MODE `0x20000028`. [HW]
2. **It applies the full transform**: MVP `dp4` ×4 from `c[a0.x+0..3]`
   (`a0.x = 0` → block slots `0x000..0x003`), then a perspective divide
   (op `0x89` = `s1 × rcp(s0.w)`), then the viewport scale/offset from
   `c[b1+0]`/`c[b1+1]` (`b1 = 0x160` → slots `0x160`/`0x161`), with a
   float-to-fixed grid snap added and subtracted around the `mad`.  It does
   **not** expect pre-transformed input; `o0` leaves in **window pixels**, not
   clip space. [HW]
3. **`B0` = transformed window position, `B1` = colour pass-through** —
   confirmed: the last arithmetic bundle writes `o0` from the viewport `mad`,
   and bundle 101 is `add o1 ← v5 + 0`, a straight copy of the fetched colour
   attribute. [HW]
4. **Our replacement is 11 bundles (132 bytes)**, in
   `captures/live-ring-0528/our-gouraud.warp.hex`.  It drops the four texcoord
   moves, the secondary colour, the grid snap and — the point of the exercise —
   all relative addressing: the matrix is `c0..c3` and the viewport is read from
   two absolutely-addressable duplicate slots `c8`/`c9`.
5. **Entry value: `entry = (bus_address / 3) | 3`** — equivalently
   `4 × (bus/12) | 3`; verified 9/9 against `entries.txt`, and independently by
   the fact that dumping `(entry & ~3) × 3` out of `/dev/mem` yields valid,
   `END`-terminated microcode in every case. [HW]  The address field is
   `entry[31:2]` = an **instruction index**, so the program image must start at
   a bus address that is a multiple of 12 — see §5.4, this bites on
   page-aligned buffers.
6. **`0x00000002` is the "not bound" sentinel**, not a count and not a
   selector.  All nine entry shadow words are initialised to 2 at
   `mtx_parhelia.o` `.text+0x4210c..0x422b2`; real entries are always multiples
   of 4, so 2 is a value no program can produce. [BIN]

---

## 1. Which slot, and why the other seven are not it

`tools/dump-warp-programs.sh` dumped every 0x600 slot the live ring referenced.
`entries.txt` maps entry → bus → slot base; each program body begins at bundle
96 of its slot (the linker pads so the last fragment starts on a 32-instruction
boundary), except the two stage entries into `16c06800`.

| slot | entry | class | first bundles | verdict |
|---|---|---|---|---|
| `16c06200` | `07957783` | `f2b=1`, reads `v0/v5/v6/v7..v10` | `add T0,v0` + 4×`dp4` | **the per-vertex program** |
| `16c05000` | `07957183` | `f2b=0` | `getc`, `mov T#,v#.yyyy`, `sync0d`, `end` | kind 3, entry-pointer installer |
| `16c05600` | `07957383` | `f2b=0` | `getidx`, `mova`, `subi`, `br` | kind 0, constant-file loader |
| `16c05c00` | `07957583` | `f2b=0` | `mov T#`, `mova`, `getc`, `streg [0x0e8..0x0fc]` | kind 1, base-register installer |
| `16c03e00` | `07956b83` | control | `movi`, `streg`, `ldreg`, `andi/ori/shli` | stage-6 program |
| `16c06800` | `079579bf` / `…cf` | control | 15×pad, `movi`, `br`, `ldreg/streg` | stage 2/4 and 3/5 |
| `16bffc00` | `07955583` | `f2b=0` arith | `mul`/`add`/`movsat` on `o[b3+n]`, `c[b1+n]` | resident clipper/interpolator |
| `16c02000` | `07956183` | control | `mova`, `sync0d`, `movi`, `streg` | resident stage program |

The ring settles it independently.  In `decoded.txt` the four `0x1100` writes of
a draw are, in order, `…7183`, `…7383`, `…7583` (each MODE `0x20000028`, each
followed by the `0x41` block-load packets it consumes) and finally

```
0x1374=00000000 0x10f0=20000002 0x10f0=20000002 0x1100=08a57783 0x10f8=00000002
```

MODE `0x20000002` and `0x10f8 = 2`.  `…7783` is the low half of the vertex
program's entry (this ring capture is from a different boot than the slot dump,
so the heap base differs — `08a5xxxx` vs `0795xxxx` — but the low 16 bits, i.e.
the offset inside the heap, are identical). [HW]

---

## 2. The vendor Gouraud program, annotated

Slot `16c06200`, bundles 96..108.  Disassembly by `tools/parhelia-warpdis2.py
-e 96 -s`; every word is reproduced bit-for-bit from mnemonics by
`tools/parhelia-warpasm.py --gen transform --matrix a0 --tex 4` (13/13
`IDENTICAL`), which is the proof that the field map below is the right one.

```
 96: 00003e00 00440000 b89a2249  add  T0 ← v0 + v0.0000
        ; T0 = the fetched position (x,y,z,w).  v0 is copied through the latch
        ; file because every vendor transform block feeds dp4 from T, never
        ; from v directly.  src1 swizzle 0000 = the constant 0.0.

 97: 1874a23f 28022303 bb9a21a2  dp4  r2.w ← T0 · c[a0.x+3] | mov o3 ← v7
 98: 1084924f 28022303 bb9a21a2  dp4  r2.z ← T0 · c[a0.x+2] | mov o4 ← v8
 99: 08948a5f 28022303 bb9a21a2  dp4  r2.y ← T0 · c[a0.x+1] | mov o5 ← v9
100: 00a4866f 28022303 bb9a21a2  dp4  r2.x ← T0 · c[a0.x+0] | mov o6 ← v10
        ; r2 = clip-space position = MVP · v0, one row per component.
        ; src1 relative: f1i=5 (src1), idxsel=1 (a0), relcomp=0 (a0.x); a0=(0,1,2,3)
        ; is installed by the kind-1 program, so a0.x = 0 and the rows are the
        ; absolute block slots 0x000..0x003.  Note f1g=0 and extsel=1/exthi=0 —
        ; the kernel relocator's inert encodings, not the D3D assembler's.
        ; The paired secondaries copy texcoord 0..3 unconditionally.

101: 0e187c1f 32440215 b89a2249  add  o1 ← v5 + r1.0000 | mov T1 ← c[b1+1]
        ; *** B1 = colour pass-through ***  o1 = v5, the diffuse attribute
        ; (fetch descriptor 0xa1c50020: 3 components at stream dword 2, slot 5).
        ; The "+ r1.0000" is the library's way of spelling a move with the
        ; primary ALU; r1 is scratch and the swizzle selects the constant 0.
        ; Secondary: T1 = c[b1+1] = c[0x161] = viewport offset (x0+W/2, y0+H/2,
        ; (zn+zf)/2, H).  A plain mov (op 1) to T1 does not disturb a0 (op 9).

102: 12257ebf 00440289 c49a22a2  smul r5 ← r2, r2.xyz1 | mov o11 ← r2
        ; op 0x89 = s1 × rcp(s0.w)  =>  r5 = (x/w, y/w, z/w, 1/w) = NDC.
        ; o11 keeps the clip-space position for the clipper stage.

103: 0808be00 00440019 b89a2249  add  o2 ← v6 + r1.0000
        ; secondary colour; unbound here, so v6 is the default attribute vector.

104: 40004e00 ea540107 b89a2246  add  T1.xy ← T1 + c[b1+8].ww00     ; w1[21:20]=1
105: 2c105e00 22440181 bb6a2222  mad  T1.xyz ← c[b1+0].xyz1 · r5 + T1.xyz0
106: 42583d58 ea540507 b99a2246  sub  o0 ← T1 − c[b1+8].ww00 | sop2 r5.w ← r5
        ; *** B0 = transformed position ***
        ;   o0.xyz = c[0x160].xyz · ndc.xyz + c[0x161].xyz
        ;   o0.w   = c[0x161].w  (= the viewport height; the .ww00 swizzle
        ;            contributes 0 to .w in both the add and the sub)
        ; with a constant K = c[b1+8].w added before the mad and subtracted
        ; after it — the classic add-magic / subtract-magic float-to-fixed grid
        ; snap.  Whether the slot really is 0x168 or 0x178 turns on w1[21:20];
        ; see docs/parhelia-block-file-constants.md §5.  Either way it is a
        ; no-op up to rounding, so our program omits it.
        ; sop2 r5.w: nothing reads r5 afterwards. [OPEN]

107: 00000200 00040000 14c00000  end
108: 00080200 00440000 301a21a2  wra   (pad, f2b=0)
```

### 2.1 Ports, definitively

| port | written by | value |
|---|---|---|
| **B0** (`o0`) | bundle 106 | window-space position, `.w = viewport height` |
| **B1** (`o1`) | bundle 101 | `v5` verbatim — **colour pass-through, confirmed** |
| B2 (`o2`) | bundle 103 | `v6` (secondary colour) |
| B3..B6 (`o3..o6`) | secondaries of 97..100 | `v7..v10` (texcoord 0..3) |
| B11 (`o11`) | secondary of 102 | clip-space position, for the clipper |

Inputs read: `v0` (position), `v5` (colour), `v6`, `v7..v10`.  Constants read:
`c[0..3]` (MVP), `c[0x160]` (scale), `c[0x161]` (offset), `c[b1+8]` (snap).

**So: MVP + perspective divide + viewport, from a non-pre-transformed vertex.**
Our program must do the same, or the triangle lands in the wrong space.

---

## 3. Our program

### 3.1 Design

Three things in the vendor program need vendor *control* microcode to have run
first:

* `c[a0.x+k]` needs `a0` — installed by the kind-1 program `mtx251`;
* `c[b1+k]` needs base register 1 = `0x160` — same program;
* the block loads themselves are consumed by the kind-0 loader `mtx1079`.

The first two are avoidable.  The constant operand encoding is a 4-bit index
plus a 3-bit `exthi` extension, i.e. **absolute indices 0..127 only**
(`docs/parhelia-warp-isa-complete.md` §3.5); `0x160` is out of reach absolutely,
which is exactly why the vendor uses `b1`.  The fix is to *duplicate* the
viewport pair into two low block slots and read those.

Slots `0x000..0x0df` are the 32×7 matrix palette.  Palette entry 0 is
`c[0..6]` (4 MVP rows + 3 normal-matrix rows) and `c[7]` is the pad the vendor's
own 8-vec4 load writes as zero.  Palette entry 1 is `c[7..13]` and is only ever
sampled with ≥2 blend matrices, which a plain Gouraud draw never has — so
**`c8` and `c9` are free**, absolutely addressable, and adjacent to the block
the loader already writes.  (`--vp-scale` / `--vp-offset` move them.)

Constant-file requirement for our program:

| slot | value |
|---|---|
| `c[0..3]` | MVP rows, `c[r] = (M[r], M[r+4], M[r+8], M[r+12])` of the GL column-major matrix |
| `c[8]` | viewport scale — the same vec4 as `c[0x160]`: `(W/2, −H/2, (zf−zn)/2, 1)` |
| `c[9]` | viewport offset — the same vec4 as `c[0x161]`: `(x0+W/2, y0+H/2, (zn+zf)/2, H)` |

Write `c[0x160]`/`c[0x161]` as well: the raster/clip block reads that pair
independently of the vertex program.

### 3.2 Source

`tools/parhelia-warpasm.py --gen gouraud`, or verbatim:

```
add  T0, v0, v0.0000                  ; T0 = fetched position (x, y, 0, 1)
dp4  r2.w, T0, c3                     ; r2 = MVP * position, one row per
dp4  r2.z, T0, c2                     ;      component; absolute rows, no a0
dp4  r2.y, T0, c1
dp4  r2.x, T0, c0
add  o1, v5, r1.0000 | mov T1, c9     ; B1 = colour; T1 = viewport offset
smul r5, r2, r2.xyz1 | mov o11, r2    ; r5 = r2/r2.w (NDC); o11 = clip pos
mad  T1.xyz, c8.xyz1, r5, T1.xyz0     ; T1.xyz = scale*ndc + offset, T1.w = H
add  o0, T1, r1.0000                  ; B0 = window-space position
end
wra                                   ; pad
```

### 3.3 Encoding — `captures/live-ring-0528/our-gouraud.warp.hex`

```
  0: 00003e00 00440000 b89a2249 | add T0, v0, v0.0000
  1: 1804a200 00440103 bb9a21a2 | dp4 r2.w, T0, c3
  2: 10049200 00440103 bb9a21a2 | dp4 r2.z, T0, c2
  3: 08048a00 00440103 bb9a21a2 | dp4 r2.y, T0, c1
  4: 00048600 00440103 bb9a21a2 | dp4 r2.x, T0, c0
  5: 0e987c1f 00440215 b89a2249 | add o1, v5, r1.0000 | mov T1, c9
  6: 12257ebf 00440289 c49a22a2 | smul r5, r2, r2.xyz1 | mov o11, r2
  7: 2c105e00 004401a1 bb6a2222 | mad T1.xyz, c8.xyz1, r5, T1.xyz0
  8: 08083e00 00440105 b89a2249 | add o0, T1, r1.0000
  9: 00000200 00040000 14c00000 | end
 10: 00080200 00440000 301a21a2 | wra (arith nop)
```

**11 bundles = 33 dwords = 132 bytes.**  12-byte granular by construction, and
0x84 ≤ 0x600, so it fits one slot with room to spare.

Field-level relation to the vendor's words (`--vs`, §2 numbering):

* bundle 0 and 6 are **bit-identical** to the vendor's 96 and 102;
* 1..4 differ from 97..100 only in `idxsel/f1i/f1g/extsel` (absolute `c3..c0`
  instead of `c[a0.x+3..0]`) and in dropping the paired texcoord move;
* 5 differs from 101 only in `src2/relcomp/f1i` (`c9` instead of `c[b1+1]`);
* 7 is the vendor's 105 with `c8` instead of `c[b1+0]`;
* 8 replaces the vendor's `sub o0, T1, snap` by `add o0, T1, 0` — same result
  with the snap omitted;
* the vendor's 103 (`o2 ← v6`) and 104 (the snap add) have no counterpart.

### 3.4 Round-trip

```
$ python3 tools/parhelia-warpasm.py --gen gouraud --hex captures/live-ring-0528/our-gouraud.warp.hex
$ python3 tools/parhelia-warpdis2.py -s captures/live-ring-0528/our-gouraud.warp.hex
```

decodes back to exactly the ten mnemonics above plus the pad; the encoder's
correctness is anchored by the 13/13 bit-exact regeneration of the live vendor
program with `--gen transform --matrix a0 --tex 4 --vs …/slot-16c06200.txt
--vs-entry 96`.

---

## 4. Tool changes

`tools/parhelia-warpasm.py`:

* new generator `--gen gouraud` (§3.2), with `--vp-scale` / `--vp-offset` to
  choose the two duplicate viewport slots (default `c8`/`c9`); it refuses a slot
  ≥ 128, which is not absolutely addressable;
* new `--hex FILE` output, one dword per line — the image format the replay
  tooling and `parhelia-warpdis2.py` both read.

---

## 5. The binding protocol

### 5.1 Where a program lives

The per-vertex program is fetched by the WARP from a **class-3 (PCI /
system-memory) heap**, not from VRAM: the kernel streams each program image
into a linear buffer and hands the hardware an instruction index into it.  The
kernel's own cursor is `engine + 0xa3118`, reset to 0 and advanced by the
program's byte length (`mtx1391` `.text+0x9f6ce`, `+0x9fb0b`) [BIN].

For our own driver the heap is simply our GFP_DMA32 buffer; the requirement is
only that the image is contiguous and its bus address is 12-byte aligned.

### 5.2 The entry value

`mtx1391`'s return path, `.text+0x9fb0b..0x9fb4d` [BIN]:

```
   mov  ecx,[r14+0xa3118]        ; old = byte cursor before this upload
   mov  edx,0xaaaaaaab
   mov  eax,ecx
   add  ecx,[rsp+0x8]            ; cursor += image length
   imul rax,rdx
   shr  rax,0x20
   shr  eax,1                    ; eax = old / 3          (magic division)
   mov  edx,[rcx+rdx*4+0x2c]     ; prog->entryOffset[entryIdx]
   lea  eax,[rax+rdx*4]          ; entry = old/3 + 4*entryOffset
```

`old` is always a multiple of 12, so `old/3 == 4 × (old/12)`:

```
entry[31:2] = instruction index = bus_address / 12
entry[1:0]  = 3
bus_address = (entry & ~3) * 3
```

Verified 9/9 on `captures/live-ring-0528/warp-programs/entries.txt` [HW]:

| entry | `(entry & ~3)·3` | slot dumped from that address |
|---|---|---|
| `07955583` | `16c00080` | valid microcode at `16bffc00` + 0x480 |
| `07956183` | `16c02480` | " |
| `07956b83` | `16c04280` | " |
| `07957183` | `16c05480` | " |
| `07957383` | `16c05a80` | " |
| `07957583` | `16c06080` | " |
| `07957783` | `16c06680` | **the Gouraud program** |
| `079579bf` | `16c06d34` | stage 2/4, bundle 111 of `16c06800` |
| `079579cf` | `16c06d64` | stage 3/5, bundle 115 of `16c06800` |

The low two bits are **3 on every entry this driver writes** (`…83`, `…bf`,
`…cf`), while `mtx1391` alone produces 0 there — so the 3 is contributed
downstream, when the heap base is relocated into the ring.  It is the same
2-bit memory-class field the driver puts in the low bits of every other address
register (`0x1000` vertex-stream base = class 3 = PCI bus-physical, `0x1220`
render-target base = memory space).  Write 3. [HW; class reading INF]

### 5.3 The registers

`reg = 0x1000 + index·4`; index bytes are consumed LSB-first out of the tag
dword, `0x21` ends a tag.  Nine registers can hold an entry [BIN, shadow words
`S[0x598]`, `S[0x59c]`, `S[0x5a0]`, `S[0x5d8]`..`S[0x5ec]`]:

| idx | MMIO | shadow | role |
|---|---|---|---|
| `0x40` | `0x1100` | `S[0x598]` | **primary entry — the per-vertex program** |
| `0x41` | `0x1104` | `S[0x59c]` | companion entry (self-test writes both) |
| `0x3e` | `0x10f8` | `S[0x5a0]` | shader-object slot 2 |
| `0x30` | `0x10c0` | `S[0x5d8]` | stage-0 entry |
| `0x32`..`0x36` | `0x10c8`..`0x10d8` | `S[0x5dc]`..`S[0x5ec]` | stage 2..6 entries |

plus two companions that are not entries:

| idx | MMIO | role |
|---|---|---|
| `0xdd` | `0x1374` | strobe; the data dword is deliberately never written, so the payload is a don't-care [BIN] |
| `0x3c` | `0x10f0` | MODE; written **twice back to back with the same value** before every entry write [BIN] |

MODE values seen [HW]: `0x20000028` for a bind-and-run of a control program
(low nibble 8 = "run once over the following `0x41` packet"), `0x20000002` for
the per-vertex bind, `0x20000000` on the unbind.  `MODE = (x & 0x8fffffff) |
0x20000000` [BIN `.text+0x436ec`, `0x431b1`, `0xa04d8`].

Per *program*, the descriptor carries an **11-entry** offset table at
`prog+0x2c` (`mtx376` `.text+0xa67fa`: `memset(prog+0x2c, 0, 0x2c)`), indices
0..10; the driver uses 0..0xa.  It is all zeros for every canned program, so an
entry offset only matters for a linker image that carries several entry points
(that is how `16c06800` yields both `…9bf` and `…9cf`). [BIN]

### 5.4 `0x00000002`

Not a count and not a selector: **it is the "no program bound" sentinel.**  The
context constructor sets all nine entry shadows to 2 at `.text+0x4210c`,
`0x42116`, `0x42120`, `0x42272`, `0x42281`, `0x4228e`, `0x4229e`, `0x422a8`,
`0x422b2`; the reset paths at `0x42ae3`/`0x42bf3` set them to 0 [BIN].  Real
entries are multiples of 4 (§5.2), so 2 can never collide with one.  In the
live ring it appears as `0x10f8 = 2` beside the vertex bind (shader-object slot
2 unused) and as `0x1100 = 2, 0x10f8 = 2` with MODE `0x20000000` on the unbind
between frames [HW].

### 5.5 The bind packet, literally

From `captures/live-ring-0528/secs.txt` (four identical occurrences, one per
captured draw), the eight ring dwords that bind the per-vertex program:

```
00000006 212121dd 00000000 3e403c3c 20000002 20000002 08a57783 00000002
   count    tag      data     tag      0x10f0   0x10f0    0x1100    0x10f8
            0x1374   (d/c)   3c 3c 40 3e
```

To bind **our** program, replace `08a57783` by our entry and keep everything
else.  The full per-draw ordering (the three control binds, the `0x41` constant
packets, the vertex-format group, the stage entries) is in
`docs/parhelia-warp-programs-decoded.md` §7; with our program the kind-1
base-register packet is no longer needed *for the vertex program*, though the
resident clipper still uses `b0`/`b1`, so keep emitting it until that is tested.

### 5.6 Placement — the alignment trap

`entry[31:2]` is an instruction index, so

```c
/* image must start at a bus address that is a multiple of 12 */
uint32_t pad   = (12u - (bus % 12u)) % 12u;
uint32_t start = bus + pad;                 /* copy the 132-byte image here */
uint32_t entry = (start / 3u) | 3u;         /* == 4*(start/12) | 3          */
```

A 4096-byte-aligned buffer is **not** 12-byte aligned (`4096 % 12 == 4`), so a
program written at page offset 0 produces an entry whose truncated index points
4 bytes short.  Pad to `offset ≡ 8 (mod 12)` inside a page-aligned buffer, or
just apply the formula above and assert `start % 12 == 0`.

Also note the heap is boot-specific: a captured entry is only valid on the boot
that produced it (`docs/parhelia-warp-program-heap.md`).

---

## 6. Constant packets our program needs

Block-load framing (`docs/parhelia-block-file-constants.md` §6.1): for `n`
vec4, `hdr = 0x41000000 | ((n<<17) & 0x7e0000) | ((n*4+3) & 0xffff)`, followed
by the sub-header `[n][first_slot][1][0]` and `n*4` float dwords.

```c
/* c[0..3] : MVP rows, c[r] = (M[r], M[r+4], M[r+8], M[r+12]) */
0x41080013, 0x00000004, 0x00000000, 0x00000001, 0x00000000,
  <16 float dwords>

/* c[8], c[9] : the viewport pair our program reads absolutely */
0x4104000b, 0x00000002, 0x00000008, 0x00000001, 0x00000000,
  <c8 = (W/2, -H/2, (zf-zn)/2, 1)>
  <c9 = (x0+W/2, y0+H/2, (zn+zf)/2, (float)H)>

/* c[0x160], c[0x161] : the same pair where the raster/clip block reads it */
0x4104000b, 0x00000002, 0x00000160, 0x00000001, 0x00000000,
  <same two vec4>
```

For the capture's 320×240 drawable those two vec4 are
`(160, −120, 0.5, 1)` = `43200000 c2f00000 3f000000 3f800000` and
`(160, 120, 0.5, 240)` = `43200000 42f00000 3f000000 43700000`.

Everything else the vendor loads (material, lights, texture matrices, fog,
point size) is unread on the unlit, untextured path; `0x172..0x17e` is raster
state and stays.

---

## 7. Open items

1. **Untested on hardware.**  Every word here is derived from the capture and
   the binaries; nothing in §3 has been executed.  First run: bind the program
   of §3.3 in place of `08a57783` in the §5.5 packet, keep every other register
   and every `0x41` packet as captured, and judge by the full-VRAM colour-diff
   method (`docs/parhelia-render-target-location.md`), not by register reads.
2. **`c8`/`c9` vs the palette.**  Free only while vertex blending is off.  If a
   later path needs the palette, move them with `--vp-scale`/`--vp-offset` to
   any other slot < 128 that the enabled fixed-function blocks do not sample.
3. **op `0x89` = `s1 × rcp(s0.w)`** is [INF, strong] (the `0x8a` sibling is
   `× rsq`, and the position in the dataflow is exactly the perspective
   divide).  Our bundle 6 is bit-identical to the vendor's, so we inherit
   whatever it really does — but a divide-free reading would mean the vendor
   program never divides, which the §4.3 arithmetic of
   `docs/parhelia-block-file-constants.md` rules out.
4. **The grid snap** is omitted.  If edges come out jittery under
   multisampling, add the vendor's `± c[b1+8].ww00` pair back (`--gen transform`
   keeps it) or fold the magic constant into a `c10`.
5. **`sop2 r5.w ← r5`** (vendor bundle 106 secondary) is dropped.  If the
   triangle fails to rasterise with our program but the vendor's works, this is
   the first suspect — it may latch `1/w` for the rasteriser.
