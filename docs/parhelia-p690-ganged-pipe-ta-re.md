Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 ganged texture pipes: the combiner merge instruction

This is a static audit of the retained vendor binaries. No target was touched
and no driver source was changed. It explains the P690 hardware result for a
single texture stage on pipe mask 5 (`0x1200 = 0x00040105`):

- With `0x1264 = 0x00004000` rows 0-3 of every 8-row band sample correctly and
  rows 4-7 read 0.
- With `0x1264 = 0x00004040` (Areg = 2) the bands are swapped.

It also records what the vendor emits for a ganged stage.

Tags: **PROVEN** means read from machine code, table bytes or a vendor ring
capture at the cited address. **INFERRED** means derived from proven facts but
not executed on hardware. Windows addresses are image VAs (image base
`0x10000`; for `PSER`, file offset = VA − `0x10000` in `.text` and `.data`, as
the section headers give `.text` VMA `0x10300`/offset `0x300` and `.data` VMA
`0x1a1b00`/offset `0x191b00`). Linux addresses are VMAs of the unlinked object.

Evidence identity (SHA-256) is the same as in
`parhelia-p690-texture-pipes-re.md`: `PSER` `52b068f9…`, `PARD` `9072c647…`,
`KRN` `5f478829…`. Additional ring captures used here:

| file | SHA-256 |
|---|---|
| `captures/gl-matrix/tex-secs.txt` | `0f8a45318a17c58012af867e06f8243a8df583974957eb73590253fc1a81917d` |
| `captures/gl-matrix/texmiplin-secs.txt` | `c457c8cd82dfa29835559f904e14acbe7220e4bcebe00145b06d7f35ebc03831` |

## 1. Answer

1. **The "TA slot" is not a texture-address instruction.** The pair
   `0x1264 = 0x00004000`, `0x126c = 0x00000c00` is an ordinary combiner
   instruction, **`ADD t0, t0, t1`**. The vendor inserts it only when a stage
   runs on a ganged pipe mask. It sums the partial results of the two ganged
   pipes into the first pipe's register. Our words were copied from 0528
   captures, where a single stage runs on mask 3 (pipes 0+1). There the second
   pipe is pipe 1, so Breg = 1 (**PROVEN**, §3 and §5).

2. **For mask 5 the vendor emits exactly one merge instruction,
   `ADD t0, t0, t2`**, as the first combiner instruction of the program:

   ```
   0x1264 = 0x00008000 | slot     Areg = 0 (t0), Breg = 2 (t2), mods 0
   0x126c = 0x00000c00            ADD, dreg = t0, scale 0, no sat
   0x1260 = 0x00008000            alpha half, same instruction
   0x1268 = 0x00000c00
   ```

   The word is **PROVEN** in all three binaries. It also appears verbatim, at
   slot 4, in a real vendor ring (§5). The only difference from the mask-3
   word is Breg: 2 instead of 1. There is no second TA slot. There is no
   gang/pair bit in Amod `[13:10]`, in `0x126c` bits 15/16 or in bits 25-31.

