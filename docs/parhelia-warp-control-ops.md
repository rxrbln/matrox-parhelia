Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# Parhelia WARP — the control class: scalar unit, branches, and the address registers

`docs/parhelia-warp-isa-complete.md` decoded the WARP *arithmetic* bundle from
the Windows D3D vs_1_x compiler.  That compiler never emits control flow, so
every opcode below `0x60` stayed as `ctlNN` with raw operand words — and those
are exactly the opcodes the vendor's own setup programs use to install the base
registers `b0..b3`, the address register `a0`, and the constant file.

This document closes that gap.  It gives the control-class instruction format,
the opcode table, the two register files the control class owns, the fully
decoded MTXPARM self-test image, the fully decoded live setup programs, and
**the exact instruction words to emit to set `b0..b3` and `a0` ourselves**.

Evidence tags: **[BIN]** read out of a vendor binary at the quoted offset;
**[HW]** read out of the live RAM/ring capture of a working draw;
**[INF]** inferred from [BIN]/[HW] facts; **[OPEN]** not determined.
Nothing here has been executed on hardware.

Binaries read (no proprietary C source):

| binary | what was taken from it |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARM.sys` | the **350-instruction WARP self-test image at file offset `0x627b8`** (VA `0x727b8`; the previous "346" count stops one nop short of the failure exit) |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | the instruction **field-patcher** at `0x79e00` with its kind table at `0x7a0fc` (36 field kinds), and the opcode-info table `0x1dd510` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | 824 canned WARP programs by ELF symbol (`mtx251`, `mtx1079`, `mtx1591`, `mtx1824`, `mtx2648`, `mtx2817`, `mtx4036`, …) |
| `captures/live-ring-0528/warp-programs/` | the eight live program slots of a working flat-shaded triangle [HW] |

Tool: `tools/parhelia-warpdis2.py` now prints the whole control class with real
mnemonics (`getc`, `putc`, `ldreg`, `streg`, `movi`, `andi`, `ori`, `subi`,
`or`, `sub`, `shl`, `shli`, `br`, `getidx`, `end`) and keeps every unexplained
bit in the flag column.

---

## 0. Headline

1. **The control class is a 32-entry, 32-bit scalar integer unit** ("creg" file
   `c0..c31`) bolted onto the same 96-bit instruction word.  It has three 5-bit
   operand fields — `A = w0[31:27]`, `B = w0[24:20]` (destination),
   `C = w1[6:2]` — plus the 32-bit immediate `{w2[21:0], w1[31:22]}`.  All three
   fields are separate **patch kinds 32/33/34** in the vendor's own field
   patcher, and the immediate is kind 35. [BIN `MTXPARD.dll 0x7a0a9/0x7a0b9/0x7a0c9/0x7a0d8`]
2. **The base registers `b0..b3` and the address register `a0` are not control
   state at all — they are the four components of two entries of the
   *address-latch file*, written by the ordinary secondary op 9 (`mova`).**
   `mova T0, v0` installs `b0..b3 = v0.x,v0.y,v0.z,v0.w`; `mova T1, v1`
   installs `a0.x..a0.w = v1.x..v1.w`.  Relative addressing picks the *file*
   with `w1[13]` (`0` = the `T0` quad → `b0..b3`, `1` = the `T1` quad → `a0`)
   and the *component* with `w1[26:25]`.  Everything the live capture shows
   (`b0=0xe0`, `b1=0x160`, `b2=0x80`, `b3=0x800`, `a0=(0,1,2,3)`) follows
   directly from the kind-1 packet's `v0`/`v1`. [HW + BIN, §5]
3. There are **four such latch pairs** — `(T0,T1)`, `(T2,T3)`, `(T4,T5)`,
   `(T6,T7)` — one per program context; the vendor's `mtx251` writes all four
   with the same vector, `mtx367` writes four *different* vectors. [BIN]
4. A second, packet-free route exists: **`movi c<n>, imm` + `putc T0.<comp>, c<n>`**
   (opcodes `0x41` and `0x0a`) writes one base-register component straight from
   a literal.  `putc T0.x, c2` is byte-identical to `mtx1591` instruction 3. [BIN]
5. `ctl34`/`ctl35` are **load/store between the creg file and a byte-addressed
   hardware register window** — WARP microcode reads and writes the 3D register
   file itself (`mtx1360`: `reg[0] = (reg[0] & 0x03ffffff) | 0x28000000`).
6. The MTXPARM self-test is a **rotate/mix signature program**: it validates its
   input words, extracts nibbles with `shli`/`andi`, uses them as rotate counts
   on six fixed patterns (rotate = `shl` + `shl` + `or`), mixes with `salu5`,
   and exports eight result words into `r0.xyzw`/`r1.xyzw` with `putc`.  Failure
   branches to a tail that zeroes `r0`/`r1`.  This one program pins down
   `or`, `shl`, `shli`, `andi`, `subi`, `movi`, `getc`, `putc` and `br`.
7. `br` (0x23) is a real branch, `target = i + 1 + imm`, with a **condition code
   in `w0[17:14]`** (0 = unconditional).  The former "`brn` 0x46" is **not** a
   branch: it is `shli` — shift by a signed immediate.  The rotate idiom
   `shli c14, c8, -18 ; shli c8, c8, 14 ; or c8, c8, c14` proves it. [BIN]
8. `sop8` (secondary op 8, "no source" in the vendor opcode table) is
   **`getidx dst, {b|a0} + k`** — it copies an address-latch quad into a normal
   register.  `mtx4036` reads both quads back to back:
   `getidx r12, b+0` / `getidx r13, a0+0`. [BIN]

---

## 1. The control-class instruction format

A control instruction is any bundle whose primary opcode `w2[30:23]` is below
`0x60`.  The D3D compiler emits these from the NOP template with only the
opcode set (`MTXPARD 0x192db2`), which is why `parhelia-warp-isa-complete.md`
concluded "they have no operand fields".  The vendor's hand-written setup and
diagnostic programs use the full field set, and the field patcher enumerates it.

### 1.1 Proof: the vendor's field-patcher kind table [BIN]

`MTXPARD.dll` `0x79e00` is a 36-way switch on a field "kind"; the jump table at
`0x7a0fc` gives the complete field map.  Kinds 0–27 are the arithmetic fields
already documented; kinds 28–35 are the ones the control class needs:

| kind | code | field | mask/shift written |
|---|---|---|---|
| 28 | `0x7a051` | `w0[13:10]` | `and 0x3c00` after `shl 0xa` |
| 29 | `0x7a077` | `w0[17:14]` | `and 0x3c000` after `shl 0xe` |
| 30 | `0x7a088` | `w0[18]` (own 1-bit kind) | `and 0x40000` after `shl 0x12` |
| 31 | `0x7a098` | `w0[19]` (own 1-bit kind) | `and 0x80000` after `shl 0x13` |
| 32 | `0x7a0a9` | **`w0[24:20]` — operand B (5 bits)** | `and 0x1f00000` after `shl 0x14` |
| 33 | `0x7a0b9` | **`w0[31:27]` — operand A (5 bits)** | `ebx &= 0x07ffffff; ebx \| v << 0x1b` |
| 34 | `0x7a0c9` | **`w1[6:2]` — operand C (5 bits)** | `and 0x7c` after `shl 2` |
| 35 | `0x7a0d8` | **imm32** | `w2 = (w2 & 0xffc00000) \| (v >> 10)`; `w1 = (w1 & 0x3fffff) \| (v << 22)` |

Kind 32 straddles the arithmetic `src2 idx`/`neg2` boundary and kind 33
straddles `src1 idx`/`neg1`: in the control class those are single 5-bit
register indices, not index+sign.

### 1.2 Field map

| bits | control-class meaning |
|---|---|
| `w0[3:0]` | port-A write mask — used only by `putc` (0x0a) |
| `w0[7:4]` | port-A destination index — `putc` |
| `w0[9:8]` | port-A destination file — `putc`; template value 2 |
| `w0[13:10]` | template `0` (`1` on `getc`/`putc`) |
| `w0[17:14]` | **branch condition code** on `br`; 0 elsewhere |
| `w0[18]` | **set-flags**: the result also updates the branch condition |
| `w0[19]` | template 0 |
| `w0[24:20]` | **operand B — the destination creg** |
| `w0[31:27]` | **operand A — second source creg** |
| `w1[1:0]` | template 0 |
| `w1[6:2]` | **operand C — first source creg** (also the vector index on `getc`) |
| `w1[8:7]` | **source register file on `getc`** (0 `v`, 1 `r`, 2 `T`, 3 `c`) |
| `w1[12:9]` | 0 (no secondary op) |
| `w1[19:14]` | template `0x10` (`extsel=2`), **`0x3c` on `putc`** — the same value `mova` carries |
| `w1[21:20]` | template 0, unused by the control class |
| `w1[31:22]` | the low 10 bits of the immediate |
| `{w2[21:0], w1[31:22]}` | **imm32** |
| `w2[22]` | opcode bit — `1` on `0x0d/0x23/0x25/0x27/0x29`, `0` on the rest |
| `w2[30:23]` | opcode |
| `w2[31]` | `f2b` = 0 throughout the control/setup class |

The template a control instruction is built from is the NOP,
`w0=0x00000200 w1=0x00040000 w2=0x00000000`; `getc`/`putc` set `w0` bit 10 as
well.  The operand order that makes every observed program consistent is

```
    cB = cC  <op>  cA          ; register form   (0x02 0x04 0x05 0x06 0x0b)
    cB = cC  <op>  imm32       ; immediate form  (0x41 0x42 0x43 0x44 0x46)
