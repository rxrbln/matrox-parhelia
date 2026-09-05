# Parhelia WARP fragment registry — what every fragment computes

Copyright (C) 2026 René Rebe <rene@exactco.de>

Companion to `docs/parhelia-warp-codegen.md` (which fragment is emitted for
which GL state — the decision tree, the relocation and patch handlers) and to
`docs/parhelia-vertex-isa.md` PART 2 (the instruction encoding). Those two
answer *when* and *how*. This file answers **what** — the semantics of the 574
microcode fragments, so the library can be written from scratch instead of
shipped.

Why it matters: the 574 fragments are vendor object code and cannot ship in a
GPL-2 driver (`parhelia-warp-codegen.md` §6.1). The rewrite needs the register
calling convention and the per-family contract, not the vendor bits.

Evidence tags: **[CONFIRMED]** = read out of the binary, or proved by a
structural identity that has no other reading; **[INFERRED]** = best reading,
self-consistent across several fragments, not proved; **[OPEN]** = not
determined, and *said so* rather than guessed.

Nothing in this file has been executed on a Parhelia. One prior hardware run is
relevant: a transform program built from the documented field layout streamed
onto real silicon and the engine began executing it, so the **encoding** is
trustworthy (`parhelia-warp-binding.md`, "Attempt 5"). What was missing is
semantics, and that is what follows.

Everything here is reproducible with the existing tools:

```
tools/parhelia-warpreg.py matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o -c
tools/parhelia-warpreg.py matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o -i 0x50
```

---

## 1. The register calling convention

This is the deliverable everything else depends on, so it comes first.

### 1.1 Three register spaces, not one

| space | width | addressed by | who writes it | who reads it |
|---|---|---|---|---|
| `r0…r127` | 128 × vec4 | `src0` w1[6:2], `src1` w0[31:27], `src2` w0[24:20], each widened to 7 bits by `exthi`/`extsel` | the **input assembler** (vertex attributes), the **CPU constant store**, and microcode itself through port B | microcode, all three source slots |
| `b0…b15` | the low 16 of `r*` — **`bN` *is* `rN`** | port B, `dst1` w0[17:14], mask `wmask1` w0[13:10] | microcode | — (read back as `rN`) |
| `a0…a511` | 512 × vec4 (× 4 banks) | port A, `dstA` = {w1[13], w1[26:25], w1[21:20], w0[7:4]}, mask `wmask0` w0[3:0] | microcode | the **setup/raster unit**, and the CPU through `0x910`/`0x914`/`0x938` kind 3 |

**`bN` is `rN` [CONFIRMED by three independent lines].**

1. *Substitution proof.* Fragments `0x5a` and `0x58` are the same fog body. `0x5a`
   is `op69 b1.xyzw ← r3.xyzw` followed by `op75 b1.z ← r116.z, r1.z`. `0x58`
   drops the `op69` and the very same operand reads `r8.z` instead of `r1.z`.
   The library author replaced "the value I just put in `b1`" with "a value that
   is already in a register" — that is only meaningful if `b1` and `r1` are the
   same storage. The identical substitution appears in the `0x5c`/`0x5e` and
   `0x60`/`0x62` pairs.
2. *Offset sweep.* Over the whole corpus, "instruction *i* writes `b_k`,
   instruction *i+1* reads `r_(k+off)`" peaks sharply at `off = 0`:
   36.9 % at 0, versus 19.9 % at +1, 15.6 % at −1 and 4–11 % for |off| ≥ 2.
   (13 987 adjacent pairs; a uniform-random reader of `r0…r15` would hit 13.9 %.)
3. *Per-register lift.* For the registers the generators use as scratch the
   enrichment is unambiguous — `r1` 2.6× / 2.3× / 1.7× over the three source
   slots, `r2` 2.1× / 2.4×, `r4` 3.2× on `src1`, `r13` 2.1× on `src0`. For the
   registers that are vertex **inputs** (`r3`, `r6`, `r8`, `r10`, `r11`) the lift
   is below 1, which is exactly right: they are read before anything writes them.

`dst1` is 4 bits, so microcode can only ever write `r0…r15`. In the whole
574-fragment library `b15` is never written and `b14` only 32 times; the working
set is **`b0…b13`**.

**Port A is a separate, write-only-from-microcode output file [CONFIRMED].**
`mtx676`'s self-test sweeps `a0…a383` and `mtx612` reads all 384 back through
`0x910`/`0x914`/`0x938`; no fragment in the library ever reads a register index
above 121, and the transform fragments write the *same* dp4 result to a port-B
temp **and** a port-A register in one instruction (`op77 b3.w, a259.xyzw`), which
would be pointless if the two were the same storage.

### 1.2 `r0…r15` — vertex inputs and temporaries, one file

The 16 attribute bindings the kernel programs (`parhelia-vertex-pipeline.md`
§6.2: `stream*2 + slot`, flat index 0..15) are exactly the 16 registers port B
can write. The natural and self-consistent reading is that the input assembler
preloads `r0…r15` per vertex and microcode then reuses the unbound ones as
scratch [INFERRED, strong].

Positions established by dataflow, valid **at the point in the program where the
family that uses them runs** — this is a compiler convention, not a hardware
wiring, and it is respected by the whole vendor library:

| register | role | evidence |
|---|---|---|
| `r0` | **object-space vertex position**. The invariant `src0` of every 4×dp4 transform block; the vector of object-linear texgen (`0x74`–`0x77`) | [CONFIRMED] read by 559 of 574 fragments, live-in in 537 |
| `r1` | primary scratch / running accumulator | [CONFIRMED] highest port-B def-use lift of any register |
| `r2` | **transformed clip-space position**, once the generator's `dst1 = 2` patch has run | [CONFIRMED] — `mtx2921` patches the position dp4s to `b2` (`P(4,2,1..4)`, `parhelia-warp-codegen.md` §4.2), and the clip-plane fragments `0xb0`–`0xb5` dot **`r2`** against the plane constants, and `0x54` emits `r2` to `a11` |
| `r3` | the *unpatched* position slot (`b3` is the library default) and, in the fog and texgen families, the eye-space position | [INFERRED, strong] |
| `r4` | normal / second geometric vector | [INFERRED] — `0x69` normalises `r4` in place, `0xb8` runs a 3×dp3 with `r4`, the texture prologues `0x6b`–`0x6e` build the texgen vector from `r0` and `r4` |
| `r5` | **primary colour** (`glColor`, the `GL_DIFFUSE` colour-material source) | [CONFIRMED] by three independent selector triples: `0x55/0x56/0x57` (`src1` = `r84`/`r5`/`r6`), `0x1d7/0x1d8/0x1d9` (`src2` = `r84`/`r5`/`r6`), `AddEfm/AddEfd/AddEfs` (`src1` = `r87`/`r5`/`r6`) |
| `r6` | **secondary colour**; also the texgen source vector the prologue leaves behind | [CONFIRMED] same three triples |
| `r7`,`r8`,`r9`,`r10` | **texture coordinate inputs, units 0…3** | [CONFIRMED] — `mtx2177` patches `src0 = i+7` (`P(0x0c, i+7, 0)`) and `src2 = i+7` (`P(0x06, i+7, 0)`), and `0x01`/`0xaa` copy `r7,r8,r9,r10` into the texcoord outputs |
| `r11` | **explicit fog coordinate** | [INFERRED, strong] — the `flag != 0` half of every fog mode reads `r11.x` where the `flag == 0` half computes a length from `r3` (§4.7) |
| `r12`,`r13` | further scratch (heavily port-B written) | [CONFIRMED usage] |

Note the deliberate hazard: `mtx2177` patches the per-unit texcoord working
register to `b(3+i)` or `b(6+i)`, i.e. it **overwrites `r5`/`r6`** for units 2
and 3. A from-scratch generator should do its own register allocation rather
than copy this. [CONFIRMED mechanism, `P(0x04, i+3, 0)` / `P(0x04, i+6, 0)`]

### 1.3 `r16…r127` — the constant / uniform file

No fragment in the library ever writes a register above 14, so **everything from
`r15` up is CPU-loaded constant space** [CONFIRMED — `dst1` is 4 bits]. The
indices the library actually reads, and what they hold:

| registers | holds | evidence |
|---|---|---|
| `r0…r3` | the model-view-projection matrix rows, in the *canonical* (as-stored) form of a transform fragment | [CONFIRMED shape] — see the caveat in §7.1 |
| `r12…r17` | **user clip-plane equations 0…5** | [CONFIRMED] `0xb5` is six `op79` accumulating `r2 · r12 … r2 · r17` into `b5` |
| `r16`,`r17` | also read by `0xbb` and the texture prologues `0x6c`–`0x6e` | [OPEN] whether this is the same allocation |
| `r18`,`r19` | output-format-tail constants (`0x45`, `0x46`, `0x48`, `0x4a`, `0x4b`) | [OPEN] |
| `r20`,`r21`,`r22`,`r24`,`r28`,`r29`,`r44`,`r45`,`r60`,`r61` | constants of the auxiliary/programmable band only (§5) | [OPEN] |
| `r84` | **front material constant** (colour-material source 0) | [CONFIRMED] `0x1d7` vs `0x1d8`/`0x1d9`, `0x55` vs `0x56`/`0x57` |
| `r85`, `r86` | further front material terms, read by the lit T&L bodies and the colour-material expansion `0x31`–`0x3b` | [INFERRED] |
| `r87` | front material term used by the `epi1_B` accumulate words (`AddEfm`) | [CONFIRMED] |
| `r88` | a **face-independent** additive material/scene term — the same register appears in the front (`0x239`) and back (`0x23c`) accumulate words | [CONFIRMED] |
| `r89+4i … r92+4i` | **texture-matrix / texgen-plane rows S,T,R,Q of unit `i`** (unit 0 = `r89`–`r92`, unit 3 = `r101`–`r104`) | [CONFIRMED] — `mtx2177` patches `src1 = 0x59 + 4*i + stage` (`P(0x09,…)` + `P(0x11,…)`), and the stored fragments read `r89`/`r90`/`r91`/`r92` for S/T/R/Q |
| `r105…r108` | a second 4×4 matrix (rows 0…3), the one `0x42`/`0x43` apply | [CONFIRMED] — it is the block immediately after texture unit 3's, so it is the fifth matrix slot |
| `r110` | a constant of the `s ≥ 6` material variants (`0x207`–`0x21e`) and of `0xbe`/`0xbf` | [OPEN] |
| `r116`, `r117` | **fog parameters** (`r116.z`/`r116.w` are the ones actually used: scale and bias/density) | [CONFIRMED family, INFERRED component roles] |
| `r118` | **back material constant** — the back-face counterpart of `r84` | [CONFIRMED] `0x1da` is `0x1d7` with `src2` `r84 → r118` and the accumulator `a1/b1,b2 → a7/b7,b8` |
| `r121` | back-face counterpart of `r87` | [CONFIRMED] `0x224` reads `r121`+`r88` where the front `0x221` reads `r87`+`r88` |
| `r(7 * slot) + k` | **per-light constant block, 7 registers per light** | [CONFIRMED] — the relocator adds `7*value` to the operand index and `value` is the light slot (`parhelia-warp-codegen.md` §1.5) |

The highest extended index the library ever names is 121, so the CPU constant
store needs 128 vec4 slots at most.

### 1.4 `a0…a511` — the output file

167 distinct port-A destinations are written across the library. The ones the
fixed-function path uses, with the family that writes them:

| output | carries | written by | tag |
|---|---|---|---|
| `a0`, `a1` | the lighting accumulators, front (`a1`) and the shared/ambient one (`a0`) | lighting `0x16b`–`0x1d6`, material `0x1d7`–`0x21e` | [CONFIRMED] |
| `a2` | **fog factor** (`.w` component) | fog `0x5a`/`0x64`; also written by the colour-source fragments `0x55`–`0x57` | [CONFIRMED for fog] |
| `a3+i` | **texture coordinate output, unit `i`** | `0xa7` (default fill), `0xab`; `0x01`, `0xaa` | [CONFIRMED] — `mtx2177` patches `dst0 = i+3` (`P(0x01, i+3, 0)`) |
| `a7`, `a8` | back-face lighting accumulators | `0x1da`, `0x1e6`, `0xa9`, `0xad` | [CONFIRMED] |
| `a11` | receives the transformed position (`r2`) and the constant `(0,0,0,1)` | `0x54`, `0x02` | [INFERRED] |
| `a25` | point size | `0xc4`, `0xc5` | [INFERRED] |
| `a64`, `a65` | colour outputs (`a65` is written from the colour stage and by the lit bodies) | `0x45`–`0x47`, `0x4b`, `0x54`, `0x10`, `0x1f` | [INFERRED, strong] |
| `a68`,`a69`,`a70` | the `.w`/q component of three consecutive outputs, one per item of the `0x65+i` loop | `0x65`,`0x66`,`0x67` | [CONFIRMED shape, GL meaning OPEN] |
| `a82`, `a85`, `a87` | further output-format-tail destinations | `0x01`, `0x4d`, `0x4e` | [OPEN] |
| `a256`, `a259`–`a262`, `a324`–`a326`, `a389` | **the transformed position path** | `0x06`–`0x09`, `0x0f`, `0x50`–`0x53` | see below |

**Position output [CONFIRMED for the single-matrix case].** `0x0f` and `0x50`
are four `op77` dot products whose port-A destinations are, in order of the
destination component they compute:

```
b3.w , a259     b3.z , a260     b3.y , a261     b3.x , a262
```

so the clip-space position occupies **`a259`…`a262`, one output register per
component, `a259` = w … `a262` = x**. The multi-matrix blend variants
(`0x51`–`0x53`) additionally engage `a256` and `a324`–`a326`; which of those is
the accumulated result and which an intermediate is [OPEN].

### 1.5 Two encoding facts a rewrite must not get wrong

**Swizzle codes 4 and 5 are the constants 0.0 and 1.0** [CONFIRMED]. Fragment
`0xa7` fills the missing components of a texture coordinate, and `mtx2177`
patches its `wmask0` to one of four values depending on the declared component
count. The fragment's source swizzle is `(4,4,4,5)`; the GL default texture
coordinate is `(s,0,0,1)`:

| `ncomp` | patched `wmask0` | components written | required value | `(4,4,4,5)` gives |
|---|---|---|---|---|
| 1 | `0x0e` | y,z,w | 0,0,1 | 0,0,1 ✓ |
| 2 | `0x0c` | z,w | 0,1 | 0,1 ✓ |
| 3 | `0x08` | w | 1 | 1 ✓ |
| 1, matrix on | `0x0f` | x,y,z,w | 0,0,0,1 | 0,0,0,1 ✓ |

All four agree only if code 4 = 0.0 and code 5 = 1.0. This **corrects**
`parhelia-vertex-isa.md` §3, which guessed code 4 = 1.0 from the `.xyz4`
idiom — that idiom is `w = 0`, which is what transforming a *direction* needs.
Codes 6 and 7 occur (`r1.6666`, `r0.7777`, `r6.xy64`) and remain [OPEN].

**Operand routing is opcode-dependent** [CONFIRMED]. Three triples in the
library select the same three-way colour-material source through three
*different* operand slots:

| triple | slot that varies | values |
|---|---|---|
| `0x55` / `0x56` / `0x57` (opcode 0x75) | `src1` | `r84` / `r5` / `r6` |
| `0x1d7` / `0x1d8` / `0x1d9` (opcode 0x84) | `src2` | `r84` / `r5` / `r6` |
| `AddEfm` / `AddEfd` / `AddEfs` (opcode 0x71) | `src1` | `r87` / `r5` / `r6` |
| `AddAfd` / `AddAfs` (opcode 0x76) | `src0` | `r5` / `r6` |

So an implementation cannot assume a fixed "first operand is the data" rule; the
opcode picks which slot is the datum. `src2` is `r0` (inert) in 12 205 of the
19 001 arithmetic instructions, so `src2 = 0` is the safe default for opcodes
that do not need it.

---

## 2. Reachability — the fragments nothing selects

The codegen document reported 165 literal-reachable fragments and 16 reached by
nothing found. Redoing the reachability exhaustively — a basic-block constant
propagation over every call site of the four appenders (`mtx1503`, `mtx3579`,
`mtx497`, `mtx2475`) plus every literal `mov DWORD PTR [reg+disp], imm` in the
module bucketed by which runtime table it lands in — gives a cleaner picture and
**29** unreachable fragments, not 16.

### 2.1 The nine tables, complete

| filler | table | fragment range it covers |
|---|---|---|
| `mtx3071` | light, directional | `0x16b`–`0x182`, all 24 |
| `mtx1216` | light, point | `0x183`–`0x1a6`, all 36 |
| `mtx3514` | light, spot | `0x1a7`–`0x1d6`, all 48 |
| `mtx787` | texgen S/T/R/Q, 4 × 28 | `0x6f`–`0xa6` **except `0x71`** |
| `mtx2788` | `mtx2884` main, 675 entries | `0xe5`–`0x133` |
| `mtx544`, `mtx668` | `epi1_A`/`epi1_B`/`epi2_A`/`epi2_B` | `0x1d7`–`0x21e`, `0x21f`–`0x23e`, all 104 |
| `mtx1603`, `mtx2741`, `mtx544` | `mtx324`'s 27-entry table | `0x147`–`0x150` |

Two corrections to `parhelia-warp-codegen.md` §5.2 fall out:

- **The light-fragment table's `K[0x28]`/`K[0x30]`/`K[0x38]` dimensions really are
  populated.** A first pass that only resolved `lea rax,[rdi+imm]` bases found 18
  entries — exactly the `(K28,K30,K38) = (0,0,0)` corner, which is the a\*L set
  plus the two unnamed ones `0x19d` and `0x1b3`. Following the base-pointer
  arithmetic (`rax = engine + (L/4)*16`, `rdx = engine + (L/4)*64`, stores at
  `rax-0x1d40` = `engine + 0x104 + 4*L`) shows the loop fills the whole band.
- **`mtx2475` does not index the registry at all.** It returns
  `engine + 0x10 + 8*kind`, a program *descriptor* pointer, with a validity mask
  of `0x39fcf` plus a special case for kind 4. The canned programs
  (`0xc6`, `0xc7`, `0xca`, `0xcc`, `0xcd`–`0xdc`, `0x13f`–`0x144`) are therefore
  reached **as `.data` blobs by pointer, never as registry indices** — their
  registry slots are decorative. The codegen doc's "the remaining 409 come either
  from `mtx2475` or through one of the seven runtime tables" over-credited
  `mtx2475` with 29 slots.
- `0x4c` **is** reachable: `mtx1270` emits it at `.text+0xbf95e` under
  `!(K[0x05] & 1)`. It was previously counted but not attributed.

### 2.2 The 29, with a verdict for each

Every one was checked three ways: no `mov <reg32>, <index>` anywhere in the
module reaches an appender call site with that value; the value never appears in
any of the nine tables; and the value is not the literal `0x13a`, which appears
nowhere in `mtx_parhelia.o` at all.

| fragments | what they are | verdict |
|---|---|---|
| `0x58` `0x59` `0x5c` `0x5d` `0x60` `0x61` | The **depth-fog** halves of the three fog modes. Five of the six are exactly a live twin with the `op69` length instruction removed and the read of that result replaced by a direct read of `r8.z`/`r3.z` (LINEAR) or `r8.w`/`r3.w` (EXP, EXP2); `0x60` is an EXP variant reading a different fog constant component (§4.7). | **Not dead — the other axis of a 2×2 fog selector.** The live pairs compute the fog distance as a *radial* length; these take it from a single component. That is precisely D3D8's `D3DRS_RANGEFOGENABLE` (range vs depth fog), and the microcode library is shared with the Windows D3D driver — 30 blobs are byte-identical between `mtx_parhelia.o` and `MTXPARD.dll` (`parhelia-vertex-isa.md` §6). The Linux ICD never emits range-fog-off, so the selector that picks them is in `MTXPARD.dll`, not here. [INFERRED, strong — the structural pairing is CONFIRMED, the D3D attribution is the inference] |
| `0x71` | `op71 b3.xy ← r6` — the **combined S+T** form of the mode-6 texgen copy, where `0x6f` writes `b3.x` and `0x70` writes `b3.y`. | **Live code, unused optimisation.** `mtx787` fills the S table with `6f 6f 72 72` and the T table with `70 70 73 73`; a table that wrote both components in one instruction would use `0x71` and save an instruction per unit. Nothing indexes it. [CONFIRMED structure] |
| `0xb7` `0xb9` `0xba` | Clip-plane **accumulate** variants: `0xb7` is `op79 b5 ← r2·(0,0,0,0)` (a null plane), `0xb9`/`0xba` are `op79 b5 ← r1 · r6` with two different swizzles of `r6`. | **Alternative clip formulations.** The live chain `0xb0`–`0xb5` accumulates `r2 · r12…r17`; these accumulate against a register-held plane instead of the constant block. Reached by nothing in this module. [CONFIRMED shape, selector OPEN] |
| `0xb8` | Three `op78` writing `b6.x/.y/.z` from `r4`,`r5`,`r6` against `r4`, then a clear of `b1` — a 3×dp3, i.e. a **normal/vector transform** into `b6`. | Same family as the texture prologues `0x6b`–`0x6e`, which also build a vector in `b6`. Unselected variant. [INFERRED] |
| `0x6b` `0x6c` | Two- and four-instruction **texture-prologue** variants. Both end with `add.a b6 ← r17, …, r4`, the same last instruction as the live `0x6d`/`0x6e`. | Unselected variants of the same stage. `K[0x06] & 0x04` picks `0x6d` and `& 0x08` picks `0x6e`; bits 0 and 1 of `K[0x06]` have no reader, and are the obvious candidates for these two. [INFERRED] |
| `0x40` | `op89 b1, a1.zw ← r109` + `op75 b6 ← r1.wwww, r2` — the two-instruction form of `0x41`, which is the same body with one extra `op73 b1, a1.x ← r109, r7`. | Unselected short form of the live `0x41` (the `mtx2177` epilogue emitted under `K[4]&0x10`). [CONFIRMED by the shared tail] |
| `0x47` | `op72 b1, a64 ← r18` + `add.e b9 ← r7.66zw, r1.xyxy` — a two-instruction form whose last instruction is byte-identical to the last instruction of `0x45`, `0x46` and `0x48`. | Unselected short form of the `0x45`/`0x46`/`0x48` output-format tail. [CONFIRMED by the shared tail] |
| `0x49` | Four identical `add.e bN ← (0,0,0,0), r0.4455` writing `b3`, `b4`, `b5`, `b6`. | **Initialise the four texture-coordinate working registers.** Same destination set (`b3`…`b6`) that `mtx2177` patches per unit. Unselected. [INFERRED, strong] |
| `0xdd` `0xde` `0xdf` `0xe0` | `wra a0 ← r7` followed by `add.e b3/b4/b5/b6 ← r7` — a four-member family, one per texture unit, that emits texture coordinate `i` and stages it. `0xe1` is the same with `a7`. | Unselected per-unit output copies; the live path uses `0xa8`/`0xa9`/`0xaa`/`0xab` instead, which do the same job with the `src2 = i+7` patch. [CONFIRMED by the register progression] |
| `0xca` | Two `wra` instructions with `wmask0 = wmask1 = 0`. | **A two-instruction NOP filler**, the pair to the one-instruction `0xc9` that the alignment helpers use. Only `0xc9` is referenced. [CONFIRMED] |
| `0xbd` `0xe2` `0xe3` `0xe4` `0x145` `0x146` | Pure **control-class** fragments — `op0d` on `r9`/`r10`/`r11`, `op24`/`op25`/`op27`/`op2b`/`op34`/`op35`/`op38`/`op41`–`op44`/`op52`, branches and NOPs, and **no arithmetic instruction at all**. `0x145`/`0x146` sit immediately before `mtx324`'s 27-entry table. | **Loop/predicate scaffolding for the programmable path.** `0x153`/`0x154`/`0x155`–`0x158` (the live `mtx324` chain) have the same shape. Unselected prologue variants. The individual control opcodes are undecoded, so what they set up is [OPEN]. |
| `0x13a` | 30 instructions, a backward branch (`-20`), `op0a` writes to `a0.x/.z/.w`, `op01`/`op02`/`op05` on `r4`/`r8`/`r9`/`r11` — a **loop body** of the auxiliary band. | Dead in this build. `mtx2884` emits `0x131`, `0x132`, `0x134` and `0x135` (via `sete al; add eax,0x134`, so only those two), `0x136`–`0x139` and `0x13b`–`0x13e` as literals; `0x13a` is the single gap and the constant `0x13a` does not occur anywhere in the object. [CONFIRMED negative] |

**Summary of the answer to "dead, table, or D3D?":** six are the D3D
range-fog-off arm of a selector that lives in the Windows driver; twenty are
unselected *alternative encodings* of stages that are reached through some other
fragment (shorter forms, per-unit forms, combined-component forms, alternative
prologues) — live code with no caller in this build; and three (`0xbd`, `0x145`,
`0x146`, plus `0x13a`) are control scaffolding whose selector is either in the
Windows driver or genuinely gone. **None of them is reached by a table this
analysis missed.**

---

## 3. Family map — all 574 fragments

Every fragment is in exactly one family. `dead` counts the members of §2.2.

