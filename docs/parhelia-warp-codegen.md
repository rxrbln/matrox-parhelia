# Parhelia WARP vertex-program CODE GENERATION — how a fixed-function GL state becomes microcode

Copyright (C) 2026 René Rebe <rene@exactco.de>

Companion to `docs/parhelia-vertex-isa.md` PART 2 (the WARP instruction
encoding), `docs/parhelia-vertex-pipeline.md` (which program is bound per draw)
and `docs/parhelia-draw-recipe.md` (submission). This file answers the one
question those three leave open: **how the vendor driver ASSEMBLES a vertex
program for an arbitrary GL state**, so an open driver can generate its own
instead of replaying captured images.

Source: `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`, unstripped
`ET_REL`, read through `scratchpad/mtxk_r.asm` (`objdump -d -M intel -r`).
Addresses are `.text` offsets. Tool: **`tools/parhelia-warpreg.py`** (new; dumps
and disassembles the fragment registry straight out of the object).

Evidence tags: **[CONFIRMED]** = read out of the binary or proved by a
structural identity; **[INFERRED]** = best reading, self-consistent, not proved;
**[OPEN]** = not determined. Nothing here has been run on hardware.

---

## 1. The state → fragment table

This is the deliverable. A fixed-function vertex program is a **list of
pre-compiled fragments** concatenated in emission order, with a flat list of
bit-field patches and a flat list of register relocations applied on top. The
table below is the complete mapping from pipeline state to that list, for the
program built by `mtx976` → `mtx2179`.

### 1.1 Notation

`K` is the ~0x58-byte pipeline state key at `ctx+0xdc` (`key2` in the pipeline
doc). `G` is the GL state object. `F(n)` = append registry fragment `n`.
`P(kind, value, instr)` = append a field patch. Registry indices are the ones
`tools/parhelia-warpreg.py` prints — index `n` lives at
`engine + 0x9c108 + n*0x18`.

### 1.2 Emission order — the whole program, top to bottom

`mtx2179` (`.text+0xa2400`) emits in exactly this order [CONFIRMED]:

| # | state condition | fragments emitted |
|---|---|---|
| 1 | `K[0x10] == 0` (pre-transformed vertices) | `0x01`, then §1.4, then `0x02`/`0xbe` |
| 1' | `K[0x10] != 0` ∧ `K[0x04]&0x02` | `0x05` |
| 2 | `K[0x10] != 0`: **one** T&L body generator, see §1.3 | `mtx2921` / `mtx1270` / `mtx2947` |
| 3 | `G[0x2a8]->u8[0x04] != 0` ∧ `K[0x10] != 0` — **fog** | see §1.6 |
| 4 | always | `mtx2177` — **texture coordinates**, §1.7 |
| 5 | `(K[0x06]&0x30)==0x10` ∧ `(K[0x08]&0x1c)==0x08` | `0x159` + `P(1, n+2, 5)` if `n != 1` |
| 6 | `K[0x04]&0x04` — **point size** | `0xc4` if `K[0x48]==3`, else `0xc5` |
| 7 | see §1.9 | `0x4f` |
| 8 | `G[0x2a0]->u8[0x5c]&0x04` | `0x65+i` for `i = 0 … count-1`, `count = G[0x2a0]->u32[0x08]` |
| 9 | `K[0x10] != 0` ∧ `(K[0x08]&0xe0) != 0` — **user clip planes** | `0xb0+(tm-1)` for `tm = K[0x08]>>5 ∈ 1..6`, then always `0xb6` |
| 10 | `K[0x10] != 0` ∧ `G[0x08]&0x08` — **lighting** | `mtx2755`, §1.5 |
| 11 | `G[0x2a8]->u8[0x05] != 0` ∧ `K[0x10] != 0` | `0xbf` + `P(0x18, 0xf40, 0)` if `(K[0x08]&3)==1` |
| 12 | always — **terminator** | `0xcb` if `K[0x04]&0x20` else `0xc8` (byte-identical, §4.5) |

Steps 9–11 are skipped entirely on the `K[0x10]==0` path.

### 1.3 Which T&L body (step 2) [CONFIRMED]

```c
lit     = G[0x08] & 0x08;                                   /* GL_LIGHTING   */
anylight= K[0x18] || K[0x1c] || K[0x20];                    /* spot|point|dir*/
if (!lit || !anylight)                    mtx2921(...);     /* unlit         */
else if (K[0x18] == 0 && K[0x1c] == 0)    mtx1270(...);     /* directional   */
else                                      mtx2947(...);     /* general       */
```