```

### 1.3 The two register files the control class owns

* **`c0..c31`** — the scalar creg file.  `c0` is the usual scratch for
  compare-only results; `c29`/`c30`/`c31` appear as operands of the
  synchronisation opcodes and may be special.  The file is *not* the
  vec4 constant file `c#` of the arithmetic class; they only share a letter.
  In this document creg operands are written `c<n>` and vec4 constants
  `c[b1+k]` as before.
* **a byte-addressed hardware register window**, reached only by `ldreg`
  (0x34) and `streg` (0x35).  Offsets observed: `0x000 0x004 0x008 0x00c
  0x010 0x014 0x018 0x060 0x064 0x068 0x06c 0x0c0 0x0dc 0x0e0 0x0e4 0x0e8
  0x0ec 0x0f0 0x0f4 0x0f8 0x0fc` — 64 dwords.  Byte offset `a` almost
  certainly maps to MMIO `0x1000 + a`: `mtx1591` reads `[0x0f0]`/`[0x0f4]`
  and the live ring writes `0x10f0` twice; the stage-6 program writes
  `[0x0c0]` and the ring writes `0x10c0`; `mtx1591` read-modify-writes
  `[0x008]/[0x00c]/[0x010]` which would be `0x1008/0x100c/0x1010`.  A base of
  `0x1034` also fits `mtx251`'s last store (`[0x0fc] = 0x0200300c`, the value
  the ring writes to `0x1130`).  The base is **[OPEN]**; the offsets are [BIN].

---

## 2. Control opcode table

`A`/`B`/`C` are the 5-bit creg fields of §1.2.  "sets flags" happens whenever
`w0[18] = 1`.

| op | mnemonic | operation | evidence |
|---|---|---|---|
| `0x00` | `nop` | — | [BIN] template |
| `0x02` | `sub` | `cB = cC − cA` | [INF] `sub c5, c5, c0` then `shl c5, c3, c5` at self-test 169–171 and 211–213 turns a 0..7 field into a right-shift count |
| `0x04` | `or` | `cB = cC \| cA` | [BIN] `mtx1591` 18–20 rebuilds `reg = 0x800 \| (old & 0xffff0000)`; [BIN] self-test rotate idiom |
| `0x05` | `salu5` | `cB = cC ⊕ cA` — a commutative 2-input mix (xor or add) | [BIN] used only as the accumulator of the self-test signature; exact function **[OPEN]** |
| `0x06` | `shl` | `cB = cC << cA`, **signed** count (negative = right) | [BIN] self-test 32–34: `shl c6,c7,c6` with `c6 = c5−32` plus `shl c9,c7,c5` plus `or` = rotate |
| `0x0a` | `putc` | port-A destination register component `= cA` | [BIN] `mtx1591` 3–5/12–14 copies `c2..c7` into `T0.xzw`/`T1.xzw`; self-test 337–344 exports `c2..c9` into `r0.xyzw`/`r1.xyzw` |
| `0x0b` | `salub` | `cB = cC <op> cA`, second class-0 scalar op | [OPEN]; stage-6 program 110/117/124 uses it with a flag word in `cA` |
| `0x0d` | `sync0d` | `cB/cC` + 32-bit mask; the "arm/wait" primitive | [OPEN] |
| `0x1e` | `ctl1e` | — | [OPEN], 3 sightings, all in blobs that are probably not WARP code |
| `0x23` | `br` | `if cond(w0[17:14]) goto i + 1 + imm` | [BIN] `mtx1079` loop back-edge `br → 100` and exit `br cc6 → 107`; self-test `br cc10 → 347` = the failure tail |
| `0x25` | `sync25` | operand `C` only (29/31), no imm | [OPEN]; last instruction of the clipper tails |
| `0x27` | `sync27` | operand `C = 30` + mask; always the last of a sync block | [OPEN] |
| `0x29` | `end` | terminate the program | [BIN] |
| `0x2b` | `sync2b` | no operands | [OPEN] |
| `0x33` | `getc` | `cB = <vector reg>.x`, register named by `(w1[8:7], C)` | [BIN] self-test 3–7 distributes `v0.xyzw`,`v1.x` into `c2..c6`; [HW] kind-1/kind-3 |
| `0x34` | `ldreg` | `cB = hwreg[imm]` | [BIN] `mtx2648` copies `[0x008..0x014]` → `[0x060..0x06c]` |
| `0x35` | `streg` | `hwreg[imm] = cC` | [BIN] same; `mtx1360` RMW of `hwreg[0]` |
| `0x41` | `movi` | `cB = imm32` | [BIN] `mtx1591` 9–11, `mtx2817` 1/3/5/6 |
| `0x42` | `subi` | `cB = cC − imm` | [BIN] `mtx1079` loop counter `subi c2,c2,1` + setflags; self-test `subi c6,c5,0x20` |
| `0x43` | `andi` | `cB = cC & imm` | [BIN] `mtx1360` `andi c3,c3,0x03ffffff`; self-test field masks |
| `0x44` | `ori` | `cB = cC \| imm` | [BIN] `mtx1360` `ori c3,c3,0x28000000` |
| `0x46` | `shli` | `cB = cC << imm`, **signed** | [BIN] self-test 326–328 `shli c14,c8,-18; shli c8,c8,14; or c8,c8,c14` = rotate-left-14 |