| fam | range(s) | n | dead | what the family computes |
|---|---|---|---|---|
| A | `0x001`–`0x005` | 5 | 0 | pre-transformed-vertex head: pass a screen-space position and colour straight to the output file |
| B | `0x006`–`0x00f`, `0x042`–`0x043`, `0x050`–`0x053` | 16 | 0 | **matrix transforms** — position, position+normal, 1…4 blend matrices, and the second-matrix 4×dp4 on `r105`–`r108` |
| C | `0x010`–`0x030` | 33 | 0 | lit T&L bodies: transform *plus* the per-vertex terms the light walk needs |
| D | `0x031`–`0x03f` | 15 | 0 | colour-material expansion: move one of `r5`/`r6`/`r84`/`r85`/`r86` into a per-term accumulator `b8`…`b11` |
| E | `0x040`–`0x041` | 2 | 1 | eye-space stage helper on `r109`, writing `a1` |
| F | `0x044`–`0x04f` | 12 | 2 | **output-format tails** — pack the finished vertex into the output registers the raster format wants |
| G | `0x054`–`0x057` | 4 | 0 | **colour source**: pass-through, or colour-material from `r84`/`r5`/`r6` |
| H | `0x058`–`0x064` | 13 | 6 | **fog** — 3 modes × {radial, depth} × {computed, explicit coordinate}, plus pass-through |
| I | `0x065`–`0x067` | 3 | 0 | per-item `.w`/q output write into `a68`,`a69`,`a70` from `r0.y`,`r0.z`,`r0.w` |
| J | `0x068`–`0x06a` | 3 | 0 | normalise/reciprocal helper (`op8a`, `op78` with `src0 == src1`) on `r3`, `r4`, `r0` |
| K | `0x06b`–`0x06e` | 4 | 2 | **texgen prologue** — build the eye/reflection vector in `b6` from `r0`, `r4`, `r6` and the constant `r17` |
| L | `0x06f`–`0x0a6` | 56 | 1 | **texgen / texture-matrix stage**, one instruction each (§4.6) |
| M | `0x0a7`–`0x0af` | 9 | 0 | texture-coordinate default fill and output packing |
| N | `0x0b0`–`0x0ba` | 11 | 4 | **user clip planes** — `n` accumulating dot products of `r2` against `r12…r17` into `b5` |
| O | `0x0bb`–`0x0bc` | 2 | 0 | light-walk init: seed `b0` from `r16`, clear `b10`/`b11` |
| P | `0x0bd`, `0x0e2`–`0x0e4`, `0x145`–`0x146` | 6 | 6 | control/loop scaffolding, no arithmetic |
| Q | `0x0be`–`0x0bf` | 2 | 0 | one dot product of `r0`/`r2` against the constant `r110` into `b2.w` |
| R | `0x0c0`–`0x0c3` | 4 | 0 | `mtx569` per-item body, 1…4 items, +21 instructions per item |
| S | `0x0c4`–`0x0c5` | 2 | 0 | **point size / point parameters** → `a25` |
| T | `0x0c6`–`0x0cc`, `0x0cd`–`0x0dc`, `0x13f`–`0x144` | 29 | 1 | complete **canned programs** reached by pointer, plus the NOP filler and the two terminators |
| U | `0x0dd`–`0x0e1` | 5 | 5 | per-unit texture-coordinate output copy (dead variant of M) |
| V | `0x0e5`–`0x13e`, `0x147`–`0x152`, `0x159`–`0x16a` | 120 | 1 | **auxiliary / programmable-path bodies** (§5) |
| W | `0x153`–`0x158` | 6 | 0 | `mtx324`'s `GL_EXT_vertex_shader` chain |
| X | `0x16b`–`0x1d6` | 108 | 0 | **lighting** — directional 24, point 36, spot 48 |
| Y | `0x1d7`–`0x21e` | 72 | 0 | **material colour source**, 12 variants × 2 faces × 3 sources |
| Z | `0x21f`–`0x23e` | 32 | 0 | **material accumulate** words, 1–2 instructions each |

419 fragments (A–S, X, Y, Z) belong to the fixed-function path and are described
per-family in §4. 120 (V) plus 6 (W) belong to the auxiliary/programmable path
and are §5. 29 (T) are whole programs, not fragments.

---

## 4. The fixed-function families in detail

Notation: `bN`/`rN` are the same register (§1.1); `aN` is the port-A output file.
Where a fragment writes both ports in one instruction it is written
`bN , aM ← …`.

### 4.1 B — matrix transform (16 fragments)

The library idiom is four `op77` instructions:

```
op77  b3.w , a259  ←  src0 = r0 (the vector) , src1 = r3 (matrix row 3)
op77  b3.z , a260  ←  src0 = r0              , src1 = r2
op77  b3.y , a261  ←  src0 = r0              , src1 = r1
op77  b3.x , a262  ←  src0 = r0              , src1 = r0
```

`src0` is the vector and `src1` is the matrix row. That assignment is
**[CONFIRMED] by a controlled pair**: `0x42` and `0x43` are byte-identical except
for `src0` (`r3` vs `r0`), both sweeping `src1` over `r105`…`r108`, i.e. the same
matrix applied to two different vectors. The same shape with `src1 = r89`…`r92`
is the texgen stage (§4.6), and with `src1 = r12`…`r17` the clip-plane stage
(§4.5).

`0x50` prefixes the quad with one `op71 ← r0` that engages neither write port;
`0x0f` is the bare quad. `0x51`/`0x52`/`0x53` repeat the quad 2/3/4 times into
`b5`,`b6`,`b9` with interleaved `op75`/`add.a` weighting instructions reading
`r4`,`r5`,`r6` — vertex blending. `0x06`–`0x09` add a 3-instruction `op78` block
whose `src0` sweeps `r4`,`r5`,`r6` — the normal transform. The fragment lengths
(5/11/16/21 for position only; 8/20/29/38 for position+normal) confirm one extra
matrix per mode step.

**The stored `src1 = r0…r3` collides with `src0 = r0` and is not explained.**
See §7.1 — this is the single most important open item for a rewrite.

### 4.2 G — colour source (4 fragments)

| frag | when | discriminating operand |
|---|---|---|
| `0x54` | `GL_LIGHTING` off — colour pass-through | — |
| `0x55` | lighting on, `K[0x28] = 0` | `src1 = r84` (material constant) |
| `0x56` | lighting on, `K[0x28] = 1` | `src1 = r5` (primary colour) |
| `0x57` | lighting on, `K[0x28] = 2` | `src1 = r6` (secondary colour) |

`0x55`/`0x56`/`0x57` are byte-identical apart from `src0`, `src1` and `src2`;
`src1` is the one that tracks the colour-material source, and it matches the
`0x1d7`/`0x1d8`/`0x1d9` triple exactly. All four write `a2` and `a11`, and `0x54`
additionally `a65`.

### 4.3 Y — material colour source (72 fragments)

`epi1_A` / `epi2_A` index this band as `e + 2*(3*s + K[0x28])` with `s` = 0…7,
which is 48 slots each; the band is the union. The structure recovered from
dataflow is exactly a 12 × 2 × 3 product:

- **12 variants** (`s` and `e` of the codegen doc's `epi*_A`), distinguished by
  the opcode pair and which accumulator they touch:
  `op84 + op86` (slots 0,2,4 of each group), `op71 ×2`, `op71 + wra`,
  `op75 ×2`, `op75 + op76 + wra`.
- **2 faces**: front writes `a1`/`a2` and `b1`/`b2`; back writes `a7`/`a8` and
  `b7`/`b8`. The two are otherwise identical.
- **3 colour sources** within each face-triple, and the *only* difference between
  the three members is one register field:

```
0x1d7  op84  b1,a1.w ← src2 = r84    (front material constant)
0x1d8  op84  b1,a1.w ← src2 = r5     (primary colour)
0x1d9  op84  b1,a1.w ← src2 = r6     (secondary colour)
0x1da  op84  b7,a7.w ← src2 = r118   (back material constant)
0x1db  op84  b7,a7.w ← src2 = r5
0x1dc  op84  b7,a7.w ← src2 = r6
```

This is what fixes `r84` = front material constant and `r118` = back material
constant [CONFIRMED]. The `s ≥ 6` variants (`0x207`–`0x21e`) additionally read
`r110`; `s ≥ 2` variants read `r10` and `r11`. What the 12 variants correspond to
in GL is [OPEN] — the codegen doc has `s` bit 1 = `G[0x10]&1` and bit 2 =
`K[0x05]&0x10`, both undecoded.

### 4.4 Z — material accumulate (32 fragments)

One or two instructions. The table is a clean 4 × 4 × 2:

```
epi1_B[r12 + 4*ebx]         front            epi2_B[r12 + 4*ebx]   back
  ebx=0: 221 222 223 239                       224 225 226 23c
  ebx=1: 227 228 229 23a                       22a 22b 22c 23d
  ebx=2: 22d 22e 22f 23b                       230 231 232 23e
  ebx=3: 233 234 235 21f                       236 237 238 220
```

Reading the registers off each fragment:

| `r12` column | front term added | back term added |
|---|---|---|
| 0 | `r87` | `r121` |
| 1 | `r5` (primary colour) | `r5` |
| 2 | `r6` (secondary colour) | `r6` |
| 3 | nothing (the `ebx=3` column is `0x21f`/`0x220`, a bare accumulator move) | nothing |

and `ebx` selects whether the face-independent constant `r88` is folded in
(`ebx = 0`: yes, two instructions; `ebx = 3`: no, one instruction) and through
which opcode (`0x71` "add.e" vs `0x76` "add.a" — the two `Add` forms the vendor
symbols name). The five surviving vendor names sit exactly where this predicts:
`AddEfm` = `0x233` (`r87`), `AddEfd` = `0x234` (`r5`), `AddEfs` = `0x235` (`r6`),
`AddAfd` = `0x23a` (`r5`+`r88`), `AddAfs` = `0x23b` (`r6`+`r88`). [CONFIRMED]

`r121` is therefore the back-face counterpart of `r87`, and `r88` is
face-independent [CONFIRMED — `0x239` (front) and `0x23c` (back) both read it].

### 4.5 N — user clip planes (11 fragments)

`0xb5`, the six-plane form, in full:

```
op7a  b5.xyzw ← r2 , r12      ; first plane: initialise the accumulator
op79  b5.xyzw ← r2 , r13      ; five accumulating dot products
op79  b5.xyzw ← r2 , r14
op79  b5.xyzw ← r2 , r15
op79  b5.xyzw ← r2 , r16
op79  b5.xyzw ← r2 , r17
```

`0xb0`…`0xb4` are the 1…5-plane prefixes; `0xb6` (`op6d b10.x ← r5`) is the
epilogue emitted for every non-zero plane count. `op7a` versus `op79` on the
*first* instruction is the "start" versus "accumulate" distinction [CONFIRMED by
the pattern]. `r2` is the transformed position (§1.2), which independently
confirms the `dst1 = 2` patch reading. Six planes matches the GL minimum and the
kernel's six-entry clip-plane array at `HW+0x1da0`.

`0xb7`, `0xb9`, `0xba` are the unselected variants of §2.2.

### 4.6 L — texgen / texture-matrix stage (56 fragments)

Each fragment is exactly **one instruction**. The 56 decompose as
`{source vector} × {S,T,R,Q} × {which component of b3 to write}`:

| texgen `mode` (from `mtx2177`'s per-unit block byte `0x0e`/`0x0f` nibbles) | source vector | fragments (S, T, R, Q) | shape |
|---|---|---|---|
| 0 | `r(7+i)` — the **raw texture coordinate** of unit `i`, installed by the `P(0x0c, i+7, 0)` patch | `0x91`–`0x94`, `.w` forms `0x95`–`0x97`, copies `0x98`–`0x9a` | dp4 vs `r89+stage` |
| 1 | `r0` | `0x8a`–`0x8d`, `.w` `0x8e`–`0x90`, copies `0x9f`–`0xa2` | dp4 vs `r89+stage` |
| 2 | `r3` | `0x7c`–`0x7f`, `.w` `0x80`–`0x82`, copies `0x9b`–`0x9e` | dp4 vs `r89+stage` |
| 3 | `r6` | `0x83`–`0x86`, `.w` `0x87`–`0x89`, copies `0xa3`–`0xa6` | dp4 vs `r89+stage` |
| 4 | `r0` | `0x74`,`0x75`,`0x76`,`0x77` | dp4 vs `r89`,`r90`,`r91`,`r92` |
| 5 | `r3` | `0x78`,`0x79`,`0x7a`,`0x7b` | dp4 vs `r89`,`r90`,`r91`,`r92` |
| 6 | `r6` | `0x6f`,`0x70` (copy) and `0x72`,`0x73` (dp4) — **S and T only** | |

The four 28-entry tables are indexed `k + 2*hasTg + 4*mode`, so each mode row has
four columns `(hasTg,k) = (0,0) (0,1) (1,0) (1,1)`:

- `(0,0)` is `0` for modes 0–3 — **no fragment is emitted**; the raw coordinate
  register is used as-is.
- `(0,1)`, the "this stage is the last declared component" case, emits a plain
  **copy** (`op71 b3.xyzw ← source`) — the texture matrix is off, so nothing but
  a move is needed.
- `(1,*)` emits the **dp4** against `r89 + 4*i + stage`.
- Modes 4 and 5 are viewer- and component-count-invariant (one fragment fills all
  four columns) — object-linear and eye-linear texgen, using the object position
  `r0` and the eye vector `r3` respectively [INFERRED, strong: those are exactly
  the two vectors `GL_OBJECT_LINEAR` and `GL_EYE_LINEAR` need, and the
  "matrix rows" they dot against are then the S/T/R/Q plane equations].
- Mode 6 exists only for S and T — **sphere map** [INFERRED, and it reads `r6`,
  the vector the texgen prologue built].
- Modes 0–3 differ only in their source vector: raw coordinate, object position,
  eye position, prologue vector. Which GL enum each is remains [OPEN], but the
  *source register* per mode is [CONFIRMED] from the fragment bodies.

Destination is always `b3`, patched per unit to `b(3+i)` or `b(6+i)`.

### 4.7 H — fog (13 fragments)

The band is a 3 × 2 × 2 plus a pass-through, and the two axes the generator uses
are only two of the three:

| GL fog mode | radial, computed | radial, explicit coord | depth variants |
|---|---|---|---|
| **LINEAR** (`mode = 3`) | `0x5a` | `0x5b` | `0x58` (`r8.z`), `0x59` (`r3.z`) |
| **EXP** (`mode = 1`) | `0x5e` | `0x5f` | `0x5c` (`r8.w`), `0x5d` (`r3.w`) |
| **EXP2** (`mode = 2`) | `0x62` | `0x63` | `0x61` (`r3.w`) |
| pass-through (`mode = 0`) | `0x64` | `0x64` | — |

`0x60` does not fit the grid: it is three instructions with the EXP shape but
reads `r116.w` where `0x5c` reads `r116.z`, and takes its distance from `r8.w`.
It is an EXP variant with a different fog parameter component [CONFIRMED shape,
role OPEN]. Every other depth variant is exactly its radial twin with the
`op69` length instruction removed and the read of that result replaced by a
register component.

The mode identification is from instruction count and shape [CONFIRMED]:

```
0x5a  LINEAR : op69 b1 ← r3.xyzw          ; |eye position|
               op75 b1.z ← r116.z , r1.z  ; * scale
               op73 b1.z ← r116.z , r1.y  ; + bias
               wra  a2.w ← r0.z           ; emit the fog factor
0x5e  EXP    : op69 ; op75 ; op91 ; add.e b2.w        ; one exponential
0x62  EXP2   : op69 ; op75 ; op75 ; op91 ; add.e b2.w ; distance squared first
0x64         : wra a2.w                                ; pass the coordinate through
```

`flag != 0` replaces `op69 b1 ← r3.xyzw` with `op69 b1.z ← r11.xxxx`, i.e. **the
fog coordinate comes from `r11.x` instead of being computed from the eye
position** — the `GL_FOG_COORD` / `D3DFVF_FOG` case. That confirms the codegen
doc's guess that the second column is "an explicit fog coordinate" and assigns
it a register.

The fog factor leaves the LINEAR and pass-through forms in **`a2.w`**; the EXP
and EXP2 forms leave it in `b2.w` (= `r2.w`) and additionally write `b6` through
`op91`. What consumes `b2.w` afterwards is [OPEN].

### 4.8 F — output-format tails (12 fragments)

The most heterogeneous fixed-function family, and the one a rewrite should
implement last. Every member ends the per-vertex work by moving finished values
into the output file. Complete dataflow:

| frag | instr | reads | writes A | writes B |
|---|---|---|---|---|
| `0x44` | 1 | `r4` | `a8.w` | `b8.zw` |
| `0x45` | 7 | `r1`,`r7`,`r8`,`r18`,`r19` | `a64` | `b1`,`b8`,`b9` |
| `0x46` | 7 | `r1`,`r4`,`r6`,`r7`,`r8`,`r19` | `a64` | `b1`,`b6`,`b9` |
| `0x47` | 2 | `r1`,`r7`,`r18` | `a64` | `b1`,`b9` |
| `0x48` | 5 | `r1`,`r4`,`r7`,`r19` | `a0` | `b1`,`b9` |
| `0x49` | 4 | `r0` | — | `b3`,`b4`,`b5`,`b6` |
| `0x4a` | 6 | `r0`,`r1`,`r7`,`r18`,`r19` | `a0`,`a1.xy` | `b1`,`b7`,`b9` |
| `0x4b` | 5 | `r0`,`r1`,`r7`,`r18`,`r19` | `a0`,`a1.xy`,`a65.zw` | `b1`,`b7`,`b9` |
| `0x4c` | 1 | `r0`,`r3` | — | `b2` |
| `0x4d` | 3 | `r0`,`r1`,`r5`,`r8` | `a85.w` | `b0`,`b1` |
| `0x4e` | 3 | `r0`,`r1`,`r5`,`r8` | `a87.w` | `b1`,`b7` |
| `0x4f` | 1 | `r7` | — | `b0` |

`0x4d` and `0x4e` are the same three instructions differing only in the last
one's destination pair (`b0`,`a85` vs `b7`,`a87`) — the choice of which output
slot the tail writes. `0x45`/`0x46`/`0x48` share their final instruction with the
dead `0x47`. The GL meaning of the `K[0x08] & 0x1c` class that selects among them
is [OPEN] and is the thing to sweep for first (§6.3).

### 4.9 X — lighting (108 fragments)

Structure is fully covered by `parhelia-warp-codegen.md` §1.5; what dataflow
adds:

- The three sub-bands (directional `0x16b`–`0x182`, point `0x183`–`0x1a6`, spot
  `0x1a7`–`0x1d6`) read `r0`…`r13` and `r84`,`r85`,`r86`, and write `a0`,`a1`,
  `a4`–`a9` and `b0`,`b1`,`b4`–`b11`.
- **Every block ends with `op81`**, an opcode that writes neither port anywhere
  in the corpus (194 occurrences, 0 port-A and 0 port-B writes) yet always reads
  two operands: a commit / accumulate-into-latch step [INFERRED, unchanged from
  `parhelia-vertex-isa.md` §13].
- The registers `r0`…`r6` in a *stored* lighting fragment are the light-0
  constant block; the relocator rewrites them to `idx + 7*slot`, and `mtx2755`
  starts its walk at `slot = 1`, so the first walked light's block is `r7`…`r13`.
  Reading a lighting fragment as if `r5` were the primary colour is therefore
  wrong — the colour-material identification of `r5`/`r6` comes from families
  G, Y and Z, which carry **no relocations**.
- Opcode census per light body: `0x78` dominates (870 of 2 614 arithmetic
  instructions in the band), then `0x75` (546), `0x73` (270), `0x71` (250),
  `0x81` (180).

### 4.10 The small helper families

| fam | detail |
|---|---|
| A `0x01` | `add.e b1 ← r0.ww44, r8` ; `op73 b0` ; `add.e b0, a65 ← r5` ; `op73 b7, a82 ← r8` ; then four `wra a3,a4,a5,a6 ← r7,r8,r9,r10` — a complete pre-transformed-vertex head that copies the four texture coordinates straight out |
| A `0x02` | `wra a11 ← (0,0,0,1)` — the homogeneous default position |
| A `0x05` | `wra a1 ← r2` |
| D `0x31`–`0x3b` | one instruction each: `op71`/`op75 b8`/`b9`/`b10 ← rX , {r84, r85, r86, r5, r6, r8, r9}` — the three material terms A/B/C, each with the same three-way colour-material choice (`0x33`/`0x34`/`0x35` → `b8`, `0x36`/`0x37`/`0x38` → `b9`, `0x39`/`0x3a`/`0x3b` → `b10`). `0x3c`–`0x3f` are the two-way tail into `b10`/`b11` |
| I `0x65`–`0x67` | `wra a68.w ← r0.y`, `a69.w ← r0.z`, `a70.w ← r0.w`, all with `src2 = r21` — three `.w` outputs from three components of `r0`. The `0x65+i` loop bound is `G[0x2a0]->u32[0x08]`, the texture unit count, so at most three of the six band members are reachable that way and `0x68`/`0x69`/`0x6a` belong to family J instead |
| J `0x68`–`0x6a` | `op78 b4, a4.xyz ← r3,r3,r3` / `op8a b4.xyz ← r4,r4` / `op8a b0 ← r0,r0` — the `src0 == src1` shape of a normalise or reciprocal-length |
| M `0xa7` | write `(0,0,0,1)` to `a3`, masked to the missing components (§1.5) |
| M `0xa8`–`0xab` | `wra aN ← src2 = r(7+i)` — emit texture coordinate `i` (the `P(0x06, i+7, 0)` and `P(0x01, i+3, 0)` patches) |
| M `0xac`,`0xad` | write three consecutive outputs' single components from `r0.w` / `r0.x,r0.y,r0.z` |
| M `0xae`,`0xaf` | 2 and 9 instructions building a normalised vector into `b6`…`b9` and emitting `a1`,`a6` — the projective/normalised texture-coordinate epilogue |
| O `0xbb` | `add.e b0 ← r16` — seed the light accumulator from a constant |
| O `0xbc` | clear `b10` and `b11` |
| Q `0xbe`,`0xbf` | `op77 b2.w ← r0`/`r2` against the constant `r110`, with `0xbf`'s `swzB` patched to `0xf40` by `mtx2179` step 11 |
| S `0xc4`,`0xc5` | 15 and 13 instructions: `op73`, three `op75`, `op8b`, `op62`, reading `r6`,`r7`,`r8`,`r9` and the constant `r24`, writing `a25` — the point-size attenuation polynomial `1/sqrt(a + b·d + c·d²)` shape [INFERRED]. `0xc4` has one extra `op78` triple; `K[0x48] == 3` picks it |
| R `0xc0`–`0xc3` | 63/84/105/126 instructions, exactly +21 per item, reading `r0`…`r13`, `r28`, `r44` and writing a wide spread of outputs — `mtx569`'s per-item body, index `S[0x80] + 0xbf` |

---

## 5. V and W — the auxiliary / programmable band (126 fragments)

`0x0e5`–`0x13e`, `0x147`–`0x152`, `0x159`–`0x16a` (family V, 120) and
`0x153`–`0x158` (family W, 6) are reached only through `mtx2884`'s 675-entry
5-D table, `mtx324`'s 27-entry table, and their literals. They are 2…562
instructions long, none contains an `end`, and **none of them is fixed-function
T&L**:

| test (all 120 members of V) | result |
|---|---|
| contains a 4×`op77` dp4 quad | **0 of 120** |
| reads a texture-matrix register (`r89`–`r108`) | **0 of 120** |
| reads a fog register (`r116`/`r117`) | **0 of 120** |
| reads a material register (`r84`–`r88`, `r110`, `r118`, `r121`) | **0 of 120** |
| contains a clip accumulate (`op79`/`op7a`) | **0 of 120** |
| contains a branch | **111 of 120** |

Their register reads stop at `r61` (`r0`–`r16`, `r18`, `r20`–`r22`, `r24`,
`r28`–`r29`, `r44`–`r45`, `r60`–`r61`), so their constants are
`r15`,`r16`,`r18`,`r20`,`r21`,`r22`,`r24`,`r28`,`r29`,`r44`,`r45`,`r60`,`r61` —
a constant allocation disjoint from the fixed-function path. Their opcode census is dominated by the
control class (`0x41` 1977, `0x23` 1617, `0x01` 1186, `0x35`, `0x34`) alongside
generic ALU (`0x71`, `0x76`, `0x73`, `0x60`, `0x0a`), and the small members are
*pure* control:

```
0x112 : op41 +0x28000102 ; op35 ; op0d r11←r0 ; op0d r9←r0 ; op2b ; nop×3 ; op27
0x130 : nop ; op41 +768 ; br +4 ; op41 +0 ; nop ; op41 +1024 ; op41 +4
```

**Reading [INFERRED]:** this is the pre-compiled body library for the *other*
WARP stage — the one `mtx1881` binds through `mtx2884` variant 6 at `S[0x580]`/
`S[0x590]` and emits to `0x1100`/`0x10f8` (`parhelia-warp-binding.md` §3 step 7),
plus `mtx324`'s `GL_EXT_vertex_shader` chain. The large 32-bit immediates and
loop structure are consistent with vertex-attribute fetch/decode and with a
D3D `vs_1_1` body library; the absence of any T&L idiom rules out both a
transform and a lighting role.

**Per-fragment semantics of this band is [OPEN] and cannot be recovered from
dataflow**, because the discriminating information sits in the control opcodes
`0x01`, `0x02`, `0x05`, `0x0d`, `0x24`, `0x25`, `0x27`, `0x2b`, `0x33`–`0x35`,
`0x38`, `0x41`–`0x44`, `0x52`, none of which is decoded. The important practical
point is that **the fixed-function path never selects any of them**, so a
rewrite that targets fixed-function GL can ignore all 126.

---

## 6. Rewrite plan

The bring-up order is the additive one `parhelia-warp-codegen.md` §6.3
recommends. Steps 1–3 need **no relocations at all** — `mtx2921`, `mtx2177`,
`mtx1270` and `mtx2947` between them make 268 field patches and zero relocation
appends; relocation is used only by `mtx2755`, i.e. only by lighting.

What follows is stated purely in the register convention of §1. No vendor
instruction bits appear, and nothing below requires copying a fragment.

### 6.0 Contract with the rest of the pipeline

Before any of the three steps, the driver must have:

- the constant store loaded so that the model-view-projection matrix rows are in
  four consecutive `r*` slots, the texture-matrix/plane rows for unit `i` in
  `r(89+4i)`…`r(92+4i)`, and the fog parameters in `r116`;
- the input assembler bound so that `r0` = object position, `r5` = primary
  colour, `r6` = secondary colour, `r(7+i)` = texture coordinate of unit `i`,
  and `r11` = the fog coordinate if one is declared;
- the setup unit programmed with the viewport rectangle, scissor and depth
  range — the vertex program does **not** apply them
  (`parhelia-vertex-pipeline.md` §7);
- the program terminated by an `end` instruction; the linker adds nothing
  (`parhelia-warp-codegen.md` §3.4).

### 6.1 Step 1 — transform and colour

Compute, in this order:

```
  P = the clip-space position                     ; P.c = dot4(r0, MVP_row_c)
  emit P.x -> a262 , P.y -> a261 , P.z -> a260 , P.w -> a259
  keep  P in a temporary; call it r2               ; the clip stage below reads it
  emit the vertex colour -> a65
      colour = r5                                  if GL_LIGHTING is off
             = r84 / r5 / r6                       if colour-material selects
                                                   constant / primary / secondary
  emit r2 -> a11
  end
```

Contracts, all [CONFIRMED] unless noted:

- the destination for the four position components is `a259`…`a262`, `a259` = w
  through `a262` = x;
- the temporary the position is kept in must be `r2` if the clip-plane stage of
  step 6 is to be reused as written — the vendor generator patches its transform
  to write `b2` for exactly that reason, and its clip fragments read `r2`;
- `a65` is the colour output and `a11` receives the position a second time
  [INFERRED for `a11`];
- an output-format tail is also required, and this is the part §4.8 cannot fully
  specify — the vendor emits `0x4d` (writing `a85.w`) for the plainest state. A
  first implementation should reproduce that one instruction's *effect* and then
  sweep `K[0x08] & 0x1c` on hardware to learn the rest.

Size: the vendor's equivalent recipe is 13 instructions
(`parhelia-warp-codegen.md` §4.3), so this fits any budget.

### 6.2 Step 2 — texture coordinates

Per texture unit `i` in 0…3, with `ncomp` the declared component count and
`stage` running S, T, R, Q:

```
  T = a working register, r(3+i)                   ; the vendor's allocation;
                                                   ; a rewrite should allocate
                                                   ; its own and avoid r5/r6
  if ncomp < 4:
      T.<components ncomp..3> = (0,0,0,1).<same>   ; the GL default
  for stage in 0..ncomp-1:
      V = the source vector for this unit's texgen mode:
            no texgen            -> r(7+i)          ; the raw coordinate
            object linear        -> r0
            eye linear           -> the eye-space position
            sphere map           -> the prologue vector (S and T only)
      if the texture matrix is enabled:
            T.<stage> = dot4(V, r(89 + 4*i + stage))
      else:
            T.<stage> = V.<stage>                   ; plain move
  emit T -> a(3+i)
```

Contracts [CONFIRMED]: the input register is `r(7+i)`, the matrix/plane rows are
`r(89+4i)`…`r(92+4i)`, the output register is `a(3+i)`, and the default fill is
`(0,0,0,1)` masked to the components above `ncomp`. `mtx2177` emits **no
relocations**, only field patches, so a generator can specialise a single
template by rewriting operand indices — or, better, emit the instruction
directly.

For eye-linear and sphere-map the eye-space position and the reflection vector
must exist first; that is what family K computes, into what the library calls
`b6`. Its exact formula is [OPEN] (§7.2) — a first implementation should support
"no texgen" and "object linear", which need only `r(7+i)` and `r0`.

### 6.3 Step 3 — fog

```
  if an explicit fog coordinate is declared:
      d = r11.x
  else:
      d = length(eye-space position)               ; radial fog, what GL does
        or |eye-space position.z|                  ; depth fog (the D3D variant)

  f = d * r116.<scale>  + r116.<bias>              ; GL_LINEAR
      exp(-(d * r116.<density>))                   ; GL_EXP
      exp(-(d * r116.<density>)^2)                 ; GL_EXP2
      d                                            ; pass-through

  emit f -> a2.w
```

Contracts: the fog parameter block is `r116` [CONFIRMED family]; the explicit
coordinate is `r11.x` [INFERRED, strong]; the destination for the LINEAR and
pass-through forms is `a2.w` [CONFIRMED]. Which components of `r116` are scale,
bias and density is [OPEN] — the vendor uses `r116.z` and `r116.w`, so the store
has at most two live components. The EXP/EXP2 forms leave the result in a
temporary rather than in `a2.w`, and what picks it up is [OPEN]; a rewrite should
just write `a2.w` in all four cases and check on hardware.

### 6.4 Beyond step 3

- **Lighting** needs the relocator and the 7-register-per-light constant block;
  the per-light body is `op73`/`op78`/`op75`/`op60` ending in `op81`, and the
  epilogue is the (colour source, `r88` fold) product of §4.4. Always emitting
  the one-light form and looping is a legitimate simplification — the two-light
  fragments are an optimisation, not a requirement.
- **Two-sided lighting** is a second whole pass with `r118`/`r121` in place of
  `r84`/`r87` and the `a7`/`b7`,`b8` accumulators in place of `a1`/`b1`,`b2`.
- **User clip planes** are `n` accumulating dot products of `r2` against
  `r12`…`r17`, first one initialising and the rest accumulating, into a single
  temporary, plus an epilogue.

### 6.5 What a rewrite does not have to do

- It does not need any of the 126 auxiliary/programmable fragments (§5).
- It does not need the 29 canned-program slots — those are whole programs the
  vendor binds by pointer, and a from-scratch driver generates its own.
- It does not need the 32-instruction alignment padding: only `mtx324` and
  `mtx2884` use it, and `mtx2179` never pads
  (`parhelia-warp-codegen.md` §3.3).
- It does not need `parhelia-warpreg.py` at run time. The tool stays useful for
  one thing: diffing a freshly written fragment against the validated vendor
  corpus to see whether the encoding is right.

---

## 7. What could not be determined

Listed rather than invented.

### 7.1 The transform fragments' `src1` collides with `src0` — the top open item

In `0x0f`/`0x50` the vector is `src0 = r0` while `src1` sweeps `r3, r2, r1, r0`,
so the *stored* form names `r0` both as the vertex position and as matrix row 0.
`0x51`/`0x52`/`0x53` repeat the same sweep for every blend matrix, so all four
matrices would occupy `r0`…`r3` as well. The linker applies **only `dst1`
patches** to these fragments (`parhelia-warp-codegen.md` §4.2), so nothing
rewrites the operands.

Three readings survive, and dataflow cannot choose between them:

1. `src0` and `src1` address different physical files (a vertex file and a
   constant file), so `r0` in `src0` and `r0` in `src1` are different registers.
   *Against:* the port-B def-use enrichment appears in **all three** slots
   (`r1` 2.6× / 2.3× / 1.7×), which a split file would not produce.
2. One of the two undecoded adjacent fields — `f1c` (w1[8:7], next to `src0`) or
   `f1a`/`f1i` — is a per-operand bank or base. `f1c = 0` implies `src0 == r0` in
   5 031 of 5 189 arithmetic instructions (97 %), which is exactly the profile of
   a field that belongs to `src0`.
3. The dp4 opcode `0x77` addresses the matrix implicitly and `src1` is a row
   *offset* rather than a register index.

**The experiment that settles it** is the one `parhelia-vertex-isa.md` §11
already describes: load distinct known values into `r0` and `r3`, upload a
one-instruction `op77`, and read the result back through
`0x910`/`0x914`/`0x938` kind 3. No rasteriser or display output is needed.

Until it is settled, a rewrite should **not** copy the vendor's register
assignment for the transform. Put the matrix somewhere unambiguous — the top of
the constant file, e.g. `r120`…`r123` — and confirm on hardware.

### 7.2 Other undetermined items

| item | why it is open |
|---|---|
| what `src2` is for | it is `r0` in 64 % of arithmetic instructions and inert in all five named `Add*` words, yet in `0x1d7`/`0x1d8`/`0x1d9` it is *the* discriminating operand, and in the transform quads it takes the values 7,8,9,10 in lock-step with the port-A writes. It is a real operand for some opcodes and something else for others |
| `f0a` as a port-A enable | holds for 95 % of arithmetic instructions but the 681 exceptions include the position dp4s of `0x50`, which carry `f0a = 2` *and* a full `wmask0` on `a259`…`a262`. Either those output writes are predicated, or `f0a` is not an enable |
| `f1c` as a port-B enable | **does not hold**, and this corrects `parhelia-vertex-isa.md` §14. Among arithmetic instructions only 54 % of `f1c = 0` cases have `wmask1 = 0`; `0x72` and `0x74` are byte-identical except for `src0` and `f1c`, and `0x74` is a live texgen fragment that must write `b3`. `f1c` behaves like a `src0` attribute. The earlier 21781/24419 correlation is an artefact of counting control-class instructions, where every field is zero |
| swizzle codes 6 and 7 | codes 4 and 5 are settled (§1.5); 6 and 7 occur in `r1.6666`, `r0.7777`, `r6.xy64`, `r7.66zw` and have no forced reading |
| ALU semantics of the individual opcodes | only `end` (0x29), `nop` (0x00), the two branches (0x23, 0x46) and `Add` (0x71/0x76) are named. `0x77` is a dot product by shape; `0x69`, `0x8a`, `0x78` with `src0 == src1` are length/reciprocal-shaped; `0x81` writes nothing and is a commit. The rest stay `opNN` |
| the 12 `s`-variants of the material band | indexed by `s` = `(term present) | (!(G[0x10]&1) << 1) | (!(K[0x05]&0x10) << 2)`; the GL meaning of `G[0x10] bit 0` and `K[0x05] bit 4` is undecoded |
| the `0x65+i` loop's GL meaning | three fragments writing `a68.w`, `a69.w`, `a70.w` from `r0.y/z/w`, guarded by `G[0x2a0]->u8[0x5c] & 0x04` and counted by the texture unit count |
| which output-format tail is which raster format | the selector is `K[0x08] & 0x1c` and `K[0x04]` bits 3/4/6, none decoded |
| everything in family V | §5 — 126 fragments, [OPEN] per fragment, and not needed for fixed function |
| the exact component roles inside `r116` | only `.z` and `.w` are read |
| what consumes `b2.w` after EXP/EXP2 fog | §4.7 |

### 7.3 Fragments whose behaviour is flagged as undetermined

Being explicit, as asked. The following are the fragments this pass will **not**
put a semantic label on:

- all 120 of family V (`0x0e5`–`0x13e`, `0x147`–`0x152`, `0x159`–`0x16a`) and
  the 6 of family W (`0x153`–`0x158`) — structure characterised in §5, function
  per fragment unknown;
- the 6 control-only fragments of family P (`0x0bd`, `0x0e2`–`0x0e4`,
  `0x145`–`0x146`) — they contain no arithmetic, so there is nothing to read;
- family Q (`0x0be`, `0x0bf`) — one dot product against `r110`, but what `r110`
  holds is unknown;
- family E (`0x40`, `0x41`) — a stage helper on `r109`, register unidentified;
- `0x44` (family F), one instruction on `r4` writing `a8.w`;
- the 12-way `s` split inside family Y, and the four-way `ebx` split inside
  family Z beyond the `r88`-fold reading of §4.4;
- `r18`, `r19`, `r20`, `r21`, `r22`, `r24`, `r28`, `r29`, `r30`, `r44`, `r45`,
  `r60`, `r61`, `r109`, `r110`, `r117` — read, never written, contents unknown.

---

## 8. Corrections this file makes

1. **`parhelia-vertex-isa.md` §3: swizzle code 4 is 0.0, not 1.0**, and code 5 is
   1.0. Forced by `0xa7`'s four `wmask0` patch values against the GL default
   texture coordinate (§1.5).
2. **`parhelia-vertex-isa.md` §14: `f1c` is not a port-B enable.** The
   correlation that suggested it is an artefact of including control-class
   instructions; among arithmetic instructions it is 54 %, and a live texgen
   fragment (`0x74`) has `f1c = 0` with a real port-B write. `f1c` tracks `src0`
   instead (97 % of `f1c = 0` instructions have `src0 = r0`).
3. **`parhelia-vertex-isa.md` §14: `f0a = 2` is not reliably "port A idle"
   either** — the 681 exceptions include the position output writes.
4. **`parhelia-warp-codegen.md` §2.4: `bN` and `rN` are the same register.** The
   register table there lists `a*`/`b*` as if they were two output files; port B
   writes into the low 16 of the source file and port A into a separate 512-entry
   output file (§1.1).
5. **`parhelia-warp-codegen.md` §5.2: sixteen unreachable fragments is
   twenty-nine**, and `mtx2475` never indexes the registry, so the 29 canned
   slots it was credited with are reached by pointer only (§2).
6. **`parhelia-warp-codegen.md` §5.2: `0x4c` is reachable** — `mtx1270`
   `.text+0xbf95e`, under `!(K[0x05] & 1)`.
7. **`parhelia-warp-codegen.md` §1.6: the fog table's missing halves are the
   depth-fog arm**, not an unknown second selector, and `flag != 0` means "take
   the fog coordinate from `r11.x`" (§4.7). The three GL fog modes are also now
   assigned: `mode = 3` LINEAR, `mode = 1` EXP, `mode = 2` EXP2, `mode = 0`
   pass-through.
8. **`parhelia-warp-codegen.md` §1.7: modes 4 and 5 use `r0` and `r3`** as their
   source vector and mode 6 uses `r6`; modes 0–3 use `r(7+i)`, `r0`, `r3` and
   `r6` respectively. The `(hasTg,k) = (0,1)` column is a plain move, not a
   transform (§4.6).
9. **The light-fragment table is fully populated** across the `K[0x28]`/
   `K[0x30]`/`K[0x38]` dimensions; a base-pointer-naive scan sees only the
   `(0,0,0)` corner (§2.1).

---

## 9. Method, so the numbers can be re-derived

All figures come from `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`
and `scratchpad/mtxk_r.asm` (`objdump -d -M intel -r`).

- **Fragment decode**: `tools/parhelia-warpreg.py`, imported as a module, gives
  the registry (`registry()`) and the per-instruction field decode (`decode()`).
  574 fragments, 39 042 instructions, 19 001 of them arithmetic
  (opcode ≥ 0x60, plus 0x0a and 0x4a).
- **Register census**: reads are taken from all three source slots with the
  `exthi`/`extsel` widening applied; writes from `wmask0 != 0` (port A) and
  `wmask1 != 0` (port B). Using `f0a`/`f1c` as additional enables changes the
  answer and is *not* done, for the reasons in §7.2.
- **`bN == rN`**: the offset sweep of §1.1, over 13 987 adjacent
  arithmetic-instruction pairs.
- **Reachability**: a per-function basic-block constant propagation (constants in
  32-bit registers, meet = intersection, jump targets as block heads, blocks with
  no resolved predecessor re-walked from an empty state) evaluated at every call
  to `mtx1503`/`mtx3579`/`mtx497`/`mtx2475`, unioned with a scan of every
  `mov DWORD PTR [<any>], imm32` in the module bucketed by which of the nine
  table-filler functions issued it, unioned with the literal lists
  `parhelia-warp-codegen.md` §1 already establishes. Residue verified per index
  by an exact `mov <reg32>, <index>` search over the whole disassembly.
- **Family fingerprints**: per fragment, presence of a 4×`op77` quad, reads of
  `r89`–`r108` / `r116`–`r117` / the material registers, presence of `op79`/
  `op7a`, presence of a branch, presence of `end`.

## RESOLVED STATICALLY: r7-r10 are loaded from constants, not inputs

Section 7 recorded the matrix-rows-versus-texcoord-inputs question as unresolvable
by dataflow and needing a hardware readback. That was wrong — the corpus does
answer it, in the fragments that write those registers.

The transform fragment `0xf` (`mtx3632`) **reads** `r0-r3` and `r7-r10` and
writes `a259-a262`, but it does **not write** `r7-r10`. The fragments that do are
single-instruction loaders sitting immediately before it:

```
0x31  mtx3905   add.e.q  b8.xyzw   r86.4444, r0.xyzw, r0
0x32  mtx561    add.e.q  b9.xyzw   r84.4444, r0.xyzw, r0
0x33  mtx341    op75.q   b8.xyzw   r3.xyzw,  r8.xyzw, r0
```

`b8` is `r8` (port B writes the low sixteen of the source file, §1), and the
sources `r84`/`r86` are above the `r15` boundary, so they are **constant-space**
registers.

**So `r7-r10` are neither matrix rows held in place nor texture coordinate
inputs. They are working registers that a preceding fragment loads from the
constant file**, after which the four `dp4`s consume them. A transform is a
*sequence*: load the rows, then dot against them.

### Why our generated program produced nothing

`tools/parhelia-3dtri.c`'s `warp_gen_transform` emits only the four `dp4`s and
`END`. With no preceding loader, `r7-r10` hold whatever the engine left there,
so the dot products ran against undefined data. The program was structurally
incomplete, not merely mis-parameterised — which is a better explanation of the
empty render target than the missing constants theory, and it is consistent with
the engine accepting the work and executing it.

**Fix:** emit four loader instructions ahead of the `dp4`s, moving the
world-view rows from constant space into `r7-r10`, in the shape fragment `0x31`
uses.

**Still unknown, and needed for that fix:** which constant-space register index
corresponds to block store index 0x000-0x003, where the world-view matrix is
uploaded. The block-to-register mapping is established for texture matrices
(`r89+4i`) and materials (`r84`/`r118`) but not for the world-view rows. That is
one readback experiment — write a recognisable value to block 0x000 and dump the
constant-space registers to see where it lands — and it needs no draw.