**`mtx2921` — the unlit body** (`.text+0xbd780`). Jump table `.rodata+0x5760`
on `mode = K[0x10]`:

| `K[0x10]` | `K[0x04]&0x80` clear | `K[0x04]&0x80` set |
|---|---|---|
| 1 | `0x50` + (`K[0x05]&1` ? `0x43` : `P(4,2,1) P(4,2,2) P(4,2,3) P(4,2,4)`) | `0x06` + (`K[0x05]&1` ? `0x43` : `P(4,2,3..6)`) |
| 2 | `0x51`, `0x42` | `0x07`, `0x42` |
| 3 | `0x52`, `0x42` | `0x08`, `0x42` |
| 4 | `0x53`, `0x42` | `0x09`, `0x42` |
| 5 | (`K[5]&4`?`0x0c`:`0x0d`), `0x0f`, `0x42` | (`K[5]&2`?`0x0a`:`0x0b`), (`K[5]&4`?`0x0c`:`0x0d`), `0x0e`, `0x42` |
| 0 or >5 | — | — |

then, on the `K[0x04]&0x80` paths only, `0x6a` if `(G[0x08]&0x20) || (G[0x09]&0x04)`;
then the common tail:

```
!(G[8]&8)                      -> 0x54          (colour pass-through)
else K[0x28] == 0 / 1 / 2      -> 0x55 / 0x56 / 0x57
K[5]&8                         -> (0x68 unless K[5]&1) then 0x69
(fog/eye-distance test)        -> 0x44
(K[8]&0x1c)==0x10 || K[4]&8    -> 0x4e, then
                                    G[9]&2 : K[4]&0x40 ? 0x46 : 0x45
                                    else   : K[4]&0x40 ? 0x48
                                           : K[6]&0x02 ? 0x4b : 0x4a
else (K[4]&0x10 ∧ (K[8]&0x1c)∈{0x0c,0x14}) or K[4]&4  -> 0x4e
else                                                  -> 0x4d
```

**`mtx1270` — the directional-only lit body** (`.text+0xbf2c0`) and
**`mtx2947` — the general lit body** (`.text+0xbdc70`) share a skeleton: the
same `K[0x10]` prologue switch (`0x06`–`0x0e`, identical), then one *main*
fragment chosen from a regular bank, then per-attribute patches, then the same
tail as `mtx2921`.

`mtx2947` main-fragment bank, selected by
`b4 = K[0x05]&0x10`, `b5 = K[0x05]&0x20`, `w18 = K[0x18] != 0`,
`g10 = G[0x08]&0x10`, and `A=K[0x28] B=K[0x30] C=K[0x38]` [CONFIRMED]:

| | `w18=0,g10=0` | `w18=0,g10=1` | `w18=1,g10=0` | `w18=1,g10=1` |
|---|---|---|---|---|
| `!b4` | `0x27` | `0x27` | `0x30` | `0x30` |
| `b4`, A=B=C=0, `!b5` | `0x1f` | `0x23` | `0x28` | `0x2c` |
| `b4`, `C==0` | `0x20` | `0x24` | `0x29` | `0x2d` |
| `b4`, `C!=0` | `0x21` | `0x25` | `0x2a` | `0x2e` |
| `b4`, `b5` | `0x22` | `0x26` | `0x2b` | `0x2f` |

`mtx1270` main-fragment bank, with `LOW = (K[0x10]==1 && !(K[5]&1) && !(G[8]&0x80))`:

| | `LOW` | plain | `g10` |
|---|---|---|---|
| `!b4` | `0x10` | `0x15` | `0x1a` |
| A=B=C=0, `!b5` | `0x11` | `0x16` | `0x1b` |
| `C==0` | `0x12` | `0x17` | `0x1c` |
| `C!=0` | `0x13` | `0x18` | `0x1d` |
| `b5` | `0x14` | `0x19` | `0x1e` |

On the `!b4` path both then emit the per-attribute expansion fragments:

```
b5 ∧ B==0 -> 0x31 ;  b5 ∧ A==0 -> 0x32
B: 0 -> 0x33 (only if b5)   1 -> 0x34   2 -> 0x35
A: 0 -> 0x36 (only if b5)   1 -> 0x37   2 -> 0x38
C: 0 -> 0x39 (only if b5)   1 -> 0x3a   2 -> 0x3b
```
and, in the shared tail, `!b4 ->` (`B!=0 || b5` ? `0x3c` : `0x3d`) then
(`A!=0 || b5` ? `0x3e` : `0x3f`).

### 1.4 The `K[0x10]==0` (pre-transformed) chain [CONFIRMED]

```
always                                            -> 0x01
(K[8]&0x1c)==0x10 || K[4]&8 :
      K[4]&1 -> 0x04 ; then K[4]&0x40 ? 0x48 : K[6]&2 ? 0x4b : 0x4a
else  K[4]&1 -> (K[4]&4 ? 0x04 : 0x03)            /* and suppress 0x02 */
G[0x2a8]->u8[5] ? 0xbe : (0x02 unless suppressed)
```

### 1.5 Lighting — `mtx2755` (`.text+0xc0ef0`), and the complete light selector

This is the part that was previously unreadable, and it now closes exactly.

`mtx2755` walks the enabled lights **two at a time**, emitting one fragment per
block plus its relocation and patch lists, then a per-material-term epilogue.
The fragment index is a **table lookup**, not a literal:

```c
L    = K[0x38] + 4*( K[0x30] + 9*sub + 3*K[0x28] + 36*mask );   /* 0 .. 9179 */
frag = *(u32 *)(engine + 0x104 + 4*L);
```

with, per block,

| field | meaning | source |
|---|---|---|
| `sub` | class of the **leading** light: `0` = point, `1` = spot, `2` = directional | the pair state machine below |
| `mask` bit 0 | `GL_LIGHT_MODEL_LOCAL_VIEWER` | `(G[0x08]>>4) & 1` |
| `mask` bits 3:1 | which light classes are in this block (below) | pair state machine |
| `mask` bit 4 | set when `!(K[0x05]&0x10)` | `K[0x05]` |
| `mask` bit 5 | set on the **second (back-face) pass** | `K[0x05]&0x20` |
| `K[0x28]`, `K[0x30]`, `K[0x38]` | colour-material source of three material terms (§2.3) | key |

Pair state machine (`nA = K[0x18]` spot, `nB = K[0x1c]` point, `nC = K[0x20]`
directional) [CONFIRMED]:

| condition | consumes | `mask \|=` | `sub` |
|---|---|---|---|
| `nA > 1` | 2 spot | `0x0a` | 1 |
| `nA==1, nB>0` | spot+point | `0x08` | 1 |
| `nA==1, nB==0, nC>0` | spot+directional | `0x0c` | 1 |
| `nA==1` alone | 1 spot | `0x02` | 1 |
| `nA==0, nB>1` | 2 point | `0x08` | 0 |
| `nA==0, nB==1, nC>0` | point+directional | `0x0c` | 0 |
| `nA==0, nB==1` alone | 1 point | — | 0 |
| `nB==0, nC>1` | 2 directional | `0x0c` | 2 |
| `nB==0, nC==1` | 1 directional | `0x04` | 2 |

`mask` bit 3 set ⇒ the block holds **two** lights and the register slot advances
by 2, else by 1.

**Proof that this is right.** The sixteen surviving vendor symbols `a*L` are the
relocation lists of exactly these fragments, and their table addresses — written
literally by the three per-light-type initialisers `mtx3071` (directional),
`mtx1216` (point), `mtx3514` (spot), all called from `mtx222` with `rdi =
engine` — invert through the formula above to the right `(sub, mask)` for all
sixteen [CONFIRMED]:

| symbol | table addr | `sub` | `mask` | reading | registry idx | instr | relocs |
|---|---|---|---|---|---|---|---|
| `aPI1L` | `+0x104` | 0 | 0 | 1 point, infinite viewer | 0x183 `mtx1532` | 10 | 5 |
| `aPL1L` | `+0x344` | 0 | 1 | 1 point, local viewer | 0x195 `mtx1240` | 10 | 4 |
| `aSI1L` | `+0x614` | 1 | 2 | 1 spot, infinite | 0x1a7 `mtx2219` | 11 | 7 |
| `aSL1L` | `+0x854` | 1 | 3 | 1 spot, local | 0x1bf `mtx1865` | 11 | 6 |
| `aDI1L` | `+0xb24` | 2 | 4 | 1 directional, infinite | 0x16b `mtx2687` | 4 | 4 |
| `aDL1L` | `+0xd64` | 2 | 5 | 1 directional, local | 0x177 `mtx3599` | 5 | 4 |
| `aPI2L` | `+0x1304` | 0 | 8 | 2 point, infinite | 0x187 `mtx283` | 19 | 10 |
| `aPL2L` | `+0x1544` | 0 | 9 | 2 point, local | 0x199 `mtx1075` | 20 | 8 |
| `aSPL2L` | `+0x15d4` | 1 | 9 | spot+point, local | 0x1cb `mtx671` | 20 | 10 |
| `aSI2L` | `+0x1814` | 1 | 10 | 2 spot, infinite | 0x1ab `mtx1095` | 22 | 14 |
| `aSL2L` | `+0x1a54` | 1 | 11 | 2 spot, local | 0x1c3 `mtx2979` | 22 | 12 |
| `aPDI2L` | `+0x1c04` | 0 | 12 | point+directional, infinite | 0x18b `mtx960` | 13 | 9 |
| `aSDI2L` | `+0x1c94` | 1 | 12 | spot+directional, infinite | 0x1af `mtx2285` | 15 | 11 |
| `aDI2L` | `+0x1d24` | 2 | 12 | 2 directional, infinite | 0x16c `mtx3519` | 8 | 8 |
| `aSDL2L` | `+0x1ed4` | 1 | 13 | spot+directional, local | 0x1c7 `mtx334` | 16 | 10 |
| `aDL2L` | `+0x1f64` | 2 | 13 | 2 directional, local | 0x178 `mtx2637` | 10 | 8 |

Two independent checks pass on all sixteen: the parallel relocation-list array
lands at `engine + 0x9108 + 16*L` for exactly the same `L` (e.g. `aDI1L`'s
`{count, ptr}` pair sits at `engine+0xb988`, and `0x9108 + 16*648 = 0xb988`),
and every fragment is **exactly `max(reloc instruction index) + 1` instructions
long** — the last instruction of every lighting fragment carries a relocation.

This settles three things the earlier docs left as guesses:

- `S`/`P`/`D` in the vendor names are the **light types** spot / point /
  directional, and a two-letter form (`PD`, `SD`, `SP`) is one fragment
  handling **two lights of different types**.
- `I`/`L` is **infinite vs local viewer** (`GL_LIGHT_MODEL_LOCAL_VIEWER`),
  carried in `mask` bit 0, sourced from `G[0x08] bit 4`.
- **`1`/`2` is the number of lights in the block, not one- vs two-sided
  lighting.** Two-sidedness is `mask` bit 5 and is implemented as a whole
  **second pass** (§1.5.1), not as a doubled fragment. The size doubling that
  suggested "two-sided" is simply two lights.

Per block, after `F(frag)`, `mtx2755` replays four static lists verbatim:

```c
for r in reloc_list[engine + 0x9108 + 16*L]      /* 8-byte {instr, kind}   */
      mtx866(engine, r.instr, r.kind, slot);     /* value = light slot     */
for p in patch_list[engine + 0x76108 + 16*L]     /* 12-byte {instr,kind,v} */
for p in patch_list[engine + 0x2e108 + 16*L]
for p in patch_list[engine + 0x52108 + 16*L]
      mtx1345(engine, p);
if (K[0x24] & (1u << slot))
    for p in patch_list[engine + 0x2d108 + 16*(sub + 4*mask)]  mtx1345(...);
slot += (mask & 8) ? 2 : 1;
```

The relocation `value` is the **light slot**, and the relocator adds `7*value`
to the register index — so **each light owns a 7-register constant block**
[CONFIRMED arithmetic].

#### 1.5.1 Two-sided lighting

If `K[0x05]&0x20`, `mtx2755` runs the whole walk a **second time** from
`slot = 0`, with `mask |= 0x20` and using the *second* dword of each material
pair — `K[0x2c]`, `K[0x34]`, `K[0x3c]`, `K[0x44]` instead of `K[0x28]`,
`K[0x30]`, `K[0x38]`, `K[0x40]`. Pass 2 additionally replays two lists that do
not exist in pass 1 (`engine+0x9a108` always, `engine+0x9b108` for the first
block when `K[0x05]&0x10`), and ends with the back-face epilogue tables.

#### 1.5.2 The material-term epilogue