Opcodes `0x01 0x03 0x07..0x09 0x0c 0x0e..0x1d 0x1f..0x22 0x24 0x26 0x28
0x2a 0x2c..0x32 0x36..0x40 0x45 0x47..0x5f` do not occur in any corpus. [OPEN]

Branch condition codes seen: `0` (unconditional, `mtx1079` back-edge and
`mtx2817` entry-point skip), `1` (`mtx853`), `6` (`mtx1079` loop exit after
`subi … setflags`), `10` (self-test early-out after `andi … setflags`).  The
encoding of the condition field is **[OPEN]**; `0 = always` is [BIN].

---

## 3. The address-latch file — where `b0..b3` and `a0` actually live

### 3.1 The relative-addressing encoding, re-read

`parhelia-warp-isa-complete.md` §2 established:

* `w1[29:27]` (`f1i`) = which operand is indexed (4/5/6 = src0/src1/src2, 7 = port-A dst)
* `w1[13]` (`idxsel`) = "index source: 1 = the program's `a0`, 0 = a driver-preset base register"
* `w1[26:25]` (`relcomp`) = "`a0.{x,y,z,w}` when `idxsel=1`, base register 0..3 when `idxsel=0`"

Read as one field, `{idxsel, relcomp}` is simply **"which entry of the
address-latch file, which component of it"**: `idxsel` picks the entry (even =
the base quad, odd = the `a0` quad) and `relcomp` picks the component.  There
is no separate base-register file: **`b0..b3` are the four components of one
latch and `a0.x..a0.w` the four components of its partner.** [INF, from the
encoding; every [HW] fact below follows]

### 3.2 What writes them: `mova` (secondary op 9) [BIN + HW]

Destination file 0 is the latch/address file.  A plain `mov` (secondary op 1)
to `T#` writes the latch's *data* — the live per-vertex program parks the
position in `T0` and the viewport offset in `T1` between two `dp4`s that read
`c[a0.x+k]`, and `a0` survives.  Only **`mova`** updates the address copy.
[HW, and stated as an inference in `parhelia-warp-programs-decoded.md` §3.1 —
this document confirms it from the encoder side]

The vendor's kind-1 program (`mtx251`, live at slot `16c05c00` index 96) is:

```
mova T0, v0      0008000f 004f1200 301a21a2
mova T2, v0      0008002f 004f1200 301a21a2
mova T4, v0      0008004f 004f1200 301a21a2
mova T6, v0      0008006f 004f1200 301a21a2
mova T1, v1      0018001f 004f1200 301a21a2   (mtx251 uses 004fd200 here; mtx2465 uses 004f1200)
mova T3, v1      0018003f 004f1200 301a21a2
mova T5, v1      0018005f 004f1200 301a21a2
mova T7, v1      0018007f 004f1200 301a21a2
```

and the packet that follows the bind supplies

```
41 06 00 0f | 000000e0 00000160 00000080 00000800   <- v0
            | 00000000 00000001 00000002 00000003   <- v1
            | 02003004 0200300c 00000000 00000000   <- v2
            | 07955583 ffffffff ffffffff ffffffff   <- v3
```

so `b0..b3 = (0xe0, 0x160, 0x80, 0x800)` and `a0 = (0, 1, 2, 3)`. [HW]

Every dependent fact checks out:

| program | operand | `idxsel`/`relcomp` | resolves to | block entry |
|---|---|---|---|---|
| live vertex program | `c[a0.x+0..3]` | 1 / 0 → `T1.x = 0` | `c[0..3]` | the MVP rows [HW] |
| live vertex program | `c[b1+1]` | 0 / 1 → `T0.y = 0x160` | `c[0x161]` | viewport offset [HW] |
| live vertex tail | `c[b1+8]` | 0 / 1 | `c[0x168]` | grid snap [HW] |
| `mtx999` | `c[b0+105]` | 0 / 0 → `T0.x = 0xe0` | `c[0x149]` | projection matrix [BIN] |
| `AddAfs` | `c[b0+88]` | 0 / 0 | `c[0x138]` | ambient product [BIN] |
| D3D `c128..c255` | base 2 | 0 / 2 → `T0.z = 0x80` | `c[128+n]` | [BIN `MTXPARD 0x191370`] |
| `mtx2655` | `v[a0.y+k]` | 1 / 1 → `T1.y` | vertex-stream window | [BIN] |
| clipper class | `o[b3+k]` | 0 / 3 → `T0.w = 0x800` | output block | [BIN] |

### 3.3 Four latch pairs, one per context

`mtx251` writes the *same* `v0` to `T0/T2/T4/T6` and the same `v1` to
`T1/T3/T5/T7`.  `mtx367` — the same program shape — writes **four different
input vectors** to `T0/T2/T4/T6`.  So there are four independent
(base quad, `a0` quad) pairs, presumably one per WARP program context/stage,
and the relative-addressing encoding only reaches the pair belonging to the
running program. [BIN + INF]

`w1[19:14]` on `mova` is `0x3c` (`extsel=7`, `exthi=4`) in every vendor
program except `mtx251`'s second group, which uses `0x3f` (`exthi=7`).
`extsel=7` is not a legal "extended operand" selector, so these bits mean
something else here; `exthi` is patch kind 17, i.e. the vendor's linker
*patches* it, so the two values are probably a real selector (candidate: which
latch pair, or a bank bit).  Emit `0x3c` — it is what `mtx2465`, `mtx1079`,
`mtx2068` and `mtx2655` all use for both groups. **[OPEN]**

### 3.4 Reading them back: `getidx` (secondary op 8) [BIN]

The vendor opcode table records secondary op 8 as "hasDst = 1, nsrc = 0,
unit 6" — a destination with no vector source [BIN `MTXPARD 0x1dd4a0`].  Its
value comes from the address-latch file, selected by the same `w1[13]`:

```
mtx4036:  getidx r12, b+0      ; the base quad  (idxsel = 0)
          getidx r13, a0+0     ; the a0 quad    (idxsel = 1)
mtx1079:  getidx r3,  a0+0     ; save a0 before the loop
          getidx T1.xy, a0+1   ; a0.xy += 1     (the loop step)
          mova   T1, r3        ; restore a0
```

The bias `+k` is the index field of the operand slot that is *not* marked
relative by `f1i` (`w1[5:2]` when `f1i = 6`, `w0[23:20]` when `f1i = 4`).
`getidx T1.xy, a0+1` is the constant-file loader's address increment — the one
instruction that makes `mtx1079`'s loop terminate. [BIN + INF]

---

## 4. The MTXPARM self-test, decoded