3. **`[obj+0x10f4+8k]` (the stage's first physical pipe) goes only into
   combiner register fields.** It becomes the register code of every combiner
   operand or destination that names texture register `t_k`: Areg `[9:5]`,
   Breg `[18:14]`, Creg `[27:23]` or dreg `0x126c[23:20]`. No register in
   `0x1100..0x13ff` maps logical sources to physical pipes or declares the
   gang (**PROVEN**, §4).

4. **The `multi` field of the pipe property table has no reader** in `PSER`,
   `PARD` or `KRN` (**PROVEN** by exhaustive reference search, §6). The gang
   is keyed on the literal mask value instead. `mask == 5` sets a per-stage
   flag bit `0x20`, which costs one combiner slot and makes the compiler emit
   the merge ADD.

5. **Hardware reading (INFERRED, consistent with every observation):**
   - Each pipe of a gang writes 0 into its register for fragments it does not
     own. Parked pipes read 0.
   - `ADD t0, t0, t1` therefore returned pipe 0's rows plus 0.
   - With Areg = 2 it returned pipe 2's rows plus 0.
   - Every other Areg value named a register holding neither half.
   - `ADD t0, t0, t2` returns both halves.

## 2. The wrong reading this corrects

`parhelia-multitexture.md` §6.1 says the TA word's Areg "selects the physical
texture pipe" and that `0x1264 = 0x00004000` is a "texture-address slot".

The part about physical pipes is correct in effect: `t_n` is pipe `n`'s
output. The rest of that reading is wrong:

- The word is `ADD t0 = t0 + t1`. Breg = 1 is a real second operand, not a
  constant.
- A stage on a single pipe (`0x00040101`) gets **no** such instruction from
  the vendor.
- Our driver's `ADD t0, t0, t1` on mask 1 works only because pipe 1 is idle
  and reads 0 (INFERRED).

The two-unit P690 result in `parhelia-multitexture.md` §6.1 fits the same
model:

- `0x1264 = 0x00004042` with dreg `t1` computes `t1 = t2 + t1(=0)`.
- The vendor emits no merge for the `{1,4}` route. The ALU instructions read
  `t0` and `t2` directly (§4).

## 3. Windows compiler path (PSER; PARD is identical)

### 3.1 Per-stage flag word: `PSER 0x177560` (resource count)

The function walks stages (`edi = stage + 0xc8`, so `[edi-0x50]` = stage
`+0x78` = pipe mask). It builds a flag word per stage at `[obj+0x2140+4k]`
(`0x177729`). `[obj+0x30]` counts combiner slots:

```
177628  cmp [edi-0x50],3   -> or edx,0x08 ; slots += 1     (mask 3)
177637  cmp [edi-0x50],0xc -> or edx,0x10 ; slots += 1     (mask c)
177647  cmp [edi-0x50],5   -> or edx,0x20 ; slots += 1     (mask 5)
177657  cmp [edi-0x50],0xf -> or edx,0x40 (or 0x80 when stage+0x114 or
                              stage+0x118 == 3, i.e. anisotropic) ; slots += 3
177746  cmp [ecx+0x30],0xa ; ja -> reject candidate (more than 10 slots)
```

These are the only compares of the stage mask against a multi-pipe value
outside the validator (`0x17b7a8`, `0x17b7d3`) (**PROVEN** by scanning
`0x170000..0x190000` and `0xcb000..0xcc000` for compares with 5).

The PARD twin is at `0x175677..0x175697`.

### 3.2 IR generation: `PSER 0x1778c0`

For each stage `k` (flag word pointer `eax = obj+0x2140+4k`) the function
emits IR records at `obj+0x12a4 + n*0x154`. Each one is inserted at the
**head** of the instruction list `obj+0x1fec`: `[ebx+4] = [edi+4]`,
`[ebx+8] = edi`, `[edi+4] = ebx`, for example at `0x177e8b..0x177eb4`. The
ALU program was appended to the tail earlier (`0x1778f2..0x17790c`). So merge
instructions come before every ALU instruction and get hardware slot 0 (then
2, 4, …):

```
177e05  test [eax],0x08 -> 0x1829d0(obj, rec, 0, 1)          (mask 3)
177e41  test [eax],0x10 -> 0x1829d0(obj, rec, 2, 3)          (mask c)
177e7d  test [eax],0x20 -> push 2 ; push 0 ; 0x1829d0(obj, rec, 0, 2)   (mask 5)
177ebb  test [eax],0x40 -> 0x182a50  (three records)         (mask f)
177f32  test [eax],0x80 -> 0x182b90  (three records)         (mask f, aniso)
```

PARD twin: `0x175ebd: test [eax],0x20 ; push 2 ; push 0 ; call 0x180a20`.

### 3.3 The merge builder: `PSER 0x1829d0` -> `0x183830`

`0x1829d0` sets IR opcode `[rec+0x10] = 5`. It calls `0x183830(rec, a, b)`
twice, once for the RGB sub-record (`rec+0xd4`) and once for the alpha
sub-record (`rec+0x114`). `0x183830`:

```
183837..183851  operand kinds A,B,C: (x & ~5) | 2      kind 2 = literal
18386f..18388a  [sub+8]  (0x126c) = ((a & 0xf) << 20) | 0xc00
18387a          [sub+0xc] = 1                          destination is literal
183890..1838ab  [sub+4]  (0x1264) = (b << 14) | (a << 5) | slot
```

So for `(a, b) = (0, 2)`: `0x1264 = 0x00008000`, `0x126c = 0x00000c00`, for
both halves. For `(0, 1)` it is `0x00004000`/`0x00000c00`, the 0528 word. For
`(2, 3)` it is `0x0000c040`/`0x00200c00`. PARD twin builder:
`0x182f20` (`or esi,0xc00` at `0x182f74`).

The quad builders use fixed-word variants of the same helper. They are
**PROVEN**, and §5 shows the mask-f merge on hardware:

| helper | `0x1264` | `0x126c` | instruction |
|---|---|---|---|
| `0x184fc0` | `0x4000` | `0x000c00` | `ADD t0, t0, t1` |
| `0x1838c0` | `0xc040` | `0x200c00` | `ADD t2, t2, t3` |
| `0x185020` | `0x8000` | `0x000c00` | `ADD t0, t0, t2` |
| `0x183920` | `0x4000` | `0x0e0c00` | `ADD_d2 t0, t0, t1` (anisotropic) |
| `0x183980` | `0xc040` | `0x2e0c00` | `ADD_d2 t2, t2, t3` (anisotropic) |

The aniso form halves each pair sum (`0x126c[19:17] = 7` is ÷2,
`parhelia-fragment-isa.md` §8). So in that mode both pipes of a pair
contribute to the same fragment. In plain mode they contribute to disjoint
fragments (INFERRED from the ÷2 difference).

### 3.4 Words reach the hardware unchanged

`0x176430` walks the IR list and calls `0x183310` per record. That calls the
RGB packer `0x1856e0` on `rec+0xd4` and the alpha packer `0x183eb0` on
`rec+0x114`. The packers rewrite a register field only for:

- **kind 1**, a pointer to a texture-register state: code = `[ptr+4]`,
  `0x185713` / `0x18578e` / `0x18580c`;
- **kind 3**, a constant;
- a **dreg with `[sub+0xc] == 0`**: `0x1856e8..0x185704`.

Kind 2 with `[sub+0xc] = 1` is copied verbatim. The results land in the
hardware record at `+0x8`/`+0xc` (`0x185883`, `0x18588c`). The record's
bit 0 is set by both packers (`0x185889`, `0x18402a`), and the alpha half is
emitted (§5 shows it on the ring).

## 4. Where `[obj+0x10f4+8k]` goes (Question 1)

- **Writer.** `PSER 0x177770` stores `firstPipe = [0x1d8684 + 12*mask]` of
  each stage's chosen mask at `[obj+0x10f4 + 8k]` and clears bit 0 of the
  flag dword at `[obj+0x10f0 + 8k]`. `0x1d8680` is the **non-LX** property
  table, used for all chips. Its entry for mask 5 is `{0, 0, zero-cost}`, so
  firstPipe = 0, the same as the LX table `0x1dbe70[5] = {1, 0, PAIR}`
  (**PROVEN**, both tables dumped). Defaults come from `0x1d9b38 = {0,1,2,3}`
  (`0x176e42`).
- **Readers.** The D3D instruction handlers build operand references to
  `t_n` as `lea [obj + n*8 + 0x10f0]` (kind 1). Examples:
  `0x1840b1` (destination: `[sub+0x10] = ptr`, `[sub+0xc] = 0`, flag
  `or [ptr],8`) and `0x185e53` (source, `or [ptr],4`); 20 such sites lie in
  `0x1840b1..0x186566`. The packers then read `[ptr+4]` into Areg, Breg,
  Creg or dreg (§3.4).
- **Neighbouring flag bits** are compiler bookkeeping only:
  - bit 1 (set at `0x17794b` for stage flag `0x100`) makes the packer remap
    the source modifier through `0x1de930 = {4,5,2,3,4,5,0…,0xf}` (signed
    texture expansion, `0x18572b..0x18574a`);
  - bits 2 and 3 mark use;
  - bit 4 is tested at `0x1775cf`.
- **No other consumer.** `+0x10f4..+0x110c` are written only at
  `0x177791..0x177828` and `0x1784d5` (reset), and read only through the
  kind-1 pointers.
- **No register maps pipes.** The complete MTXPARD register-write set
  (`parhelia-fragment-isa.md` §9.5.7) has no writer in `0x1238..0x125c`,
  `0x1288..0x12ac`, `0x12b4..0x12bc`, `0x12cc..0x12e0` or `0x12ec..0x12f0`.
  Gang-dependent state is limited to §7.

**KRN twin (PROVEN):**
- `mtx1080` @`0x6efb0` writes `[ctx+0xf3c+8k] = mtx2971[mask].firstPipe`
  (`0x6efd0`, relocation `mtx2971+4`; `mtx2971` = non-LX table, `.data
  0x194a0`) and clears bit 0 of `[ctx+0xf38+8k]`.
- The operand builders `mtx2619` (`0x6a307`, `0x6a3f4`, `0x6a4e2`,
  `0x6a5fe`) and `mtx1653` (`0x6b276`, …) point texture operands at
  `ctx+0xf38+8n` with kind 1 (`0x6a31b..0x6a32e`).
- The packer `mtx2505` @`0x6ad80` reads `[ptr+4]` into dreg (`0x6adac`) and
  Areg (`0x6ae64`).

## 5. Vendor ring evidence (0528, vendor Linux GL, `captures/gl-matrix`)

Decoded with `tools/decode-gl-secs.py`:

```
tex-secs.txt, SEC 195c3fee (bilinear, one stage):
   1200 = 00040103   1204 = 0001000c                   mask 3, parked c
   1264 = 00004000   1260 = 00004000   126c = 00000c00   1268 = 00000c00   ADD t0,t0,t1
   1264 = 08000002   1260 = 08000000   126c = 01400000   1268 = 01400000   MUL_sat r0,t0,v0
   1264 = 00000084   126c = 00400400                                        pad

texmiplin-secs.txt, SEC 195e31a6 (trilinear, one stage):
   1200 = 0004010f   1204..120c = 0                   mask f
   1264 = 00004000   1260 = 00004000   126c = 00000c00   1268 = 00000c00   ADD t0,t0,t1
   1264 = 0000c042   1260 = 0000c040   126c = 00200c00   1268 = 00200c00   ADD t2,t2,t3
   1264 = 00008004   1260 = 00008000   126c = 00000c00   1268 = 00000c00   ADD t0,t0,t2
   1264 = 08000008   1260 = 08000000   126c = 01400000   1268 = 01400000   MUL_sat r0,t0,v0
   1264 = 0000008a   126c = 00400400                                        pad
```

These are exactly the §3 builder outputs: flag `0x08` for mask 3 and flag
`0x40` for mask f. The third quad instruction is the mask-5 merge word, so
`ADD t0, t0, t2` with alpha half is **observed on real vendor hardware
output**, not only in code. The texturing instruction reads `t0` after the
merge.

## 6. The pipe property `multi` field (Question 3)

- **PSER.** The raw dword `0x1d8680` occurs once in the whole file
  (`0x17b996`, `lea ebx,[ebx*4+0x1d8680]`). `0x1d8684` occurs 17 times, all
  firstPipe reads in `0x177770..0x177da1`. `0x1dbe70` occurs once
  (`0x17b98d`, `lea`). After either `lea`, only `[ebx+8]` (costTable) is read
  (`0x17b99a`). No other pointer into either table exists (raw search over
  every 4-byte offset of both tables).
- **KRN.** Relocations against `mtx1861` (LX, `.data 0x1b5c0`) and `mtx2971`
  are:
  - `mtx2124` `0x71724`/`0x717b1` (`lea`, then only `[rcx+8]` at `0x7172a`);
  - `mtx2971+4` (firstPipe) at `mtx1080 0x6efd2` and nine sites in
    `mtx3363`.

  Field 0 is never read.

Mask 5 is still special. It is recognised by value in the validator and in
the resource count (`0x177647`, KRN `mtx3025 0x6f205`), and it indexes the
pair cost table in the rate model. So the `multi` flag is data the shipped code never consumes; the
ganging behaviour is hard-wired to the mask values 3, c, 5 and f.

## 7. Everything that differs between mask 5 and mask 1 (Question 3)

| item | mask 1 | mask 5 | evidence |
|---|---|---|---|
| `0x1200[3:0]` | 1 | 5 | `PSER 0x1767e0` |
| park on first free unit | `0x0001000e` | `0x0001000a` | `PSER 0xcbac8..0xcbb0f`, `KRN 0x3f878..0x3f8d5` |
| `0x11e8` bits (`PSER 0x177350`) | bit 22 | bit 22; bits 21/24 clear because 0 and 2 lie in different pairs | `parhelia-p690-texture-pipes-re.md` §5 |
| combiner | no merge instruction | `ADD t0,t0,t2` at the head of the program, both halves | §3, §5 |
| slot budget | n | n + 1 (count ≤ 10; >5 pads to 10, sets `0x80000` and halves the rate cap) | `0x177650`, `0x177746`; rate cap `0x177230` |
| t-register code | 0 | 0 (firstPipe) | §4 |
| rate cost table | single `0x1dd330` | pair `0x1dc930` | `0x17b98a` |
| validator | any kind | kind 0/4/0x10/0x11 only, no dependents | `0x17b7a8`, `0x17b7d3` |

No sampler-window register (`0x11c0..0x11e4`) depends on the mask. The
per-unit sampler state is emitted once for the unit, not per pipe. No code
programs the 4-row band split. That split is therefore a hardware property
(INFERRED from the absence of any writer).

## 8. Linux KRN twin (Question 4)

| role | PSER | KRN |
|---|---|---|
| per-stage flags, mask==5 -> `0x20` | `0x177560` (`0x177647`) | `mtx3025` @`0x6f130` (`0x6f205..0x6f20d`), flags at `ctx+0x2958+4k`, mask at TA record `+0x90` (`ctx+0xa88`) |
| IR generation | `0x1778c0` | `mtx3363` @`0x6f870` |
| mask-5 call | `0x177e93` push 2, push 0 | `0x6fe20`: `edx = 0`, `ecx = 2`, `call mtx1484`, then `mtx556` (insert at list head `ctx+0x2720`) |
| mask 3 / c calls | `(0,1)` / `(2,3)` | `0x6fec0` `(0,1)` / `0x6fe70` `(2,3)` |
| merge builder | `0x1829d0` + `0x183830` | `mtx1484` @`0x6d050` (`[rec+0x20] = 5`), `mtx1585` @`0x6b900` RGB, `mtx2507` @`0x6a810` alpha |

`mtx1585` writes the same fields:
- kinds `[+0x20]/[+0x38]/[+0x50] = 2`;
- `[+0x10] = 1`;
- byte `+0xd` of `0x126c`: `(x & ~2) | 0x0c`, which gives opcode `0xc00`;
- byte `+0xe`: `a << 4` (dreg);
- `0x1264 = (a << 5) | (b << 14)`, with Amod, Bmod, Creg and Cmod cleared
  (`0x6b94d..0x6b99c`).

`mtx2507` is byte-for-byte the same for the alpha half.

The TA records built by `mtx2132`/`mtx1652` (opcode via `mtx1411`) carry no
gang information. `+0xa4`/`+0xa8`/`+0xac` are logical stage indices:

- `mtx598` sets a record's `+0xa4` from its own `+0xa8`, or from its first
  consumer's `+0xa8` for hardware opcode 8. It then uses `+0xa4` to select
  the 0x82-byte state block (`0x70e14..0x70e2f`, `0x70e93`).
- `mtx1723` packs `+0xa8 & 7` into a descriptor byte (`0x718f5`).
- `+0xac` is passed only to the bump builder `mtx2949` (`mtx3363 0x6f993`,
  `0x6fa74`, `0x6fb55`).

None of them feeds the merge or any pipe register (**PROVEN** by the
complete reader list in `0x6c000..0x76000`).

## 9. Hardware test list (ordered, most likely first)

Keep the current mask-5 state:
- `0x1200 = 0x00040105`
- unit 0 window, then `0x11e8 = 0x00500000`
- `0x1204 = 0x0001000a`, `0x11e8 = 0`
- `0x1208 = 0x120c = 0`, each followed by `0x11e8 = 0`
- `0x12f4 = 32.0`

Verify on scanout.

1. **Merge word, vendor form** (PROVEN word): slot-0 instruction
   `0x1264 = 0x00008000`, `0x1260 = 0x00008000`, `0x126c = 0x00000c00`,
   `0x1268 = 0x00000c00`. Everything else unchanged. Expect all eight rows
   of each band to be correct.
2. If RGB is correct but alpha shows the band pattern: make sure the alpha
   half of slot 0 is actually emitted (slot flag bit 0) with the same words.
   The vendor ring carries it (§5).
3. Diagnostic only (INFERRED equivalent): `0x1264 = 0x00000040`
   (Areg = 2, Breg = 0, dreg still `t0`). This should give the same image as
   step 1.
4. Windows `0x11e8` form for completeness: `0x11e8 = 0x00400000` with
   `0x12f4 = 16.0`. The measured insensitivity to `0x11e8` bits 20/22
   predicts no change. Do **not** set bits 21/24 (`0x01600000`) for mask 5:
   the vendor sets them only for a fully owned pair (0,1) or (2,3), and that
   value locked up the engine.
5. Optional cleanup, vendor form, after step 1 works:
   - For single-pipe routes (`0x00040101`) the vendor emits no merge. Our
     `ADD t0, t0, t1` reads the idle pipe 1 and can be dropped to save a
     slot.
   - For the two-stage `{0x40101, 0x40104}` route the vendor emits no merges
     at all and the ALU reads `t0` and `t2` directly.

   Keep programs at 5 slots or fewer to avoid the 10-slot padding and the
   `0x80000` pipeline bit.

## 10. Reproduction

```sh
D=xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll
P=xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
K=matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
objdump -h $D                                                            # section map
objdump -d -Mintel --start-address=0x177560 --stop-address=0x17776a $D   # flags, mask==5 -> 0x20
objdump -d -Mintel --start-address=0x177770 --stop-address=0x177830 $D   # firstPipe -> [obj+0x10f4+8k]
objdump -d -Mintel --start-address=0x1778c0 --stop-address=0x177fe0 $D   # IR generation, head insert
objdump -d -Mintel --start-address=0x1829d0 --stop-address=0x182cd0 $D   # merge record builders
objdump -d -Mintel --start-address=0x183830 --stop-address=0x1839f0 $D   # (a,b) ADD word, quad words
objdump -d -Mintel --start-address=0x184fc0 --stop-address=0x185090 $D   # quad words 0x4000 / 0x8000
objdump -d -Mintel --start-address=0x1856e0 --stop-address=0x1858be $D   # RGB packer, kind 1 -> [ptr+4]
objdump -d -Mintel --start-address=0x17b8d0 --stop-address=0x17ba4d $D   # rate: only [entry+8] read
objdump -d -Mintel --start-address=0x175677 --stop-address=0x17569b $P   # PARD mask compares
objdump -d -Mintel --start-address=0x175ebd --stop-address=0x175ee5 $P   # PARD mask-5 merge call
objdump -dr -Mintel --start-address=0x6f130 --stop-address=0x6f309 $K    # mtx3025
objdump -dr -Mintel --start-address=0x6fe20 --stop-address=0x6ff10 $K    # mtx3363 merge calls
objdump -dr -Mintel --start-address=0x6d050 --stop-address=0x6d10e $K    # mtx1484
objdump -dr -Mintel --start-address=0x6b900 --stop-address=0x6b9a1 $K    # mtx1585
objdump -dr -Mintel --start-address=0x6efb0 --stop-address=0x6efee $K    # mtx1080 firstPipe
objdump -dr $K | grep -E 'R_X86_64.*(mtx1861|mtx2971)'                    # multi field unread
python3 tools/decode-gl-secs.py --regs 0x1200,0x1204,0x1264,0x126c,0x1260,0x1268 \
    captures/gl-matrix/tex-secs.txt captures/gl-matrix/texmiplin-secs.txt
python3 - <<'EOF'                                    # property tables {multi, first, cost}
import struct
d=open('xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll','rb').read()
for base in (0x1d8680, 0x1dbe70):
    print(hex(base), [struct.unpack('<3I', d[base-0x10000+12*m:][:12])[:2] for m in range(16)])
EOF
```