Pass 1 ends with up to two more fragments, from two tables in the engine object:

```c
r12 = (K[0x05]&0x40) ? K[0x40] : 3;
ebx = (K[0x05]&0x80) ? K[0x38] : 3;
if (arg5 /* = anylight */) {
    s = (ebx!=3 || r12!=3) | (!(G[0x10]&1) << 1) | (!(K[0x05]&0x10) << 2);
    e = ((K[0x04] & 0x11000) == 0x10000);
    F( epi1_A[ e + 2*(3*s + K[0x28]) ] );        /* engine+0xa0428, 48 u32 */
}
if (ebx!=3 || r12!=3 || !arg5)
    F( epi1_B[ r12 + 4*ebx ] );                  /* engine+0xa04e8, 16 u32 */
```

`epi1_A` rows `s = 0..7`, columns `(K[0x28]=0,1,2) × (e=0,1)`:

```
s=0: 1d7 000 | 1d8 000 | 1d9 000     s=4: 1dd 207 | 1de 208 | 1df 209
s=1: 1ef 000 | 1f0 000 | 1f1 000     s=5: 1f5 20a | 1f6 20b | 1f7 20c
s=2: 1e3 000 | 1e4 000 | 1e5 000     s=6: 1e9 20d | 1ea 20e | 1eb 20f
s=3: 1fb 000 | 1fc 000 | 1fd 000     s=7: 201 210 | 202 211 | 203 212
```

`epi1_B[r12 + 4*ebx]`:

```
ebx=0: 221 222 223 239      ebx=2: 22d 22e 22f 23b
ebx=1: 227 228 229 23a      ebx=3: 233 234 235 21f
```

The pass-2 tables `epi2_A` (`engine+0xa0528`) and `epi2_B` (`engine+0xa05e8`)
are the same shape with the back-face fragment triples:

```
epi2_A  s=0: 1da 000|1db 000|1dc 000   s=4: 1e0 213|1e1 214|1e2 215
        s=1: 1f2 000|1f3 000|1f4 000   s=5: 1f8 216|1f9 217|1fa 218
        s=2: 1e6 000|1e7 000|1e8 000   s=6: 1ec 219|1ed 21a|1ee 21b
        s=3: 1fe 000|1ff 000|200 000   s=7: 204 21c|205 21d|206 21e
epi2_B  ebx=0: 224 225 226 23c   ebx=1: 22a 22b 22c 23d
        ebx=2: 230 231 232 23e   ebx=3: 236 237 238 220
```

Reading `epi1_A[s=0] = {0x1d7, 0x1d8, 0x1d9}` against the registry closes the
last loop: those three fragments differ **only** in which register they read —
`r84`, `r5`, `r6` — and `epi2_A[s=0] = {0x1da, 0x1db, 0x1dc}` reads `r118`,
`r5`, `r6` and writes the `a7`/`b7-8` register set instead of `a1`/`b1-2`.
So `r84` = front material constant, `r118` = back material constant, and the
three-way index `K[0x28]` is the **colour-material source**: 0 = material
constant, 1 = `r5`, 2 = `r6` [CONFIRMED by the surviving names `AddEfm` /
`AddEfd` / `AddEfs`, which are registry `0x233`/`0x234`/`0x235` = `epi1_B`
row `ebx=3`, and read `r87` / `r5` / `r6` respectively].

### 1.6 Fog (step 3) [CONFIRMED]

`mode = *(u32 *)((u8*)G[0x2a8] + 0x1b)`, `flag = *(u32 *)(… + 0x1f)`:

| `mode` | `flag == 0` | `flag != 0` |
|---|---|---|
| 0 | `0x64` | `0x64` |
| 1 | `0x5e` | `0x5f` |
| 2 | `0x62` | `0x63` |
| 3 | `0x5a` | `0x5b` |
| other | — | — |

Three modes plus a pass-through matches `GL_LINEAR` / `GL_EXP` / `GL_EXP2` plus
an explicit fog coordinate; all four fragments read `r116`/`r117`, the fog
parameter registers [INFERRED naming, CONFIRMED structure].

### 1.7 Texture coordinates — `mtx2177` (`.text+0xc0380`) [CONFIRMED]

Prologue: if `K[0x10] != 0`, emit `0x6d` when `K[0x06]&0x04`, else `0x6e` when
`K[0x06]&0x08`.