`MTXPARM.sys` file offset `0x627b8` (VA `0x727b8`), **350 instructions**
(`350 × 12 = 0x1068` bytes; the last four are the failure tail that the early
`br`s jump to, which is why a 346-instruction reading looked like it branched
past the end).

Shape:

```
  0..  7   marshal the two input vectors into c2..c6
  8.. 28   six validate-or-fail tests: andi …, setflags ; br cc10 -> 347
 29..298   32 identical blocks:
                shli c5, <input>, -n     ; extract a bit field
                andi c5, c5, 0xf|0x1f
                subi c6, c5, 0x20
                movi c7, <one of six fixed patterns>
                shl  c6, c7, c6          ; c7 >> (32-c5)
                shl  cK, c7, c5          ; c7 << c5
                or   cK, cK, c6          ; = rotate-left(c7, c5)
                salu5 cAcc, cAcc, cK     ; mix into the running signature
299..336   final mixing of c9..c17 into c2..c9, with two more explicit
           rotates built from shli/shli/or
337..344   putc r0.x..r1.w, c2..c9      ; export the 8-word signature
345        end                          ; success
346        nop
347..349   mov r0, r0.0000 ; mov r1, r1.0000 ; end     ; failure: zero the result
```

The six patterns are `0xa7468c4e 0x822fe4ed 0x39cb29f3 0x85ac4b69 0x4721b6a8
0x38c215f4`.  The rotate counts come from nibble/5-bit fields of the caller's
`v0.x`, `v0.y` and `v1.x`, so the host picks the exact signature it expects.

This program is what pins the arithmetic down: the two explicit rotates at
326–328 and 334–336 can only be `x = (x << k) | (x >> (32−k))`, which fixes
`0x46` as a signed-immediate shift, `0x06` as a signed-register shift and
`0x04` as OR, and the export block fixes `0x0a` as creg → register.

Full listing (`python3 tools/parhelia-warpdis2.py selftest.bin --no-raw`):

```
   0: mov T0, v0.yyyy   ; f2b=0
   1: mov T1, v0.zzzz   ; f2b=0
   2: mov T2, v0.wwww   ; f2b=0
   3: getc c2, v1.x
   4: getc c3, v0.x
   5: getc c4, T0.x
   6: getc c5, T1.x
   7: getc c6, T2.x
   8: andi c0, c2, 0xffffffff   ; setflags
   9: br cc10, -> 347
  10: nop
  11: or c7, c3, c4
  12: or c7, c7, c5
  13: or c7, c7, c6
  14: andi c0, c7, 0xffffffff   ; setflags
  15: br cc10, -> 347
  16: nop
  17: andi c0, c3, 0xff   ; setflags
  18: br cc10, -> 347
  19: nop
  20: andi c0, c3, 0xff00   ; setflags
  21: br cc10, -> 347
  22: nop
  23: andi c0, c3, 0xff0000   ; setflags
  24: br cc10, -> 347
  25: nop
  26: andi c0, c3, 0xff000000   ; setflags
  27: br cc10, -> 347
  28: nop
  29: andi c5, c2, 0x1f
  30: subi c6, c5, 0x20
  31: movi c7, 0xa7468c4e
  32: shl c6, c7, c6
  33: shl c9, c7, c5
  34: or c9, c9, c6
  35: shli c5, c4, -24
  36: andi c5, c5, 0xf
  37: subi c6, c5, 0x20
  38: movi c7, 0x822fe4ed
  39: shl c6, c7, c6
  40: shl c10, c7, c5
  41: or c10, c10, c6
  42: salu5 c9, c9, c10   ; ?salu5
  43: shli c5, c4, -28
  44: andi c5, c5, 0xf
  45: subi c6, c5, 0x20
  46: movi c7, 0x39cb29f3
  47: shl c6, c7, c6
  48: shl c10, c7, c5
  49: or c10, c10, c6
  50: salu5 c9, c9, c10   ; ?salu5
  51: salu5 c9, c9, c4   ; ?salu5
  52: shli c5, c3, -20
  53: andi c5, c5, 0xf
  54: subi c6, c5, 0x20
  55: movi c7, 0x85ac4b69
  56: shl c6, c7, c6
  57: shl c10, c7, c5
  58: or c10, c10, c6
  59: salu5 c9, c9, c10   ; ?salu5
  60: shli c5, c4, -20
  61: andi c5, c5, 0xf
  62: subi c6, c5, 0x20
  63: movi c7, 0x85ac4b69
  64: shl c6, c7, c6
  65: shl c10, c7, c5
  66: or c10, c10, c6
  67: salu5 c9, c9, c10   ; ?salu5
  68: shli c5, c4, -12
  69: andi c5, c5, 0xf
  70: subi c6, c5, 0x20
  71: movi c7, 0x822fe4ed
  72: shl c6, c7, c6
  73: shl c10, c7, c5
  74: or c10, c10, c6
  75: salu5 c9, c9, c10   ; ?salu5
  76: shli c5, c3, -8
  77: andi c5, c5, 0xf
  78: subi c6, c5, 0x20
  79: movi c7, 0x4721b6a8
  80: shl c6, c7, c6
  81: shl c10, c7, c5
  82: or c10, c10, c6
  83: salu5 c9, c9, c10   ; ?salu5
  84: shli c5, c4, -8
  85: andi c5, c5, 0xf
  86: subi c6, c5, 0x20
  87: movi c7, 0x38c215f4
  88: shl c6, c7, c6
  89: shl c10, c7, c5
  90: or c10, c10, c6
  91: salu5 c9, c9, c10   ; ?salu5
  92: shli c5, c4, -4
  93: andi c5, c5, 0xf
  94: subi c6, c5, 0x20
  95: movi c7, 0xa7468c4e
  96: shl c6, c7, c6
  97: shl c10, c7, c5
  98: or c10, c10, c6
  99: salu5 c9, c9, c10   ; ?salu5
 100: shli c5, c3, -28
 101: andi c5, c5, 0xf
 102: subi c6, c5, 0x20
 103: movi c7, 0x39cb29f3
 104: shl c6, c7, c6
 105: shl c10, c7, c5
 106: or c10, c10, c6
 107: salu5 c9, c9, c10   ; ?salu5
 108: shli c5, c2, -5
 109: andi c5, c5, 0xf
 110: subi c6, c5, 0x20
 111: movi c7, 0x4721b6a8
 112: shl c6, c7, c6
 113: shl c10, c7, c5
 114: or c10, c10, c6
 115: salu5 c9, c9, c10   ; ?salu5
 116: shli c5, c2, -9
 117: andi c5, c5, 0xf
 118: subi c6, c5, 0x20
 119: movi c7, 0x85ac4b69
 120: shl c6, c7, c6
 121: shl c11, c7, c5
 122: or c11, c11, c6
 123: shli c5, c2, -13
 124: andi c5, c5, 0x1f
 125: subi c6, c5, 0x20
 126: shl c6, c4, c6
 127: shl c12, c4, c5
 128: or c12, c12, c6
 129: salu5 c11, c11, c12   ; ?salu5
 130: shli c5, c4, -16
 131: andi c5, c5, 0xf
 132: subi c6, c5, 0x20
 133: movi c7, 0x85ac4b69
 134: shl c6, c7, c6
 135: shl c13, c7, c5
 136: or c13, c13, c6
 137: salu5 c11, c13, c11   ; ?salu5
 138: shli c5, c3, -24
 139: andi c5, c5, 0xf
 140: subi c6, c5, 0x20
 141: movi c7, 0x39cb29f3
 142: shl c6, c7, c6
 143: shl c13, c7, c5
 144: or c13, c13, c6
 145: salu5 c11, c13, c11   ; ?salu5
 146: shli c5, c3, -16
 147: andi c5, c5, 0xf
 148: subi c6, c5, 0x20
 149: movi c7, 0x85ac4b69
 150: shl c6, c7, c6
 151: shl c13, c7, c5
 152: or c13, c13, c6
 153: salu5 c11, c13, c11   ; ?salu5
 154: shli c5, c3, -12
 155: andi c5, c5, 0xf
 156: subi c6, c5, 0x20
 157: movi c7, 0x4721b6a8
 158: shl c6, c7, c6
 159: shl c13, c7, c5
 160: or c13, c13, c6
 161: salu5 c11, c11, c13   ; ?salu5
 162: shli c5, c2, -18
 163: andi c5, c5, 0x1f
 164: subi c6, c5, 0x20
 165: movi c7, 0x39cb29f3
 166: shl c6, c7, c6
 167: shl c13, c7, c5
 168: or c13, c13, c6
 169: shli c5, c2, -23
 170: sub c5, c0, c5
 171: shl c5, c3, c5
 172: andi c5, c5, 0x1f
 173: subi c6, c5, 0x20
 174: movi c7, 0x38c215f4
 175: shl c6, c7, c6
 176: shl c14, c7, c5
 177: or c14, c14, c6
 178: salu5 c13, c13, c14   ; ?salu5
 179: shli c5, c2, -28
 180: andi c5, c5, 0xf
 181: subi c6, c5, 0x20
 182: movi c7, 0x85ac4b69
 183: shl c6, c7, c6
 184: shl c15, c7, c5
 185: or c15, c15, c6
 186: salu5 c15, c15, c13   ; ?salu5
 187: salu5 c14, c14, c15   ; ?salu5
 188: salu5 c14, c14, c3   ; ?salu5
 189: salu5 c14, c14, c9   ; ?salu5
 190: shli c5, c3, -5
 191: andi c5, c5, 0xf
 192: subi c6, c5, 0x20
 193: movi c7, 0x85ac4b69
 194: shl c6, c7, c6
 195: shl c10, c7, c5
 196: or c10, c10, c6
 197: salu5 c9, c9, c3   ; ?salu5
 198: salu5 c10, c10, c4   ; ?salu5
 199: salu5 c10, c10, c9   ; ?salu5
 200: movi c5, 0xa7468c4e
 201: salu5 c11, c11, c5   ; ?salu5
 202: salu5 c11, c11, c10   ; ?salu5
 203: shli c5, c4, -8
 204: andi c5, c5, 0xf
 205: subi c6, c5, 0x20
 206: movi c7, 0x38c215f4
 207: shl c6, c7, c6
 208: shl c13, c7, c5
 209: or c13, c13, c6
 210: salu5 c11, c11, c13   ; ?salu5
 211: andi c5, c2, 0x7
 212: sub c5, c0, c5
 213: shl c5, c4, c5
 214: andi c5, c5, 0x1f
 215: subi c6, c5, 0x20
 216: shl c6, c11, c6
 217: shl c13, c11, c5
 218: or c13, c13, c6
 219: salu5 c12, c12, c13   ; ?salu5
 220: andi c5, c4, 0xf
 221: subi c6, c5, 0x20
 222: movi c7, 0x39cb29f3
 223: shl c6, c7, c6
 224: shl c13, c7, c5
 225: or c13, c13, c6
 226: salu5 c12, c12, c13   ; ?salu5
 227: salu5 c12, c12, c4   ; ?salu5
 228: movi c5, 0x85ac4b69
 229: salu5 c13, c10, c5   ; ?salu5
 230: salu5 c14, c14, c13   ; ?salu5
 231: salu5 c14, c14, c3   ; ?salu5
 232: shli c5, c3, -9
 233: andi c5, c5, 0x1f
 234: subi c6, c5, 0x20
 235: movi c7, 0x38c215f4
 236: shl c6, c7, c6
 237: shl c15, c7, c5
 238: or c15, c15, c6
 239: salu5 c15, c15, c13   ; ?salu5
 240: salu5 c15, c15, c4   ; ?salu5
 241: salu5 c15, c15, c9   ; ?salu5
 242: shli c5, c3, -17
 243: andi c5, c5, 0xf
 244: subi c6, c5, 0x20
 245: movi c7, 0x85ac4b69
 246: shl c6, c7, c6
 247: shl c16, c7, c5
 248: or c16, c16, c6
 249: salu5 c13, c13, c16   ; ?salu5
 250: shli c5, c3, -28
 251: andi c5, c5, 0xf
 252: subi c6, c5, 0x20
 253: shl c6, c3, c6
 254: shl c16, c3, c5
 255: or c16, c16, c6
 256: andi c5, c4, 0x1f
 257: subi c6, c5, 0x20
 258: movi c7, 0x822fe4ed
 259: shl c6, c7, c6
 260: shl c17, c7, c5
 261: or c17, c17, c6
 262: salu5 c16, c16, c17   ; ?salu5
 263: salu5 c16, c16, c3   ; ?salu5
 264: salu5 c10, c10, c16   ; ?salu5
 265: shli c5, c3, -24
 266: andi c5, c5, 0x1f
 267: subi c6, c5, 0x20
 268: movi c7, 0x4721b6a8
 269: shl c6, c7, c6
 270: shl c17, c7, c5
 271: or c17, c17, c6
 272: salu5 c16, c16, c17   ; ?salu5
 273: salu5 c16, c16, c9   ; ?salu5
 274: shli c5, c4, -5
 275: andi c5, c5, 0x1f
 276: subi c6, c5, 0x20
 277: movi c7, 0x38c215f4
 278: shl c6, c7, c6
 279: shl c17, c7, c5
 280: or c17, c17, c6
 281: salu5 c15, c15, c17   ; ?salu5
 282: salu5 c15, c15, c4   ; ?salu5
 283: shli c5, c4, -10
 284: andi c5, c5, 0xf
 285: subi c6, c5, 0x20
 286: movi c7, 0x38c215f4
 287: shl c6, c7, c6
 288: shl c17, c7, c5
 289: or c17, c17, c6
 290: salu5 c13, c13, c17   ; ?salu5
 291: salu5 c13, c13, c4   ; ?salu5
 292: shli c5, c4, -14
 293: andi c5, c5, 0x1f
 294: subi c6, c5, 0x20
 295: movi c7, 0x39cb29f3
 296: shl c6, c7, c6
 297: shl c17, c7, c5
 298: or c17, c17, c6
 299: salu5 c2, c14, c12   ; ?salu5
 300: salu5 c2, c2, c16   ; ?salu5
 301: salu5 c3, c15, c12   ; ?salu5
 302: movi c4, 0x39cb29f3
 303: salu5 c3, c3, c4   ; ?salu5
 304: salu5 c3, c3, c9   ; ?salu5
 305: salu5 c4, c9, c11   ; ?salu5
 306: salu5 c4, c4, c13   ; ?salu5
 307: salu5 c5, c10, c12   ; ?salu5
 308: salu5 c5, c5, c17   ; ?salu5
 309: movi c6, 0xa7468c4e
 310: salu5 c5, c5, c6   ; ?salu5
 311: salu5 c6, c15, c12   ; ?salu5
 312: salu5 c6, c6, c13   ; ?salu5
 313: movi c7, 0x822fe4ed
 314: salu5 c6, c6, c7   ; ?salu5
 315: movi c8, 0x85ac4b69
 316: salu5 c7, c17, c8   ; ?salu5
 317: salu5 c7, c7, c9   ; ?salu5
 318: salu5 c7, c7, c16   ; ?salu5
 319: salu5 c7, c7, c12   ; ?salu5
 320: movi c8, 0x38c215f4
 321: salu5 c7, c7, c8   ; ?salu5
 322: movi c14, 0x4721b6a8
 323: salu5 c8, c10, c12   ; ?salu5
 324: salu5 c8, c8, c15   ; ?salu5
 325: salu5 c8, c8, c14   ; ?salu5
 326: shli c14, c8, -18
 327: shli c8, c8, 14
 328: or c8, c8, c14
 329: salu5 c8, c8, c9   ; ?salu5
 330: movi c10, 0xa7468c4e
 331: salu5 c9, c13, c12   ; ?salu5
 332: salu5 c9, c9, c10   ; ?salu5
 333: salu5 c9, c9, c11   ; ?salu5
 334: shli c10, c9, -24
 335: shli c9, c9, 8
 336: or c9, c9, c10
 337: putc r0.x, c2
 338: putc r0.y, c3
 339: putc r0.z, c4
 340: putc r0.w, c5
 341: putc r1.x, c6
 342: putc r1.y, c7
 343: putc r1.z, c8
 344: putc r1.w, c9
 345: end
 346: nop
 347: mov r0, r0.0000   ; f2b=0
 348: mov r1, r1.0000   ; f2b=0
 349: end
```