Then a loop over `nTex = G[0x2a0]->u32[0x08]` texture units (**≤ 4**: the
function keeps four 4-entry stack arrays and does not bound-check). Per unit
`i`, from the 20-byte per-unit block at `G[0x2a0] + 0xc + 20*i`:

```
flags = block[0x0d]   bit4 = texture matrix enabled
                      bits[3:1] = ncomp_raw ; ncomp = ncomp_raw ? ncomp_raw : 4
                      bit7 = alternate destination bank
                      bit5 = (epilogue only, last unit)
modes : block[0x0e] low/high nibble = texgen mode for S / T
        block[0x0f] low/high nibble = texgen mode for R / Q
hasTg = (flags & 0x0e) != 0
```

If `ncomp != 4`, fragment `0xa7` fills the missing components; its `wmask0`
patch value is

| `ncomp` | matrix off | matrix on |
|---|---|---|
| 1 | `0x0e` | `0x0f` |
| 2 | `0x0c` | `0x0e` |
| 3 | `0x08` | `0x0c` |

Then the four coordinate stages **S, T, R, Q in that order**, each guarded by
`ncomp > stage`, each looking its fragment up in a per-stage 28-entry table
indexed by `idx = k + 2*hasTg + 4*mode` where `k` is the one-hot
"`ncomp == stage+1`" bit:

| `mode` | S `engine+0x9f6f0` | T `+0x9f760` | R `+0x9f7d0` | Q `+0x9f840` |
|---|---|---|---|---|
| 0 | `00 98 91 95` | `00 99 92 96` | `00 9a 93 97` | `00 00 94 94` |
| 1 | `9f a0 8a 8e` | `00 a1 8b 8f` | `00 a2 8c 90` | `00 00 8d 8d` |
| 2 | `9b 9c 7c 80` | `00 9d 7d 81` | `00 9e 7e 82` | `00 00 7f 7f` |
| 3 | `a3 a4 83 87` | `00 a5 84 88` | `00 a6 85 89` | `00 00 86 86` |
| 4 | `74 74 74 74` | `75 75 75 75` | `76 76 76 76` | `77 77 77 77` |
| 5 | `78 78 78 78` | `79 79 79 79` | `7a 7a 7a 7a` | `7b 7b 7b 7b` |
| 6 | `6f 6f 72 72` | `70 70 73 73` | `00 00 00 00` | `00 00 00 00` |

(columns are `(hasTg,k) = (0,0) (0,1) (1,0) (1,1)`; tables filled by `mtx787`
@`.text+0x9ce90`.) Modes 4 and 5 are viewer/`ncomp`-invariant and have a full
S/T/R/Q quad — object-linear and eye-linear; mode 6 exists only for S and T —
sphere map [INFERRED]. Modes 0–3 are the matrix/normal/reflection variants
[OPEN which is which].

Each stage then applies the same patch block:

```c
if      (flag_b)         { P(0x04, i+6, 0); P(0x05, 1, 0); }   /* dst1 = i+0x16 */
else if (flags & 0x80)   { P(0x04, 1, 0);   P(0x05, 0, 0); }
else                       P(0x04, i+3, 0);
if (mode == 0)             P(0x0c, i+7, 0);                     /* src0        */
if (hasTg) {
    P(0x09,  reg_j       & 0xf, 0);                             /* src1 low    */
    P(0x11, (reg_j >> 4) & 0x7, 0);                             /* exthi       */
    if (mode == 0 && flag_a)
        P(0x18, mtx2929[ K[0x4c + 12*i] ], 0);                  /* swzB        */
}
```
with `reg_j = 0x59 + 4*i + stage` and
`mtx2929 = {0x000, 0x928, 0x948, 0xa88, 0x688, 0 …}` (`.rodata+0xc7510`).
`flag_a = ((K[0x08]&3) == 2)`, `flag_b = ((G[0x2a0]->u8[0x5c] & 0x18) != 0)`.

Per-unit tail and epilogue:

```
flags&0x80 : [nothing emitted this unit -> 0xa8 + P(0x06,i+7,0)] , 0xae , 0xab + P(0x01,i+3,0)
else if nothing emitted ∧ flag_b ∧ i!=0 : 0xa9 + P(0x09,i+7,0) + P(0x01,i+6,0)
epilogue, flag_b : [0x5c&0x08 -> 0xac, 0xae] [0x5c&0x10 -> 0xaf] [0x5c&0x08 -> 0xad] 0xaa
epilogue, K[4]&0x10 ∧ (K[8]&0x1c)∈{0x0c,0x14} ∧ K[0x10]!=0 ∧ (last unit flags&0x20):
                   0x41 + P(0x04, nTex+2, 2)
```