---

## 5. The live setup programs, decoded

The three standalone programs the vendor binds before the draw
(`captures/live-ring-0528/warp-programs/`, entry index 96 in each slot) and the
stale kind-12 copy at index 0 of slot `16c05000`:

```
--- slot-16c05000 (entry 96)
  96: getc c29, v0.x
  97: mov T0, v0.yyyy   ; f2b=0
  98: mov T1, v0.zzzz   ; f2b=0
  99: mov T2, v0.wwww   ; f2b=0
 100: mov T3, v1.yyyy   ; f2b=0
 101: mov T4, v1.zzzz   ; f2b=0
 102: mov T5, v1.wwww   ; f2b=0
 103: sync0d c10, c10, c29, 0xffffffff   ; ?sync0d
 104: nop
 105: getc c17, T2.x
 106: getc c18, v1.x
 107: getc c19, T3.x
 108: getc c20, T4.x
 109: getc c21, T5.x
 110: op6e T0., v0.yz00, v0, v0   ; f2b=0 noBwrite op6e_unknown
 111: end
--- slot-16c05600 (entry 96)
  96: getidx r3, a0+0   ; f2b=0 idx=a0.x
  97: getc c2, v0.x
  98: mova T1.x, v0.yyyy   ; f2b=0 extsel=7,exthi=4
  99: mova T1.y, v0.zzzz   ; f2b=0 extsel=7,exthi=4
 100: subi c2, c2, 0x1   ; setflags
 101: br cc6, -> 107
 102: nop
 103: mov c0, v0   ; w1[21:20]=1 f2b=0 extsel=6,exthi=4 f1g=6 idxsel=1,relcomp=0
 104: getidx T1.xy, a0+1   ; f2b=0 idx=a0.x
 105: br -> 100
 106: nop
 107: mova T1, r3   ; f2b=0 extsel=7,exthi=4
 108: end
--- slot-16c05c00 (entry 96)
  96: mov T5, v3.xxxx   ; f2b=0
  97: mov T4, v3.yyyy   ; f2b=0
  98: mov T3, v3.zzzz   ; f2b=0
  99: mov T2, v3.wwww   ; f2b=0
 100: mov T1, v2.xxxx   ; f2b=0
 101: mov T0, v2.yyyy   ; f2b=0
 102: mova T0, v0   ; f2b=0 extsel=7,exthi=4
 103: mova T2, v0   ; f2b=0 extsel=7,exthi=4
 104: mova T4, v0   ; f2b=0 extsel=7,exthi=4
 105: mova T6, v0   ; f2b=0 extsel=7,exthi=4
 106: mova T1, v1   ; f2b=0 extsel=7,exthi=7
 107: mova T3, v1   ; f2b=0 extsel=7,exthi=7
 108: mova T5, v1   ; f2b=0 extsel=7,exthi=7
 109: mova T7, v1   ; f2b=0 extsel=7,exthi=7
 110: getc c24, T5.x
 111: getc c25, T4.x
 112: getc c26, T3.x
 113: getc c27, T2.x
 114: getc c28, T1.x
 115: getc c29, T0.x
 116: streg [0x0e8], c24
 117: streg [0x0ec], c25
 118: streg [0x0f0], c26
 119: streg [0x0f4], c27
 120: streg [0x0f8], c28
 121: streg [0x0fc], c29
 122: salu5 c14, c14, c14   ; ?salu5
 123: end
--- slot-16c05000 index 0 (stale kind-12 copy, mtx2068)
   0: streg [0x004], c14
   1: streg [0x008], c6
   2: add T0, c[b1+2], v0.0000 | mova T1, r13   ; w1[21:20]=3 f2b=0 extsel=7,exthi=4 f1g=2 idx=base1
   3: add T1, c[b1+3], v0.0000 | mova T0, r12   ; w1[21:20]=3 f2b=0 extsel=7,exthi=4 f1g=2 idx=base1
   4: sync0d c10, c10, c1, 0x3800000   ; ?sync0d
   5: sync0d c11, c11, c0, 0x30   ; ?sync0d
   6: sync2b c0, c0, c0, 0x0   ; ?sync2b
   7: sync0d c9, c9, c0, 0x10   ; ?sync0d
   8: sync27 c0, c30, c0, 0x10   ; ?sync27
   9: nop
```

### 5.1 Kind 3 (`mtx1824`, slot `16c05000` @96) — publish the resident entries

`getc` loads `v0.x` and the pre-swizzled `v0.yzw`/`v1.yzw` into `c29` and
`c17..c21`.  With the packet
`41020007 | 03800015 00000000 3f800000 07956183 | 3f803f80 07955583 ffffffff 07956b83`:

| creg | value | meaning [HW] |
|---|---|---|
| `c29` | `0x03800015` | a WARP mode/control word |
| `c17` | `0x07956183` | kind-12 program entry |
| `c18` | `0x3f803f80` | — |
| `c19` | `0x07955583` | kind-6 (clipper) program entry |
| `c20` | `0xffffffff` | unused entry |
| `c21` | `0x07956b83` | stage-6 program entry |

The `mov T0..T5, v0/v1.yyyy/zzzz/wwww` in front exist because a control
instruction's `w2[21:0]` is the top of the immediate, so `getc` has **no
swizzle** and can only read component `.x`; the components have to be
pre-broadcast into latches first.  That explains the shape of all three
programs. [INF, from the encoding]

The program never `streg`s these — `sync0d c10, c10, c29, 0xffffffff` and the
destination-less `op6e` before `end` are what commits them. [OPEN]

### 5.2 Kind 0 (`mtx1079`, slot `16c05600` @96) — the constant-file loader

```
getidx r3, a0+0            ; save a0
getc   c2, v0.x            ; c2 = count
mova   T1.x, v0.yyyy       ; a0.x = first block index
mova   T1.y, v0.zzzz       ; a0.y = the source window index
loop:
subi   c2, c2, 1  (setflags)
br cc6 -> exit
nop
mov    c0, v0              ; destination file 3, relative to a0.x -> c[a0.x]
getidx T1.xy, a0+1         ; a0.xy += 1
br     -> loop
nop
exit:
mova   T1, r3              ; restore a0
end
```

This is the whole "block load" mechanism: the `0x41` packets that follow the
bind carry `(count, first, 1, 0)` as `v0` and the payload as `v1..`, and the
**constant/block file is written by microcode through destination file 3**.
[HW + BIN]

### 5.3 Kind 1 (`mtx251`, slot `16c05c00` @96) — base registers, `a0`, entries

```
mov  T5..T0, v3.xyzw / v2.xy        ; pre-broadcast for getc
mova T0,T2,T4,T6 <- v0             ; b0..b3 for the four contexts
mova T1,T3,T5,T7 <- v1             ; a0     for the four contexts
getc c24..c29 <- T5,T4,T3,T2,T1,T0
streg [0x0e8] c24 … [0x0fc] c29
salu5 c14, c14, c14
end
```

With the packet of §3.2: `b0..b3 = (0xe0,0x160,0x80,0x800)`,
`a0 = (0,1,2,3)`, and the six stored words are
`(kind-6 entry, −1, −1, −1, 0x02003004, 0x0200300c)`.  `0x0200300c` is exactly
the value the ring writes to MMIO `0x1130`. [HW]

### 5.4 The stage programs

`mtx2817` (slot `16c06800` @111/@115) is a two-entry-point prologue:
entry 111 sets `c14 = 0x300` and jumps over `movi c10, 0`; entry 115 sets
`c14 = 0x400, c10 = 4`; both fall into `mtx2648`, which copies hardware
registers `[0x008 0x00c 0x010 0x014]` into `[0x060 0x064 0x068 0x06c]`.

The stage-6 program (slot `16c03e00` @96) is a pure register bit-field
rewriter: it reads `[0x008]/[0x00c]/[0x010]/[0x014]`, and for each of three of
them computes
`new = (old & 0x1000ffff) | ((((old & 0x09000000) >> 6) | old) <op 0x0b> flag) & 0x0fff0000`
where `flag = old14 & 0x10000000`.  Nothing in it touches `a0` or the bases.

---

## 6. What to emit to set `b0..b3` and `a0` ourselves

Two routes.  Both were assembled from the field rules of §1/§3 and decoded back
with `tools/parhelia-warpdis2.py`; where a vendor word exists for the same
operation it is byte-identical.

### 6.1 Route A — packet-driven, the vendor's own form (recommended)