`mtx2177` emits **no relocations**; the per-unit specialisation is entirely
field patches [CONFIRMED].

### 1.8 User clip planes (step 9) [INFERRED, strong]

`tm = K[0x08] >> 5` indexes a 7-entry jump table at `.rodata+0x2fd0`; `tm = n`
emits fragment `0xb0 + n - 1`, which is `n` instructions of
`opcode 0x7a/0x79  b5.xyzw ← r2.xyzw, r(12+j).xyzw` — a dot product of one
vector against `n` consecutive constant registers `r12..r17`, accumulated into
`b5`. Six is the OpenGL minimum number of user clip planes, and the kernel's
clip-plane array (`HW+0x1da0`, opcode `0x25`, `idx ≤ 5`) is likewise six
entries. `0xb6` is emitted afterwards for every `tm != 0`.

### 1.9 The `0x4f` predicate (step 7) [CONFIRMED]

```c
a = K[0x08] & 0x1c;  c = K[0x04];
if      (a == 0x10)  emit = 0;
else if (c & 0x08)   emit = 0;
else if ((c & 0x10) && K[0x10] && (a == 0x0c || a == 0x14)) emit = 1;
else                 emit = (c & 0x04) != 0;
```

---

## 2. The state key, as far as it is decoded

`K` is `ctx+0xdc`; `mtx332` (`.text+0xa6980`) stamps it into the program
descriptor for the cache comparator.

### 2.1 Fields used by the generators

| field | width | meaning | tag |
|---|---|---|---|
| `K[0x04]` | u32, bytes 4/5/6 also read as u8 | see §2.2 | |
| `K[0x08] & 0x03` | 2 | selects a `swzB` patch table entry in `mtx2177` | [OPEN] |
| `K[0x08] & 0x1c` | 3 | texture-environment / output-format class; values `0x08`, `0x0c`, `0x10`, `0x14` are tested | [OPEN] |
| `K[0x08] >> 5` | 3 | **number of user clip planes**, 0..6 | [INFERRED] |
| `K[0x10]` | u32 | **transform mode**: 0 = pre-transformed vertices, 1 = one matrix, 2/3/4 = 2/3/4 blend matrices, 5 = a further variant | [INFERRED] |
| `K[0x18]` | u32 | **number of spot lights** | [CONFIRMED] |
| `K[0x1c]` | u32 | **number of point lights** | [CONFIRMED] |
| `K[0x20]` | u32 | **number of directional lights** | [CONFIRMED] |
| `K[0x24]` | u32 | per-light-slot bitmask selecting an extra patch list | [CONFIRMED mechanism] |
| `K[0x28+8i]` | u32 | **front-face colour source of material term `i`** (0..3): 0 = material constant, 1 = `r5` (primary colour), 2 = `r6` (secondary colour), 3 = absent | [INFERRED, strong] |
| `K[0x2c+8i]` | u32 | the same for the **back face** | [CONFIRMED symmetry] |
| `K[0x48]` | u32 | compared `== 3`; selects `0xc4` vs `0xc5` and gates a whole `mtx2884` branch | [OPEN] |
| `K[0x4c + 0xc*i]` | u32 | per-texture-unit selector into `mtx2929` | [CONFIRMED mechanism] |

The `K[0x10]` reading rests on fragment sizes: `0x50`/`0x51`/`0x52`/`0x53` are
5/11/16/21 instructions — one extra 4×`dp4` block per mode step — and the
`K[0x04]&0x80` twins `0x06`/`0x07`/`0x08`/`0x09` are 8/20/29/38, i.e. the same
plus a normal transform per matrix.

### 2.2 `K[0x04]` and `K[0x05]` bits

| bit | effect | tag |
|---|---|---|
| `K[4] 0x01` | emits `0x03`/`0x04` on the pre-transformed path | [OPEN] |
| `K[4] 0x02` | emits `0x05` | [OPEN] |
| `K[4] 0x04` | **point size** — emits `0xc4`/`0xc5`, and forces `0x4f`/`0x4e` | [INFERRED] |
| `K[4] 0x08` | joins `(K[8]&0x1c)==0x10` in every output-format test | [OPEN] |
| `K[4] 0x10` | with `(K[8]&0x1c)∈{0x0c,0x14}`, enables `0x4f` and the `mtx2177` `0x41` epilogue | [OPEN] |
| `K[4] 0x20` | terminator `0xcb` instead of `0xc8` (same bytes) | [CONFIRMED] |
| `K[4] 0x40` | selects `0x48`/`0x46` over `0x4a`/`0x45` in every tail | [OPEN] |
| `K[4] 0x80` | **normal transform needed** — picks the `0x06..0x0e` bank over `0x50..0x53` | [INFERRED] |
| `K[5] 0x01` | picks `0x43` over the four `dst1` patches; part of `LOW` in `mtx1270` | [OPEN] |
| `K[5] 0x02`, `0x04` | mode-5 sub-variants (`0x0a`/`0x0b`, `0x0c`/`0x0d`) | [OPEN] |
| `K[5] 0x08` | emits `0x68`/`0x69` | [OPEN] |
| `K[5] 0x10` | main-fragment bank split; `mask` bit 4 in the light selector | [OPEN] |
| `K[5] 0x20` | **`GL_LIGHT_MODEL_TWO_SIDE`** — runs the light walk a second time with the back-face material sources | [CONFIRMED] |
| `K[5] 0x40` | use `K[0x40]` rather than 3 in `epi1_B` | [CONFIRMED mechanism] |
| `K[5] 0x80` | use `K[0x38]` rather than 3 in `epi1_B` | [CONFIRMED mechanism] |

### 2.3 GL-state bits used directly

| field | meaning | tag |
|---|---|---|
| `G[0x08] & 0x08` | **`GL_LIGHTING`** | [INFERRED, strong] |
| `G[0x08] & 0x10` | **`GL_LIGHT_MODEL_LOCAL_VIEWER`** (`mask` bit 0) | [CONFIRMED] |
| `G[0x08] & 0x20`, `G[0x09] & 0x04` | suppress the `f1d` patch; emit `0x6a` | [OPEN] |
| `G[0x08] & 0x80` | suppresses the `src0`/`f1c` zeroing chain | [OPEN] |
| `G[0x09] & 0x02` | selects `0x45`/`0x46` over `0x48`/`0x4a`/`0x4b` | [OPEN] |
| `G[0x10] & 0x01` | `s` bit 1 of the material epilogue | [OPEN] |
| `G[0x2a0]` | texture-unit array: `->u32[0x08]` = unit count, `->u8[0x5c]` flags, per-unit 20-byte blocks at `+0xc` | [CONFIRMED] |
| `G[0x2a8]` | fog / raster block: `->u8[0x04]` fog enable, `->u8[0x05]`, `->u8[0x0e]`, `->u32@0x1b` fog mode, `->u32@0x1f` fog flag | [CONFIRMED mechanism] |

### 2.4 Register conventions recovered

| register(s) | holds | tag |
|---|---|---|
| `r0` | the vertex position / current working vector (read by 28 k of 40 k instructions) | [CONFIRMED usage] |
| `r0..r3` | a 4-row matrix in the *canonical* (unrelocated) form of a transform fragment | [CONFIRMED] |
| `r5` | **primary colour** (`glColor`) | [CONFIRMED via `AddEfd`] |
| `r6` | **secondary colour** | [CONFIRMED via `AddEfs`] |
| `r12..r17` | user clip-plane equations | [INFERRED] |
| `r84`, `r87`, `r88`, `r121` | front material constants | [CONFIRMED via `AddEfm`, `epi*` tables] |
| `r118` | back material constant | [CONFIRMED via `epi2_A`] |
| `r89..r92` | texture-matrix rows | [CONFIRMED] |
| `r105..r108` | a second 4-row matrix used by `0x42`/`0x43` | [CONFIRMED] |
| `r116`, `r117` | fog parameters | [INFERRED] |
| `a1`/`b1..b2` | front lighting accumulator | [CONFIRMED] |
| `a7`/`b7..b8` | back lighting accumulator | [CONFIRMED] |
| `a65` | colour output | [INFERRED] |
| `a259..a262` | transformed position output | [CONFIRMED] |
| per-light block | **7 consecutive registers**, base `7 * slot` | [CONFIRMED] |