Nine instructions.  Bind it exactly as the vendor binds kind 1
(`[4][403c3cdd][x][20000028][20000028][entry]`, MODE nibble 8 = "run once on
the following packet") and follow it with one `0x41` packet whose sub-header is
`v0` and whose first data vec4 is `v1`.

```
0008000f 004f1200 301a21a2   mova T0, v0     ; b0..b3 = v0.xyzw   (context 0)
0008002f 004f1200 301a21a2   mova T2, v0     ;                    (context 1)
0008004f 004f1200 301a21a2   mova T4, v0     ;                    (context 2)
0008006f 004f1200 301a21a2   mova T6, v0     ;                    (context 3)
0018001f 004f1200 301a21a2   mova T1, v1     ; a0.xyzw = v1.xyzw  (context 0)
0018003f 004f1200 301a21a2   mova T3, v1     ;                    (context 1)
0018005f 004f1200 301a21a2   mova T5, v1     ;                    (context 2)
0018007f 004f1200 301a21a2   mova T7, v1     ;                    (context 3)
00000200 00040000 14c00000   end
```

packet (the vendor's, with our values substituted):

```
41 06 00 0f | b0 b1 b2 b3 | a0x a0y a0z a0w | 00000000 ×4 | 00000000 ×4
```

For the vendor's own configuration that is `b = (0x0e0, 0x160, 0x080, 0x800)`
and `a0 = (0, 1, 2, 3)`.

Word derivation (so the generator can vary the latch index and the input slot):

```
w0 = (2 << 18) | (srcv << 20) | (latch << 4) | 0x0f
                     ; fileB = 2 with maskB = 0 = "port B idle" (template)
                     ; src2 = input register srcv, file 0, no negate
                     ; port A: file 0 (latch), index latch, mask 0xf
w1 = 0x004f1200      ; sop = 9 (mova), extsel/exthi = 0x3c, f1g = 1
w2 = 0x301a21a2      ; primary 0x60 (wra), f2b = 0, identity swizzles
```

Instructions 0–3 and 4–7 are byte-identical to `mtx2465` instructions 2–9
[BIN] and to slot `16c05c00` indices 102–109 [HW].

### 6.2 Route B — self-contained, no input packet

Seventeen instructions, no packet at all: load a literal into a creg with
`movi` and copy it into one component of the latch with `putc`.

```
00200200 38040000 20800000   movi c2, 0x000000e0
10000401 000f0000 05000000   putc T0.x, c2        ; b0
00200200 58040000 20800000   movi c2, 0x00000160
10000402 000f0000 05000000   putc T0.y, c2        ; b1
00200200 20040000 20800000   movi c2, 0x00000080
10000404 000f0000 05000000   putc T0.z, c2        ; b2
00200200 00040000 20800002   movi c2, 0x00000800
10000408 000f0000 05000000   putc T0.w, c2        ; b3
00200200 00040000 20800000   movi c2, 0x00000000
10000411 000f0000 05000000   putc T1.x, c2        ; a0.x
00200200 00440000 20800000   movi c2, 0x00000001
10000412 000f0000 05000000   putc T1.y, c2        ; a0.y
00200200 00840000 20800000   movi c2, 0x00000002
10000414 000f0000 05000000   putc T1.z, c2        ; a0.z
00200200 00c40000 20800000   movi c2, 0x00000003
10000418 000f0000 05000000   putc T1.w, c2        ; a0.w
00000200 00040000 14c00000   end
```

(repeat the `putc` pairs with `T2/T3`, `T4/T5`, `T6/T7` if the other three
contexts are needed).

Word derivation:

```
movi c<n>, imm :  w0 = (n << 20) | 0x200
                  w1 = 0x00040000 | ((imm << 22) & 0xffffffff)
                  w2 = 0x20800000 | (imm >> 10)
putc <file><idx>.<comp>, c<n> :
                  w0 = (n << 27) | (1 << 10) | (file << 8) | (idx << 4) | (1 << comp)
                  w1 = 0x000f0000
                  w2 = 0x05000000
                  ; file: 0 = latch/address T#, 1 = temp r#, 2 = output o#
```

Cross-checks against vendor words [BIN]:

| ours | vendor | note |
|---|---|---|
| `putc T0.x, c2` = `10000401 000f0000 05000000` | `mtx1591` instr 3 | **identical** |
| `movi c2, 0x800` = `00200200 00040000 20800002` | `mtx1591` instr 9 `movi c5, 0x800` = `00500200 00040000 20800002` | identical but for the creg index |
| `movi c2, 0x3` = `00200200 00c40000 20800000` | `mtx1591` instr 10 `movi c6, 0x3` = `00600200 00c40014 20800000` | identical but for `B` and an inert leftover in `C` |

Route B is [INF] — the `putc` direction (creg → register) is proven by
`mtx1591`, but it has never been used on an *address* latch in a vendor
program, so whether a `putc` to file 0 updates the address copy the way `mova`
does is the one untested step.  Route A is the safe one.

### 6.3 Which context does our program use?

Unknown [OPEN].  Route A writes all four pairs with the same values, exactly as
the vendor does, so it does not matter.  If Route B is used, write `T0/T1`
*and* `T2/T3`, `T4/T5`, `T6/T7`.

---

## 7. The remaining scalar / arithmetic-class opcodes named in the gap list

| op | verdict |
|---|---|
| `sop8` → **`getidx dst, {b\|a0}+k`** | secondary op 9's counterpart: copy an address-latch quad into a register.  "hasDst=1, nsrc=0, unit 6" in the vendor opcode table [BIN `0x1dd4a0`]; two independent uses (`mtx4036` reads both quads, `mtx1079` saves and increments `a0`) [BIN] |
| `smul` = primary `0x89` | **scalar × vector**: the opcode-info record `06 0f 88 00 48 00 28 00 18 00 01 02 01` says `dst.c` reads `src0.w` and `src1.c` — a broadcast of `src0.w` [BIN `0x1dd390`].  Unit 1, the same unit as `rcp`/`rsq`/`0x8b..0x8d`.  In the live vertex program `smul r5, r2, r2.xyz1` is the perspective divide, so `0x89 = src1 × (1/src0.w)` [INF, strong]; `0x8a` is the same with `rsq` |
| `sop2` / `sop3` | secondary ops 2 and 3, opcode-info **class 0** (`mov` is class 1, `frc`/`floor`/`movsat` class 2, `rcp`/`rsq` class 6), componentwise, one source, unit 6 — the same unit as `mov`.  Class 0 has no D3D mapping at all.  Both are emitted *paired with the primary op that produced the value*, writing the **same register the primary writes** (`smul T0, T0, r12 \| sop3 T0, v0.1111` in `mtx1223`/`mtx458`, `sub o0, … \| sop2 r5.w, r5` in the live tail), which a plain move cannot be — the most economical reading is a **predicated/select move** (substitute the source where the primary's result is invalid, e.g. `w = 0` in a divide).  **[OPEN]** |
| `op6e` = primary `0x6e` | absent from the D3D opcode-info table (null record) — a hardware opcode outside the D3D subset.  One sighting: `mtx1824` instruction 14, immediately before `end`, with **both write masks zero** (`op6e —, v0.yzz0, v0, v0`).  A destination-less instruction at the end of the "publish the resident entries" program reads as a commit/barrier. **[OPEN]** |
| `ctl33 ctl35 ctl0d ctl27 ctl2b ctl05 ctl04 ctl06 ctl41 ctl42 ctl43` | §2 |
| `br` | §2; `target = i + 1 + imm`, condition in `w0[17:14]` |
| `brn` | **does not exist** — opcode `0x46` is `shli`, §2 |
| `mova` | §3.2 |

---

## 8. Corrections to earlier documents

| doc | claim | correction |
|---|---|---|
| `parhelia-warp-isa-complete.md` §4.1 | "opcodes < 0x60 … have no operand fields" | true only of what the D3D compiler emits.  The class has three 5-bit creg operands and a 32-bit immediate; the vendor's own patcher enumerates them as kinds 32–35 [BIN] |
| `parhelia-warp-isa-complete.md` §2 | `w1[13]`/`w1[26:25]` = "index-register select / base-register select" | they are one field: entry (`w1[13]`) and component (`w1[26:25]`) of the **address-latch file**; `b0..b3` and `a0.x..w` are two entries' components, not a separate file [INF] |
| `parhelia-warp-isa-complete.md` §3.5 / §9 | "Base 1's value and how the bases are programmed are [OPEN] (candidates: fixed 128×k, or written through the `0x910/0x938` port / a block load)" | neither: they are written by `mova T0, v#` inside a bound microprogram [HW+BIN] |
| `parhelia-warp-isa-complete.md` §4.2 | secondary op 8 "writes a destination with no source [OPEN: constant/zero write]" | `getidx` — reads the address-latch quad [BIN] |
| `parhelia-warp-programs-decoded.md` §4.1 | kind 3 `ctl33` "operands T2, v1?, T3, T4, T5 and src2 = 1..5" | the destination cregs are `c29` and `c17..c21`, not 1..5 (the field is `w0[24:20]`, 5 bits) |
| `parhelia-warp-programs-decoded.md` §4.2 | "`sop8 r3` = save `a0` -> r3 [INF]"; "`sop8 T1.xy` = `a0 += 1` [INF]" | confirmed, and generalised: `getidx dst, {b\|a0}+k` |
| `parhelia-warp-programs-decoded.md` §4.3 | "`mova T0/T2/T4/T6` = base registers b0..b3" | `T0/T2/T4/T6` are four *contexts*; within one context `b0..b3` are `T0.x..T0.w` |
| `parhelia-warp-programs-decoded.md` §8 item 6 | "the `ctl33/ctl35` operand encodings of kinds 3/1 — needed only to program base registers other than the vendor's values" | `ctl33`/`ctl35` are `getc`/`streg` and have nothing to do with the base registers; §6 gives the base-register sequence |
| `tools/parhelia-warpdis2.py` | `0x46 = brn` | `0x46 = shli` |
| MTXPARM self-test | "346 instructions" | 350 — the last four are the failure exit the `br`s target |

---

## 9. Still open

1. `salu5` (0x05) and `salub` (0x0b) — the exact scalar functions (xor/add/and,
   and the conditional form the stage-6 program uses).
2. The synchronisation group `0x0d 0x25 0x27 0x2b` and the destination-less
   `op6e`; the `0x28000102`/`0x08000000`/`0x03800000` descriptors they write to
   hardware register `[0x000]`.
3. The base of the `ldreg`/`streg` window (`0x1000 + a` vs `0x1034 + a`),
   settled by one readback experiment.
4. The `br` condition-code encoding (`w0[17:14]`): only `0 = always` is proven.
5. `w1[19:14]` on `mova`/`putc`: `0x3c` vs `mtx251`'s `0x3f`.
6. Whether `putc` to destination file 0 updates the *address* copy of the latch
   the way `mova` does (Route B in §6.2).
7. Secondary ops 2/3 (class 0) and the unused control opcodes.
