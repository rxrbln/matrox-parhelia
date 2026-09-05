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

---

## 3. The generator machinery

### 3.1 The four vectors

| engine offset | element | built by | consumed by |
|---|---|---|---|
| `+0xa0` | `{u32 frag_index; u32 n_patches; u32 n_relocs}` (12 B) | `mtx1503`, `mtx3579` | `mtx3558` |
| `+0xc0` | `{u32 instr_index; u32 field_kind; u32 value}` (12 B) | `mtx2634`, `mtx1345` | `mtx3558` patcher |
| `+0xe0` | `{u32 instr_index; u32 kind; u32 value}` (12 B) | `mtx866` | `mtx3558` relocator |
| `+0x100` | `u32` running byte size | `mtx1503`/`mtx3579` | `new[]` in `mtx976` |

Each vector is the usual 32-byte `std::vector` head (`+0x00` alloc, `+0x08`
begin, `+0x10` end, `+0x18` cap).

### 3.2 The appenders [CONFIRMED]

```c
void mtx1503(engine *E, u32 frag, u32 len);   /* .text+0xa20e0 append at END   */
void mtx3579(engine *E, u32 frag, u32 len);   /* .text+0xa2010 insert at BEGIN */
      /* both: push {frag,0,0} to E+0xa0 ; E->[0x100] += len */

void mtx2634(engine *E, u32 field_kind, u32 value, u32 instr_index);  /* 0xa1fa0 */
void mtx1345(engine *E, u64 packed /*instr | kind<<32*/, u32 value);  /* 0xa1f30 */
      /* both: push {instr_index, field_kind, value} to E+0xc0
               and ++ the LAST recipe entry's n_patches            */

void mtx866 (engine *E, u32 instr_index, u32 kind, u32 value);        /* 0xa1ec0 */
      /* push {instr_index, kind, value} to E+0xe0
         and ++ the LAST recipe entry's n_relocs                    */
```

**Watch the argument order** — `mtx2634` takes `(kind, value, instr)` while
`mtx866` takes `(instr, kind, value)`. Both build the same record layout
`{instr, kind, value}`. This corrects the ordering assumed in
`docs/parhelia-vertex-pipeline.md` §3.1.

`mtx1345` is the bulk form: it copies a preformed `{instr, kind, value}` triple
straight out of a static table, which is how `mtx2755` replays the per-light
patch lists.

### 3.3 The alignment helpers — 32-instruction granularity [CONFIRMED]

Fragment `0xc9` (`mtx2026`, 12 bytes, 1 instruction, a bare `wra`) is the NOP
filler. Three helpers use it:

| helper | address | direction | rule |
|---|---|---|---|
| `mtx2936(E, _, len_bytes)` | `0xa2070` | **prepend** (`mtx3579`) | pad `len/12` up to a multiple of 32; **returns** the pad count |
| `mtx497(E, frag)` | `0xa2380` | append | pad one fragment's own `len/12` up to 32 |
| `mtx3939(E, u32 *cursor, u32 extra)` | `0xa2140` | append | pad `*cursor+extra` up to 32 and advance `*cursor` |
| `mtx3587(E)` | `0xa22e0` | append | pad the **whole recipe so far** up to 32 |

So the WARP wants program stages to start on 32-instruction boundaries. Only
`mtx324`/`mtx2884` use them; `mtx2179` never pads.

### 3.4 The linker `mtx3558` (`.text+0xa0f20`) [CONFIRMED]

```c
void mtx3558(engine *E, void *desc /*unused*/, u8 *dst)
{
    n  = (E->recipe.end - E->recipe.begin) / 12;
    pc = 0;                       /* GLOBAL patch cursor  */
    rc = 0;                       /* GLOBAL reloc cursor  */
    u8 *frag = dst;               /* destination, in BYTES */
    for (i = 0; i < n; i++) {
        e  = &E->recipe.begin[i];
        fe = &E->frag[e->frag_index];              /* E+0x9c108 + 24*idx */
        LocMemCpy(frag, fe->code, fe->len_bytes);
        for (j = 0; j < e->n_relocs;  j++, rc++) apply_reloc(frag, &E->reloc.begin[rc]);
        for (k = 0; k < e->n_patches; k++, pc++) apply_patch(frag, &E->patch.begin[pc]);
        frag += fe->len_bytes;
    }
}
```

Four facts an implementation must get right:

1. **`instr_index` in a patch or relocation record is relative to the start of
   the fragment it belongs to**, not to the program. The destination cursor
   `frag` advances once per recipe entry, at the very end, and both inner loops
   address through it.
2. **Order is memcpy → relocations → patches**, per fragment.
3. **Both cursors are global and flat.** They are zeroed once before the loop
   and only ever incremented; `n_patches`/`n_relocs` merely partition the two
   flat lists. Emission order therefore *is* the binding order.
4. **The linker adds nothing.** No header, no terminator, no END instruction,
   no padding. The output buffer is exactly `sum(fragment lengths)`; the
   terminating `end` instruction comes from the last recipe fragment
   (`0xc8`/`0xcb`).

### 3.5 The field patcher — all 36 kinds [CONFIRMED]

Dispatch is a 36-entry jump table at `.rodata+0x2eb0` on `field_kind`; kinds
above `0x23` are a silent no-op. Every handler is
`word = (word & ~mask) | ((value & fieldmask) << shift)` on the three dwords of
`program + 12*instr_index`.

| kind | field | word | bits | note |
|---|---|---|---|---|
| `0x00` | `wmask0` | w0 | 3:0 | destination-A write mask |
| `0x01` | `dst0` | w0 | 7:4 | `dstA[3:0]` |
| `0x02` | `f0a` | w0 | 9:8 | port-A control (`2` = idle) |
| `0x03`, `0x1c` | `wmask1` | w0 | 13:10 | |
| `0x04`, `0x1d` | `dst1` | w0 | 17:14 | |
| `0x05`, `0x1e`, `0x1f` | `f0bc` | w0 | 19:18 / 18 / 19 | |
| `0x06`+`0x07`, `0x20` | `src2` | w0 | 23:20 + 24, or 24:20 | |
| `0x08` | `f0e` | w0 | 26:25 | |
| `0x09`+`0x0a`, `0x21` | `src1` | w0 | 30:27 + 31, or 31:27 | |
| `0x0b`, `0x12` | `f1a` | w1 | 1:0 | |
| `0x0c`+`0x0d`, `0x22` | `src0` | w1 | 5:2 + 6, or 6:2 | |
| `0x0e` | `f1c` | w1 | 8:7 | port-B control (`0` = idle) |
| `0x0f` | `f1d` | w1 | 12:9 | |
| `0x10` | `dstA[8]` | w1 | 13 | |
| `0x11` | `exthi` | w1 | 16:14 | source-register high bits |
| `0x13` | `dstA[5:4]` | w1 | 21:20 | |
| `0x14` | `f1g` | w1 | 24:22 | |
| `0x15` | `dstA[7:6]` | w1 | 26:25 | |
| `0x16` | `f1i` | w1 | 29:27 | |
| `0x17` | `swzA` | w1/w2 | `w1[31:30] = v&3`, `w2[9:0] = (v>>2)&0x3ff` | 12-bit swizzle, source 0 |
| `0x18` | `swzB` | w2 | 21:10 | 12-bit swizzle, source 1 |
| `0x19` | `f2a` | w2 | 22 | |
| `0x1a` | `opcode` | w2 | 30:23 | |
| `0x1b` | `f2b` | w2 | 31 | |
| `0x23` | `imm32` | w1/w2 | `w2[21:0] = v>>10`, `w1[31:22] = v&0x3ff` | overlays both swizzles |

Kinds `0x1c`–`0x23` operate on a second stack copy of the same 12 bytes and are
alternative views, not a second instruction. The three "wide" kinds `0x20`,
`0x21`, `0x22` write the 5-bit source selects in one go; `0x06`+`0x07`,
`0x09`+`0x0a`, `0x0c`+`0x0d` are the split forms of the same fields, and the
generators use both (`mtx2177` uses `0x09` together with `0x11`).

### 3.6 The relocator [CONFIRMED]

```c
/* record {instr_index, kind, value};  w = 3*instr_index  */
n7 = 7 * value + <that operand's current low 4 bits>;
kind 0 (src0): w1[5:2]   = n7 & 0xf ; w1[16:14] = (n7>>4)&7 ; w1[19:17] = 0
kind 1 (src1): w0[30:27] = n7 & 0xf ; w1[16:14] = (n7>>4)&7 ; w1[19:17] = 1
kind 2 (src2): w0[23:20] = n7 & 0xf ; w1[16:14] = (n7>>4)&7 ; w1[19:17] = 2
```

Three consequences for a generator:

- the effective source register is `n7 & 0x7f`, i.e. `{exthi(3):idx(4)}`; bit 4
  of the 5-bit source field (`w1[6]`, `w0[31]`, `w0[24]`) is **never touched**;
- `extsel` (`w1[19:17]`) is written **only** here, and there is one per
  instruction — so **at most one operand per instruction may be relocated**;
- the stride is 7, so every relocatable per-item constant block is **7 vec4
  registers wide**. For lighting the item is the light slot; the maximum
  extended index observed in the library is 121, i.e. 17 blocks.

### 3.7 Entry points [CONFIRMED]

The program descriptor holds per-variant entry offsets at `desc+0x2c[0..10]`,
in instructions; the hardware handle is `4 * (base_instruction + entry[variant])`
with the WARP-enable bits ORed into the low two bits.

`mtx376` (the descriptor constructor) zeroes all eleven. **Only `mtx2884` ever
writes them**; programs built through `mtx976`/`mtx2179` and through `mtx324`
keep entry offset **0** for every variant. `mtx2884`'s rule is

```c
base = (variant==2||variant==4) ? finfo(frag)[1]
     : (variant==3||variant==5) ? finfo(frag)[2] : 0;
pad  = mtx2936(E, desc, bytes_emitted_before_the_last_fragment);   /* prepends NOPs */
desc->entry[variant] = base + pad;
```
where `finfo` is the registry entry's `+0x10` pointer. Because the pad is
*prepended*, the last appended fragment always begins on a 32-instruction
boundary.

`+0x10` is initialised to NULL for all 575 slots by `mtx3882` and no other
writer of it exists in the module; the only paths that dereference it are
`mtx2884` variants 2–5, which are the *software*-WARP variants. On the hardware
path `mtx1881` calls `mtx2884` with variant 6 only, and variant 6 never touches
`finfo` [CONFIRMED; whether variants 2–5 are dead or filled from outside the
module is **[OPEN]**].

### 3.8 The two program families, and who builds which

| built by | called from | what it is |
|---|---|---|
| `mtx976` → `mtx2179` | `mtx2381`, on shader-state change | **the fixed-function vertex program** — everything in §1 |
| `mtx2884(…, variant 6, …)` | `mtx1881`, hardware path | an auxiliary stage program, built from its own tables |
| `mtx324(…, variant 0, …)` | `mtx1881` when `ctx[0x6f8] != 0` | the `GL_EXT_vertex_shader` path |
| `mtx2884(…, variants 2..5, …)` | `mtx1881`, software-WARP path only | |
| `mtx2475(engine, kind, i)` | `mtx1881`, `mtx3654` | the **canned** `.data` programs (kinds 0–17), no linking at all |

`mtx976`'s cache is 5 round-robin hot slots plus an LRU list capped at 0x2a9 =
681 entries; a miss zeroes `E[0x100]`, clears the three vectors, runs
`mtx2179`, `new[]`s the buffer and links it.

`mtx324` and `mtx2884` additionally emit fragments of their own:

- `mtx324`: a 27-entry table at `engine+0xa033c` indexed by `9*B + 3*A + C`,
  or `0x151`/`0x152`, or the chain `0x153` → (`0x154`) → 32-align → `0x155`
  or `0x156` → `0x157` or `0x158`, with `imm32` patches
  `1 << ((s2[8]>>5) + 15)` and `dst1`/`src2` patches carrying `nTex+2`.
- `mtx2884`: a 675-entry 5-D table at `engine+0x9f8b0` indexed by
  `225*c2c + 75*r9 + 25*c30 + 5*(b+r15) + r8`, plus literals `0x131`,
  `0x134`–`0x139`, `0x13b`–`0x13e` and the whole `0x15a`–`0x16a` family, plus
  `mtx569` for the per-light patch replay.

---

## 4. Worked example — plain transform with per-vertex colour

GL state: `glVertex` + `glColor`, `GL_LIGHTING` off, no textures, no fog, no
user clip planes, no point-size attenuation, one modelview-projection matrix.

Key values: `K[0x10] = 1`, `K[0x18] = K[0x1c] = K[0x20] = 0`, `K[0x04] = 0`,
`K[0x05] = 0`, `K[0x06] = 0`, `K[0x08] = 0`, `G[0x08] & 0x08 = 0`,
`G[0x2a0]->u32[0x08] = 0`, `G[0x2a8]->u8[0x04] = 0`, `G[0x2a8]->u8[0x05] = 0`.

### 4.1 Walk

| step | test | result |
|---|---|---|
| `mtx2179` §1.2 #1' | `K[4]&2` = 0 | — |
| #2 | `!lit` | `mtx2921` |
| `mtx2921` | `mode = K[0x10] = 1`, `K[4]&0x80` = 0, `K[5]&1` = 0 | `F(0x50)`, `P(4,2,1) P(4,2,2) P(4,2,3) P(4,2,4)` |
| `mtx2921` tail | `!(G[8]&8)` | `F(0x54)` |
| `mtx2921` tail | `K[5]&8` = 0 | — |
| `mtx2921` tail | `(K[8]&0x1c)!=0x10`, `K[4]&8`=0, `K[4]&0x10`=0, `K[4]&4`=0 | `F(0x4d)` |
| #3 fog | `G[0x2a8]->u8[4]` = 0 | — |
| #4 `mtx2177` | `K[6]` = 0, `nTex` = 0, both epilogue guards false | — |
| #5 | `(K[6]&0x30) != 0x10` | — |
| #6 | `K[4]&4` = 0 | — |
| #7 | `0x4f` predicate false | — |
| #8 | `G[0x2a0]->u8[0x5c]&4` = 0 | — |
| #9 | `(K[8]&0xe0)` = 0 | — |
| #10 | `G[8]&8` = 0 | — |
| #11 | `G[0x2a8]->u8[5]` = 0 | — |
| #12 | `K[4]&0x20` = 0 | `F(0xc8)` |

### 4.2 The recipe

```
recipe = [ {0x50, n_patches=4, n_relocs=0},
           {0x54, 0, 0},
           {0x4d, 0, 0},
           {0xc8, 0, 0} ]
patch  = [ {instr=1, kind=0x04, value=2},
           {instr=2, kind=0x04, value=2},
           {instr=3, kind=0x04, value=2},
           {instr=4, kind=0x04, value=2} ]
reloc  = []
size   = 60 + 36 + 36 + 24 = 156 bytes = 13 instructions
```

### 4.3 The linked instruction stream

Bytes are little-endian dwords, three per instruction. Produced by applying
§3.4 to the four registry blobs; the four patches rewrite `dst1` (`w0[17:14]`)
of instructions 1–4 from `3` to `2`, i.e. the transformed position is written
to `b2` instead of `b3`.

```
   0: 00003e00 00440000 b89a2249   add.e.q  b0.xyzw            r0.4444, r0.xyzw, r0
   1: 1874a23f 28022303 bb9a21a2   op77.q   b2.w, a259.xyzw    r0.xyzw, r3.xyzw, r7
   2: 1084924f 28022303 bb9a21a2   op77.q   b2.z, a260.xyzw    r0.xyzw, r2.xyzw, r8
   3: 08948a5f 28022303 bb9a21a2   op77.q   b2.y, a261.xyzw    r0.xyzw, r1.xyzw, r9
   4: 00a4866f 28022303 bb9a21a2   op77.q   b2.x, a262.xyzw    r0.xyzw, r0.xyzw, r10
   5: 0e187c1f 32440215 b89a2249   add.e.q  b1.xyzw, a65.xyzw  r5.4444, r1.xyzw, r1
   6: 12257ebf 00440289 c49a22a2   op89.q   b5.xyzw, a11.xyzw  r2.xyz5, r2.xyzw, r2
   7: 0808be00 00440019 b89a2249   add.e.q  b2.xyzw            r6.4444, r1.xyzw, r0
   8: 40004e00 ea540107 b89a2246   add.e.q  b1.xy              r1.ww44, r8.xyzw, r0
   9: 2c105e00 22440181 bb6a2222   add.a.q  b1.xyz             r0.xyz4, r5.xyz5, r1
  10: 42583d58 ea540507 b99a2246   op73.q   b0.xyzw, a85.w     r1.ww44, r8.xyzw, r5
  11: 00000200 00040000 14c00000   end
  12: 00080200 00440000 301a21a2   wra
```

Instructions 1–4 are the textbook 4×`dp4`: `src0` = `r0` (the vertex position),
`src1` sweeping `r3, r2, r1, r0` (the matrix rows), destination components
`w, z, y, x`, with the same result also written to the port-A output registers
`a259..a262`. Instructions 5 and 7 move the primary (`r5`) and secondary (`r6`)
colours; instruction 6 writes `a11`. Instructions 8–10 are the output-format
tail. `end` is instruction 11.

The unpatched fragment `0x50` differs only in those four `dst1` nibbles:

```
   1: 1874e23f 28022303 bb9a21a2   op77.q   b3.w, a259.xyzw    …
```

### 4.4 Cross-check against `tools/warp_programs.h`

**None of the seven boot programs is this case**, and they are not meant to be:
they are a different, complementary set. The seven are the *canned* `.data`
blobs `mtx3605 mtx3694 mtx1223 mtx1739 mtx458 mtx3213 mtx2068` bound as
`S[0x28]`, `S[0x30]`, `S[0x38]`, `S[0x40]`, `S[0x60]` by `mtx1881`; the program
built above is the separately-bound generated program. A draw runs both.

Five of the seven **are** registry members, at contiguous indices — which is
itself a useful identification [CONFIRMED]:

| kind | symbol | registry idx | bytes | instr |
|---|---|---|---|---|
| 0 | `mtx1079` | `0x0cc` | 168 | 14 |
| 1 | `mtx251` | `0x0c6` | 348 | 29 |
| 3 | `mtx1824` | `0x0c7` | 204 | 17 |
| 4 [n lights] | `mtx1461`…`mtx3013` | `0x0dd − n` | 120…408 | 10…34 |
| 6 | `mtx3605` | `0x13f` | 552 | 46 |
| 7 | `mtx3694` | `0x140` | 564 | 47 |
| 9 | `mtx1739` | `0x141` | 492 | 41 |
| 10 | `mtx458` | `0x142` | 504 | 42 |
| 12 | `mtx2068` | `0x143` | 120 | 10 |
| 17 | `mtx3032` | `0x144` | 4932 | 411 |

`mtx1223` (kind 8) and `mtx3213` (kind 11) are **not** in the registry, nor are
`mtx367` (kind 2), `mtx2655` (kind 15) or `mtx223` (kind 16).

Diffing kind 6 (`mtx3605`) against the generated program shows they are
unrelated code: `mtx3605` contains no `dp4` at all (its opcode census is
`0x71`×11 `0x73`×11 `0x76`×11 `0x60`×8 `0x75`×2) and writes `a12 a33 a104-105
a107 a160 a193-194 a199-200 a203`. It is an output/format stage, not a
transform. The transform lives only in the generated program.

### 4.5 One more useful identity

Registry `0xc8` and `0xcb` are **byte-identical** — they are the only duplicate
pair in the whole 574-entry library. Both are `end` followed by a bare `wra`.
So `K[0x04] bit 5`, which chooses between them, has no effect on the emitted
microcode; it only changes which registry slot the recipe names.

---

## 5. The fragment registry

`mtx3882` (`.text+0x97030`, 24153 bytes of straight-line stores) builds a
**575-slot** table at `engine + 0x9c108`, stride `0x18`:

```c
struct warp_fragment {
    u32          len_bytes;   /* +0x00, always a multiple of 12          */
    u32          _pad;        /* +0x04, always 0                         */
    const u32   *code;        /* +0x08, R_X86_64_32S against a .data blob */
    const u32   *info;        /* +0x10, entry offsets; NULL at init (§3.7)*/
};
```

Slot 0 is a sentinel (`len = 8`, `code = NULL`). Slots 1–574 total
**468,504 bytes = 39,042 instructions**. The registry ends at
`0x9c108 + 575*0x18 = 0x9f6f0`, which is exactly where the four texgen tables of
§1.7 begin — an independent confirmation of the slot count.

`tools/parhelia-warpreg.py` recovers all of this straight out of the object
(length from the immediate store, code pointer from the relocation) and needs
no hand-maintained table:

```
tools/parhelia-warpreg.py matroxdriver-*/kernel/mtx_parhelia.o          # the table
tools/parhelia-warpreg.py matroxdriver-*/kernel/mtx_parhelia.o -i 0x50  # disassemble one
tools/parhelia-warpreg.py matroxdriver-*/kernel/mtx_parhelia.o -c       # CSV
tools/parhelia-warpreg.py matroxdriver-*/kernel/mtx_parhelia.o -g       # duplicate blobs
```

### 5.1 Families

The registry is laid out by function, and every band boundary below is
corroborated either by a generator that indexes into it or by the register set
the fragments touch.

| range | n | family | how it is addressed |
|---|---|---|---|
| `0x001`–`0x005` | 5 | pre-transformed-vertex head | literals in `mtx2179` |
| `0x006`–`0x009` | 4 | position **and normal** transform, 1…4 blend matrices | `mtx2921`/`mtx2947`/`mtx1270` `K[0x10]` switch |
| `0x00a`–`0x00f` | 6 | transform-mode-5 parts; `0x0f` is the bare 4×`dp4` | same switch |
| `0x010`–`0x01e` | 15 | lit T&L body, directional-only (`mtx1270`), 3 sub-banks × 5 modes | bank table §1.3 |
| `0x01f`–`0x030` | 18 | lit T&L body, general (`mtx2947`), 2×(4+4+1) | bank table §1.3 |
| `0x031`–`0x03b` | 11 | per-attribute expansion, driven by `A`/`B`/`C` | literals |
| `0x03c`–`0x03f` | 4 | attribute tail | literals |
| `0x042`–`0x043` | 2 | second-matrix 4×`dp4` on `r105..r108` | literals |
| `0x044`–`0x04f` | 12 | output-format tails (`0x45 0x46 0x48 0x4a 0x4b 0x4d 0x4e`) | literals |
| `0x050`–`0x053` | 4 | position-only transform, 1…4 blend matrices | `K[0x10]` switch |
| `0x054`–`0x057` | 4 | colour pass-through and its three colour-material sources | `K[0x28]` |
| `0x058`–`0x064` | 13 | **fog** — 3 modes × 2 variants, plus pass-through, reading `r116`/`r117` | §1.6 |
| `0x065`–`0x06a` | 6 | per-item output write (`a68`/`a69`/`a70`, `.w` from `r0.y/z/w`) | `0x65+i` loop |
| `0x06b`–`0x06e` | 4 | texture prologue (`0x6d`/`0x6e`) | `K[0x06]` |
| `0x06f`–`0x0a6` | 56 | **texgen / texture-matrix stage** — 1-instruction `dp4`s against `r89..r92` with four `src0` choices | the four 28-entry tables §1.7 |
| `0x0a7`–`0x0af` | 9 | texture-coordinate defaults and output packing | literals in `mtx2177` |
| `0x0b0`–`0x0b6` | 7 | **user clip planes**, 1…6 planes plus epilogue | jump table `.rodata+0x2fd0` |
| `0x0c0`–`0x0c3` | 4 | `mtx569` per-item body, 1…4 items (+21 instructions each) | `mtx569`, index `S[0x80]+0xbf` |
| `0x0c4`–`0x0c5` | 2 | point size / point parameters | `K[0x04]&4` |
| `0x0c6`–`0x0cc` | 7 | the canned `.data` programs of kinds 0/1/3, the NOP filler `0xc9`, the terminators `0xc8`/`0xcb` | direct |
| `0x0cd`–`0x0dc` | 16 | canned kind 4 — the "N lights" family; index = `0xdd − N` | `mtx2475(e,4,N-1)` |
| `0x0e5`–`0x13e` | 90 | **monolithic** 100…560-instruction programs; none contains an `end`, so they are still fragments | `mtx2884`'s 675-entry table |
| `0x145`–`0x152` | 14 | more of the same | `mtx2884`, `mtx324`'s 27-entry table |
| `0x13f`–`0x144` | 6 | the canned fixed-function set, kinds 6/7/9/10/12/17 | direct |
| `0x153`–`0x158` | 6 | `mtx324`'s `GL_EXT_vertex_shader` chain | literals |
| `0x159`–`0x16a` | 18 | `mtx2884` literals and further monolithic bodies | literals |
| `0x16b`–`0x182` | 24 | **lighting, directional** | `engine+0x104` table, §1.5 |
| `0x183`–`0x1a6` | 36 | **lighting, point** | same |
| `0x1a7`–`0x1d6` | 48 | **lighting, spot** | same |
| `0x1d7`–`0x21e` | 72 | **material colour source** — 12 variants × {front, back} × {constant, primary colour, secondary colour} | `epi1_A` / `epi2_A` |
| `0x21f`–`0x23e` | 32 | 1- and 2-instruction accumulate words, same 3-way colour source; includes the five surviving `Add*` symbols | `epi1_B` / `epi2_B` |

Only **21 of the 574** fragments contain an `end` (opcode `0x29`): the two
terminators `0xc8`/`0xcb`, the canned kinds 0/1/3, and all sixteen members of
the kind-4 light family. Everything else is a fragment in the strict sense and
must be followed by a terminator.

The full per-entry table is in the appendix.

### 5.2 Reachability

Of the 574 fragments, the ones reachable by a **literal** index in a generator
are roughly a quarter; the rest are reached through the seven runtime tables:

| table | base | size | index | selects |
|---|---|---|---|---|
| light fragments | `engine+0x104` | 9180 u32 | §1.5 | `0x16b`–`0x1d6` |
| texgen S/T/R/Q | `engine+0x9f6f0` … `+0x9f840` | 4 × 28 u32 | §1.7 | `0x06f`–`0x0a6` |
| `mtx2884` main | `engine+0x9f8b0` | 675 u32 | 5-D | `0x0e5`–`0x152` |
| `mtx324` main | `engine+0xa033c` | 27 u32 | `9B+3A+C` | monolithic |
| `epi1_A` / `epi2_A` | `engine+0xa0428` / `+0xa0528` | 48 u32 each | `e + 2*(3s + K[..])` | `0x1d7`–`0x21e` |
| `epi1_B` / `epi2_B` | `engine+0xa04e8` / `+0xa05e8` | 16 u32 each | `r12 + 4*ebx` | `0x21f`–`0x23e` |

The parallel relocation and patch lists are addressed by the *same* index `L`
as the light table:

| array | base | record | applied with |
|---|---|---|---|
| relocations | `engine+0x9108 + 16*L` | 8 B `{instr, kind}` | `mtx866(…, value = light slot)` |
| patches | `engine+0x2e108 + 16*L` | 12 B `{instr, kind, value}` | `mtx1345` |
| patches | `engine+0x52108 + 16*L` | 12 B | `mtx1345` |
| patches | `engine+0x76108 + 16*L` | 12 B | `mtx1345` |
| patches | `engine+0x2d108 + 16*(sub+4*mask)` | 12 B | `mtx1345`, only if `K[0x24] & (1<<slot)` |
| patches | `engine+0x9a108`, `+0x9b108` | 12 B | `mtx1345`, second (back-face) pass only |

All of these are filled at init by `mtx222` (`.text+0x9efe0`), which memsets the
regions and then calls `mtx3071` (directional), `mtx1216` (point), `mtx3514`
(spot) and `mtx3882` (the registry) with `rdi = engine`.

---

## 6. What an open driver writes, and what it can copy

### 6.1 Copy verbatim — nothing, if the driver must be redistributable

The 574 fragments are **vendor object code**. They can be extracted from an
installed `matroxdriver` package the way firmware is (`parhelia-warpreg.py`
does exactly that), but they cannot be shipped inside a GPL driver. So for a
redistributable open driver the honest answer is:

> **the whole microcode library has to be rewritten**, and this document is the
> specification for doing so: the ISA is in `parhelia-vertex-isa.md` PART 2, the
> register conventions are in §2.4, and §1 is the list of pieces that have to
> exist.

The vendor library remains valuable as (a) a *validated corpus* to imitate
instruction by instruction, (b) a bring-up shortcut on a developer machine that
already has the vendor package installed, and (c) the reference against which a
freshly written fragment can be diffed.

### 6.2 What must be written from scratch either way

| piece | why |
|---|---|
| the recipe builder | it is a decision tree, not data; §1 is the spec |
| the linker | 40 lines, §3.4 |
| the field patcher | 36 one-line handlers, §3.5 |
| the relocator | 3 handlers, §3.6 |
| the constant-block layout | 7 registers per light, matrices where the fragments expect them |
| the runtime tables | 9180 + 112 + 675 + 27 + 128 entries; these are *data the driver chooses*, not vendor code, once its own fragments exist |

### 6.3 The minimum viable path

An open driver does **not** need the full tree to draw. §4's four-fragment
recipe is 13 instructions and covers `glVertex` + `glColor`. Adding capability
is additive and in this order:

1. **transform + colour** — fragments equivalent to `0x50`, `0x54`, `0x4d`,
   `0xc8`. 13 instructions.
2. **texture coordinates** — one `dp4` quad per unit against the texture-matrix
   registers, i.e. the `0x74`–`0x77` shape, plus the `0xa7` default fill.
3. **fog** — one of the four `0x5a`–`0x64` shapes.
4. **lighting** — the per-light bodies. This is where the 7-register block and
   the relocator become mandatory; up to two lights per fragment is an
   optimisation that can be skipped by always emitting the 1-light form.
5. **two-sided lighting** — a second pass with the back-face material registers.
6. **user clip planes** — `n` accumulating dot products against `r12+i`.

Steps 1–3 need **no relocations at all**: `mtx2921`, `mtx2177`, `mtx1270` and
`mtx2947` between them make 268 field patches and **zero** relocation appends.
Relocation is used only by `mtx2755`, i.e. only for lighting.

### 6.4 Numbers worth knowing

- instruction store: 512 instructions (6144 bytes); the canned boot set is 222
  instructions, so a generated program has roughly 290 left;
- program stages want to start on **32-instruction** boundaries (§3.3);
- `desc->n_blocks = ceil(instructions / 128)`;
- program cache: 5 hot slots + a 681-entry LRU.

---

## 7. Corrections to existing documents

1. **`parhelia-vertex-pipeline.md` §3.1: `mtx2634`'s argument order.** The
   record is `{instr_index, field_kind, value}` but the *call* is
   `mtx2634(engine, field_kind, value, instr_index)`. The relocation appender is
   `mtx866` (`.text+0xa1ec0`) and its order **is** `(engine, instr_index, kind,
   value)`. There is a third appender, `mtx1345` (`.text+0xa1f30`), which pushes
   a preformed triple. (§3.2)
2. **`parhelia-vertex-isa.md` §13: the `a*L` → fragment table.** Its index
   column is right and its symbol/instruction/relocation columns are shifted by
   one. The correct mapping is in §1.5; all sixteen entries fit, including the
   two the old table reported as "does not fit", and each fragment is exactly
   `max(reloc instr) + 1` instructions long.
3. **`parhelia-vertex-isa.md` §13: the naming scheme.** `S`/`P`/`D` are the
   **light types** spot/point/directional, not lighting terms; `1`/`2` is the
   **number of lights** in the fragment, not one- vs two-sided. Two-sided
   lighting is a second whole pass (§1.5.1). `I`/`L` = infinite/local viewer is
   confirmed, and is `G[0x08]` bit 4.
4. **`parhelia-vertex-pipeline.md` §3.3: the fragment-index lists per
   generator are incomplete.** `mtx2947` also emits `0x06 0x07 0x09 0x0a 0x0d
   0x0e 0x33–0x3b 0x3d 0x3f 0x46 0x4a 0x4b 0x4d`; `mtx2921` also emits `0x07 0x09 0x0b
   0x0d 0x0e 0x0f 0x42 0x4a 0x4b 0x4d 0x51 0x53 0x55 0x56 0x57 0x6a`;
   `mtx2177` also emits `0x6d` and the four texgen tables; `mtx324` also emits
   `0x151 0x152 0x155 0x156` and a 27-entry table; `mtx2884` also emits
   `0x134 0x135 0x137 0x139` and the whole `0x15a`–`0x16a` family.
5. **`mtx2755`'s guard.** It is `K[0x10] != 0 && (G[0x08] & 0x08)`, not
   `glstate[0x2a8]->[0x1b] == 1` — that byte is the **fog mode** and gates a
   different branch.
6. **The recipe registry's `+0x10` field is a pointer to entry offsets, not a
   "loaded" flag** — though `mtx3882` leaves it NULL and only the software-WARP
   variants of `mtx2884` dereference it (§3.7).
7. **Four further recipe helpers exist** that no earlier document lists:
   `mtx3939`, `mtx3587`, `mtx497` (32-instruction alignment padding with
   fragment `0xc9`) and `mtx569` (the per-item fragment + patch-list replay used
   by `mtx2884`). (§3.3)

---

## 8. Still open

| item | what would settle it |
|---|---|
| the GL meaning of `K[0x04]` bits 0/1/3/4/6, `K[0x05]` bits 0–4, `K[0x08]` bits 0–4, `K[0x48]` | run the vendor stack under `tools/parhelia-glsweep`, toggle one `glEnable` at a time and diff the emitted recipe from a kernel trace |
| which of texgen modes 0–3 is `GL_SPHERE_MAP` / `GL_NORMAL_MAP` / `GL_REFLECTION_MAP` / matrix | same method, per `glTexGeni` mode |
| `K[0x10] == 5` | it has no blend-matrix reading; check whether it is `GL_VERTEX_BLEND` tweening or a `glRasterPos` path |
| the 12 variants inside the `0x1d7`–`0x21e` material band | they are indexed by `s` (3 bits) in `epi*_A`; `s` bit 1 is `G[0x10]&1` and bit 2 is `K[0x05]&0x10` |
| the meaning of the `0x65+i` loop (`G[0x2a0]->u32[0x08]` items writing `a68..a70`) | the same sweep; the fragments write only the `.w` component of three consecutive output registers |
| whether registry `+0x10` is ever non-NULL | trace `mtx2884` variants 2–5 on the software-WARP path |
| the `0x0e5`–`0x13e` / `0x145`–`0x152` monolithic band | 104 fragments of 100–560 instructions, reached only through `mtx2884`'s 5-D table; nothing in the fixed-function path touches them |
| everything about hardware behaviour | none of this has been executed on a Parhelia |

---

## Appendix — the complete 575-slot registry

Generated by `tools/parhelia-warpreg.py`. `bytes` is the registry's own length
field, which matches the ELF symbol size for all 574 real entries. `instr` =
`bytes / 12`.

| idx | symbol | bytes | instr | family | note |
|---|---|---|---|---|---|
| 0x000 | - | 8 | - | null sentinel | slot 0, code = NULL |
| 0x001 | `mtx3653` | 96 | 8 | pre-transformed head |  |
| 0x002 | `mtx3310` | 12 | 1 | pre-transformed head |  |
| 0x003 | `mtx3753` | 36 | 3 | pre-transformed head |  |
| 0x004 | `mtx2312` | 36 | 3 | pre-transformed head |  |
| 0x005 | `mtx1989` | 12 | 1 | pre-transformed head |  |
| 0x006 | `mtx2093` | 96 | 8 | pos+normal xform, 1..4 matrices |  |
| 0x007 | `mtx2622` | 240 | 20 | pos+normal xform, 1..4 matrices |  |
| 0x008 | `mtx2227` | 348 | 29 | pos+normal xform, 1..4 matrices |  |
| 0x009 | `mtx2076` | 456 | 38 | pos+normal xform, 1..4 matrices |  |
| 0x00a | `mtx2712` | 36 | 3 | transform mode 5 parts |  |
| 0x00b | `mtx1964` | 12 | 1 | transform mode 5 parts |  |
| 0x00c | `mtx1446` | 36 | 3 | transform mode 5 parts |  |
| 0x00d | `mtx3286` | 12 | 1 | transform mode 5 parts |  |
| 0x00e | `mtx2734` | 96 | 8 | transform mode 5 parts |  |
| 0x00f | `mtx3632` | 48 | 4 | transform mode 5 parts | bare 4xdp4 position transform |
| 0x010 | `mtx399` | 60 | 5 | lit T&L body (mtx1270) |  |
| 0x011 | `mtx2615` | 60 | 5 | lit T&L body (mtx1270) |  |
| 0x012 | `mtx1704` | 72 | 6 | lit T&L body (mtx1270) |  |
| 0x013 | `mtx1805` | 96 | 8 | lit T&L body (mtx1270) |  |
| 0x014 | `mtx3870` | 132 | 11 | lit T&L body (mtx1270) |  |
| 0x015 | `mtx1071` | 132 | 11 | lit T&L body (mtx1270) |  |
| 0x016 | `mtx1813` | 132 | 11 | lit T&L body (mtx1270) |  |
| 0x017 | `mtx1015` | 144 | 12 | lit T&L body (mtx1270) |  |
| 0x018 | `mtx3382` | 168 | 14 | lit T&L body (mtx1270) |  |
| 0x019 | `mtx1003` | 204 | 17 | lit T&L body (mtx1270) |  |
| 0x01a | `mtx1873` | 144 | 12 | lit T&L body (mtx1270) |  |
| 0x01b | `mtx1451` | 132 | 11 | lit T&L body (mtx1270) |  |
| 0x01c | `mtx1685` | 156 | 13 | lit T&L body (mtx1270) |  |
| 0x01d | `mtx522` | 180 | 15 | lit T&L body (mtx1270) |  |
| 0x01e | `mtx975` | 216 | 18 | lit T&L body (mtx1270) |  |
| 0x01f | `mtx1039` | 192 | 16 | lit T&L body (mtx2947) |  |
| 0x020 | `mtx1120` | 204 | 17 | lit T&L body (mtx2947) |  |
| 0x021 | `mtx3765` | 228 | 19 | lit T&L body (mtx2947) |  |
| 0x022 | `mtx3504` | 264 | 22 | lit T&L body (mtx2947) |  |
| 0x023 | `mtx1944` | 204 | 17 | lit T&L body (mtx2947) |  |
| 0x024 | `mtx3676` | 216 | 18 | lit T&L body (mtx2947) |  |
| 0x025 | `mtx2343` | 240 | 20 | lit T&L body (mtx2947) |  |
| 0x026 | `mtx714` | 276 | 23 | lit T&L body (mtx2947) |  |
| 0x027 | `mtx2242` | 252 | 21 | lit T&L body (mtx2947) |  |
| 0x028 | `mtx942` | 204 | 17 | lit T&L body (mtx2947) |  |
| 0x029 | `mtx2271` | 216 | 18 | lit T&L body (mtx2947) |  |
| 0x02a | `mtx2570` | 240 | 20 | lit T&L body (mtx2947) |  |
| 0x02b | `mtx3623` | 276 | 23 | lit T&L body (mtx2947) |  |
| 0x02c | `mtx2608` | 228 | 19 | lit T&L body (mtx2947) |  |
| 0x02d | `mtx326` | 240 | 20 | lit T&L body (mtx2947) |  |
| 0x02e | `mtx1886` | 264 | 22 | lit T&L body (mtx2947) |  |
| 0x02f | `mtx743` | 300 | 25 | lit T&L body (mtx2947) |  |
| 0x030 | `mtx3367` | 360 | 30 | lit T&L body (mtx2947) |  |
| 0x031 | `mtx3905` | 12 | 1 | per-attribute expansion |  |
| 0x032 | `mtx561` | 12 | 1 | per-attribute expansion |  |
| 0x033 | `mtx341` | 12 | 1 | per-attribute expansion |  |
| 0x034 | `mtx3679` | 12 | 1 | per-attribute expansion |  |
| 0x035 | `mtx2535` | 12 | 1 | per-attribute expansion |  |
| 0x036 | `mtx2875` | 12 | 1 | per-attribute expansion |  |
| 0x037 | `mtx2069` | 12 | 1 | per-attribute expansion |  |
| 0x038 | `mtx2009` | 12 | 1 | per-attribute expansion |  |
| 0x039 | `mtx905` | 12 | 1 | per-attribute expansion |  |
| 0x03a | `mtx1998` | 12 | 1 | per-attribute expansion |  |
| 0x03b | `mtx2892` | 12 | 1 | per-attribute expansion |  |
| 0x03c | `mtx1221` | 12 | 1 | attribute tail |  |
| 0x03d | `mtx3925` | 12 | 1 | attribute tail |  |
| 0x03e | `mtx1187` | 12 | 1 | attribute tail |  |
| 0x03f | `mtx624` | 12 | 1 | attribute tail |  |
| 0x040 | `mtx630` | 24 | 2 | stage helper |  |
| 0x041 | `mtx2167` | 36 | 3 | stage helper |  |
| 0x042 | `mtx999` | 60 | 5 | 2nd-matrix transform (r105-108) | 4xdp4 on r105-108, src0=r3 |
| 0x043 | `mtx1333` | 60 | 5 | 2nd-matrix transform (r105-108) | 4xdp4 on r105-108, src0=r0 |
| 0x044 | `mtx2967` | 12 | 1 | output-format tail |  |
| 0x045 | `mtx1844` | 84 | 7 | output-format tail |  |
| 0x046 | `mtx2735` | 84 | 7 | output-format tail |  |
| 0x047 | `mtx3398` | 24 | 2 | output-format tail |  |
| 0x048 | `mtx2752` | 60 | 5 | output-format tail |  |
| 0x049 | `mtx2804` | 48 | 4 | output-format tail |  |
| 0x04a | `mtx270` | 72 | 6 | output-format tail |  |
| 0x04b | `mtx3512` | 60 | 5 | output-format tail |  |
| 0x04c | `mtx3984` | 12 | 1 | output-format tail |  |
| 0x04d | `mtx3888` | 36 | 3 | output-format tail |  |
| 0x04e | `mtx3559` | 36 | 3 | output-format tail |  |
| 0x04f | `mtx1661` | 12 | 1 | output-format tail |  |
| 0x050 | `mtx1621` | 60 | 5 | position xform, 1..4 matrices | 4xdp4 MVP transform -> b3 / a259-262 |
| 0x051 | `mtx3291` | 132 | 11 | position xform, 1..4 matrices |  |
| 0x052 | `mtx1546` | 192 | 16 | position xform, 1..4 matrices |  |
| 0x053 | `mtx2035` | 252 | 21 | position xform, 1..4 matrices |  |
| 0x054 | `mtx737` | 36 | 3 | colour pass-through / source | primary+secondary colour -> a65/a11 |
| 0x055 | `mtx3945` | 36 | 3 | colour pass-through / source |  |
| 0x056 | `mtx3201` | 36 | 3 | colour pass-through / source |  |
| 0x057 | `mtx2123` | 36 | 3 | colour pass-through / source |  |
| 0x058 | `mtx3572` | 36 | 3 | fog |  |
| 0x059 | `mtx2056` | 36 | 3 | fog |  |
| 0x05a | `mtx1426` | 48 | 4 | fog |  |
| 0x05b | `mtx1076` | 48 | 4 | fog |  |
| 0x05c | `mtx1995` | 36 | 3 | fog |  |
| 0x05d | `mtx3876` | 36 | 3 | fog |  |
| 0x05e | `mtx1675` | 48 | 4 | fog |  |
| 0x05f | `mtx1037` | 48 | 4 | fog |  |
| 0x060 | `mtx800` | 36 | 3 | fog |  |
| 0x061 | `mtx2116` | 48 | 4 | fog |  |
| 0x062 | `mtx4028` | 60 | 5 | fog |  |
| 0x063 | `mtx2770` | 60 | 5 | fog |  |
| 0x064 | `mtx3491` | 12 | 1 | fog | fog coord pass-through |
| 0x065 | `mtx2581` | 12 | 1 | per-item output write |  |
| 0x066 | `mtx3525` | 12 | 1 | per-item output write |  |
| 0x067 | `mtx644` | 12 | 1 | per-item output write |  |
| 0x068 | `mtx821` | 12 | 1 | per-item output write |  |
| 0x069 | `mtx2527` | 12 | 1 | per-item output write |  |
| 0x06a | `mtx2685` | 12 | 1 | per-item output write |  |
| 0x06b | `mtx3832` | 48 | 4 | texture prologue |  |
| 0x06c | `mtx2906` | 24 | 2 | texture prologue |  |
| 0x06d | `mtx1006` | 36 | 3 | texture prologue |  |
| 0x06e | `mtx548` | 96 | 8 | texture prologue |  |
| 0x06f | `mtx2944` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x070 | `mtx2500` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x071 | `mtx739` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x072 | `mtx3219` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x073 | `mtx1211` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x074 | `mtx2587` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x075 | `mtx1566` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x076 | `mtx551` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x077 | `mtx3783` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x078 | `mtx3170` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x079 | `mtx2368` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x07a | `mtx1379` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x07b | `mtx419` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x07c | `mtx3808` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x07d | `mtx964` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x07e | `mtx1896` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x07f | `mtx2683` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x080 | `mtx3200` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x081 | `mtx3452` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x082 | `mtx1724` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x083 | `mtx2583` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x084 | `mtx3951` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x085 | `mtx1084` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x086 | `mtx1581` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x087 | `mtx573` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x088 | `mtx1126` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x089 | `mtx1337` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x08a | `mtx2201` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x08b | `mtx1134` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x08c | `mtx4034` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x08d | `mtx3188` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x08e | `mtx3006` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x08f | `mtx2266` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x090 | `mtx2041` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x091 | `mtx3279` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x092 | `mtx518` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x093 | `mtx1452` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x094 | `mtx2383` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x095 | `mtx2754` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x096 | `mtx2401` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x097 | `mtx600` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x098 | `mtx3618` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x099 | `mtx735` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x09a | `mtx506` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x09b | `mtx1109` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x09c | `mtx1325` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x09d | `mtx337` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x09e | `mtx2314` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x09f | `mtx3713` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a0 | `mtx1731` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a1 | `mtx3657` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a2 | `mtx2626` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a3 | `mtx2826` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a4 | `mtx1926` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a5 | `mtx1897` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a6 | `mtx1934` | 12 | 1 | texgen / texture-matrix stage |  |
| 0x0a7 | `mtx2933` | 12 | 1 | texcoord default + packing |  |
| 0x0a8 | `mtx1424` | 12 | 1 | texcoord default + packing |  |
| 0x0a9 | `mtx3821` | 12 | 1 | texcoord default + packing |  |
| 0x0aa | `mtx3003` | 36 | 3 | texcoord default + packing |  |
| 0x0ab | `mtx1351` | 12 | 1 | texcoord default + packing |  |
| 0x0ac | `mtx2306` | 36 | 3 | texcoord default + packing |  |
| 0x0ad | `mtx3727` | 36 | 3 | texcoord default + packing |  |
| 0x0ae | `mtx1849` | 24 | 2 | texcoord default + packing |  |
| 0x0af | `mtx226` | 108 | 9 | texcoord default + packing |  |
| 0x0b0 | `mtx2866` | 12 | 1 | user clip planes | 1 clip-plane dot products (r12..r12) |
| 0x0b1 | `mtx2760` | 24 | 2 | user clip planes | 2 clip-plane dot products (r12..r13) |
| 0x0b2 | `mtx1927` | 36 | 3 | user clip planes | 3 clip-plane dot products (r12..r14) |
| 0x0b3 | `mtx2190` | 48 | 4 | user clip planes | 4 clip-plane dot products (r12..r15) |
| 0x0b4 | `mtx2043` | 60 | 5 | user clip planes | 5 clip-plane dot products (r12..r16) |
| 0x0b5 | `mtx3352` | 72 | 6 | user clip planes | 6 clip-plane dot products (r12..r17) |
| 0x0b6 | `mtx1765` | 12 | 1 | user clip planes |  |
| 0x0b7 | `mtx2968` | 12 | 1 | helper |  |
| 0x0b8 | `mtx1492` | 48 | 4 | helper |  |
| 0x0b9 | `mtx310` | 12 | 1 | helper |  |
| 0x0ba | `mtx3290` | 12 | 1 | helper |  |
| 0x0bb | `mtx342` | 12 | 1 | helper |  |
| 0x0bc | `mtx3841` | 24 | 2 | helper |  |
| 0x0bd | `mtx1360` | 156 | 13 | helper |  |
| 0x0be | `mtx3146` | 36 | 3 | helper |  |
| 0x0bf | `mtx3570` | 12 | 1 | helper | 1 instr, patched swzB = 0xf40 |
| 0x0c0 | `mtx2226` | 756 | 63 | mtx569 per-item body, n=1..4 |  |
| 0x0c1 | `mtx393` | 1008 | 84 | mtx569 per-item body, n=1..4 |  |
| 0x0c2 | `mtx1336` | 1260 | 105 | mtx569 per-item body, n=1..4 |  |
| 0x0c3 | `mtx1668` | 1512 | 126 | mtx569 per-item body, n=1..4 |  |
| 0x0c4 | `mtx727` | 180 | 15 | point size |  |
| 0x0c5 | `mtx793` | 156 | 13 | point size |  |
| 0x0c6 | `mtx251` | 348 | 29 | canned kinds 0/1/3, NOP, terminators | = canned kind 1 (mtx251) |
| 0x0c7 | `mtx1824` | 204 | 17 | canned kinds 0/1/3, NOP, terminators | = canned kind 3 (mtx1824) |
| 0x0c8 | `mtx1974` | 24 | 2 | canned kinds 0/1/3, NOP, terminators | end + wra  (terminator) |
| 0x0c9 | `mtx2026` | 12 | 1 | canned kinds 0/1/3, NOP, terminators | 1-instruction NOP filler |
| 0x0ca | `mtx3856` | 24 | 2 | canned kinds 0/1/3, NOP, terminators |  |
| 0x0cb | `mtx1977` | 24 | 2 | canned kinds 0/1/3, NOP, terminators | end + wra  (byte-identical to 0xc8) |
| 0x0cc | `mtx1079` | 168 | 14 | canned kinds 0/1/3, NOP, terminators | = canned kind 0 (mtx1079) |
| 0x0cd | `mtx3013` | 408 | 34 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 16 lights |
| 0x0ce | `mtx3217` | 384 | 32 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 15 lights |
| 0x0cf | `mtx3530` | 360 | 30 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 14 lights |
| 0x0d0 | `mtx1306` | 336 | 28 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 13 lights |
| 0x0d1 | `mtx2059` | 312 | 26 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 12 lights |
| 0x0d2 | `mtx2787` | 288 | 24 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 11 lights |
| 0x0d3 | `mtx3048` | 264 | 22 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 10 lights |
| 0x0d4 | `mtx3345` | 240 | 20 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 9 lights |
| 0x0d5 | `mtx980` | 216 | 18 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 8 lights |
| 0x0d6 | `mtx2378` | 192 | 16 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 7 lights |
| 0x0d7 | `mtx2621` | 168 | 14 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 6 lights |
| 0x0d8 | `mtx894` | 144 | 12 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 5 lights |
| 0x0d9 | `mtx348` | 120 | 10 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 4 lights |
| 0x0da | `mtx1140` | 120 | 10 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 3 lights |
| 0x0db | `mtx1507` | 120 | 10 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 2 lights |
| 0x0dc | `mtx1461` | 120 | 10 | canned kind 4: N lights, idx = 0xdd-N | canned kind 4, 1 light |
| 0x0dd | `mtx933` | 24 | 2 | helper |  |
| 0x0de | `mtx3794` | 24 | 2 | helper |  |
| 0x0df | `mtx2923` | 24 | 2 | helper |  |
| 0x0e0 | `mtx1909` | 24 | 2 | helper |  |
| 0x0e1 | `mtx924` | 12 | 1 | helper |  |
| 0x0e2 | `mtx3992` | 108 | 9 | helper |  |
| 0x0e3 | `mtx853` | 36 | 3 | helper |  |
| 0x0e4 | `mtx1802` | 12 | 1 | helper |  |
| 0x0e5 | `mtx3733` | 2868 | 239 | monolithic program |  |
| 0x0e6 | `mtx1274` | 3024 | 252 | monolithic program |  |
| 0x0e7 | `mtx2437` | 3720 | 310 | monolithic program |  |
| 0x0e8 | `mtx2283` | 3720 | 310 | monolithic program |  |
| 0x0e9 | `mtx3597` | 3924 | 327 | monolithic program |  |
| 0x0ea | `mtx1407` | 3924 | 327 | monolithic program |  |
| 0x0eb | `mtx3668` | 576 | 48 | monolithic program |  |
| 0x0ec | `mtx3646` | 4272 | 356 | monolithic program |  |
| 0x0ed | `mtx3928` | 4284 | 357 | monolithic program |  |
| 0x0ee | `mtx3387` | 3948 | 329 | monolithic program |  |
| 0x0ef | `mtx823` | 4416 | 368 | monolithic program |  |
| 0x0f0 | `mtx1347` | 4416 | 368 | monolithic program |  |
| 0x0f1 | `mtx2164` | 4104 | 342 | monolithic program |  |
| 0x0f2 | `mtx3807` | 5316 | 443 | monolithic program |  |
| 0x0f3 | `mtx2444` | 5616 | 468 | monolithic program |  |
| 0x0f4 | `mtx1789` | 4008 | 334 | monolithic program |  |
| 0x0f5 | `mtx1210` | 4068 | 339 | monolithic program |  |
| 0x0f6 | `mtx1376` | 5064 | 422 | monolithic program |  |
| 0x0f7 | `mtx3037` | 5064 | 422 | monolithic program |  |
| 0x0f8 | `mtx3526` | 5196 | 433 | monolithic program |  |
| 0x0f9 | `mtx3428` | 5196 | 433 | monolithic program |  |
| 0x0fa | `mtx2795` | 1200 | 100 | monolithic program |  |
| 0x0fb | `mtx3557` | 5676 | 473 | monolithic program |  |
| 0x0fc | `mtx4005` | 5676 | 473 | monolithic program |  |
| 0x0fd | `mtx1627` | 5364 | 447 | monolithic program |  |
| 0x0fe | `mtx901` | 5748 | 479 | monolithic program |  |
| 0x0ff | `mtx3475` | 5748 | 479 | monolithic program |  |
| 0x100 | `mtx1197` | 5424 | 452 | monolithic program |  |
| 0x101 | `mtx1142` | 6444 | 537 | monolithic program |  |
| 0x102 | `mtx1289` | 6744 | 562 | monolithic program |  |
| 0x103 | `mtx1343` | 4332 | 361 | monolithic program |  |
| 0x104 | `mtx352` | 4392 | 366 | monolithic program |  |
| 0x105 | `mtx2476` | 5400 | 450 | monolithic program |  |
| 0x106 | `mtx2808` | 5400 | 450 | monolithic program |  |
| 0x107 | `mtx893` | 5520 | 460 | monolithic program |  |
| 0x108 | `mtx1816` | 5520 | 460 | monolithic program |  |
| 0x109 | `mtx3359` | 1200 | 100 | monolithic program |  |
| 0x10a | `mtx1677` | 6024 | 502 | monolithic program |  |
| 0x10b | `mtx1799` | 6024 | 502 | monolithic program |  |
| 0x10c | `mtx3781` | 5700 | 475 | monolithic program |  |
| 0x10d | `mtx3771` | 6072 | 506 | monolithic program |  |
| 0x10e | `mtx1517` | 6072 | 506 | monolithic program |  |
| 0x10f | `mtx2861` | 5760 | 480 | monolithic program |  |
| 0x110 | `mtx1173` | 1860 | 155 | monolithic program |  |
| 0x111 | `mtx3999` | 2124 | 177 | monolithic / mtx2884 literal |  |
| 0x112 | `mtx1054` | 120 | 10 | monolithic / mtx2884 literal |  |
| 0x113 | `mtx3172` | 2616 | 218 | monolithic / mtx2884 literal |  |
| 0x114 | `mtx4026` | 120 | 10 | monolithic / mtx2884 literal |  |
| 0x115 | `mtx703` | 2544 | 212 | monolithic / mtx2884 literal |  |
| 0x116 | `mtx2339` | 1092 | 91 | monolithic / mtx2884 literal |  |
| 0x117 | `mtx3909` | 924 | 77 | monolithic / mtx2884 literal |  |
| 0x118 | `mtx3164` | 1392 | 116 | monolithic / mtx2884 literal |  |
| 0x119 | `mtx659` | 1236 | 103 | monolithic / mtx2884 literal |  |
| 0x11a | `mtx2365` | 1488 | 124 | monolithic / mtx2884 literal |  |
| 0x11b | `mtx3449` | 1488 | 124 | monolithic / mtx2884 literal |  |
| 0x11c | `mtx1203` | 1392 | 116 | monolithic / mtx2884 literal |  |
| 0x11d | `mtx2080` | 1392 | 116 | monolithic / mtx2884 literal |  |
| 0x11e | `mtx3970` | 120 | 10 | monolithic / mtx2884 literal |  |
| 0x11f | `mtx3177` | 1980 | 165 | monolithic / mtx2884 literal |  |
| 0x120 | `mtx3778` | 1980 | 165 | monolithic / mtx2884 literal |  |
| 0x121 | `mtx433` | 1548 | 129 | monolithic / mtx2884 literal |  |
| 0x122 | `mtx3739` | 1800 | 150 | monolithic / mtx2884 literal |  |
| 0x123 | `mtx1946` | 1800 | 150 | monolithic / mtx2884 literal |  |
| 0x124 | `mtx1359` | 1368 | 114 | monolithic / mtx2884 literal |  |
| 0x125 | `mtx2136` | 1464 | 122 | monolithic / mtx2884 literal |  |
| 0x126 | `mtx1364` | 1464 | 122 | monolithic / mtx2884 literal |  |
| 0x127 | `mtx2604` | 1392 | 116 | monolithic / mtx2884 literal |  |
| 0x128 | `mtx705` | 1392 | 116 | monolithic / mtx2884 literal |  |
| 0x129 | `mtx1915` | 120 | 10 | monolithic / mtx2884 literal |  |
| 0x12a | `mtx1524` | 2160 | 180 | monolithic / mtx2884 literal |  |
| 0x12b | `mtx2494` | 2160 | 180 | monolithic / mtx2884 literal |  |
| 0x12c | `mtx4008` | 1668 | 139 | monolithic / mtx2884 literal |  |
| 0x12d | `mtx1708` | 2124 | 177 | monolithic / mtx2884 literal |  |
| 0x12e | `mtx1473` | 2124 | 177 | monolithic / mtx2884 literal |  |
| 0x12f | `mtx3224` | 1668 | 139 | monolithic / mtx2884 literal |  |
| 0x130 | `mtx2817` | 84 | 7 | monolithic / mtx2884 literal |  |
| 0x131 | `mtx2597` | 1008 | 84 | monolithic / mtx2884 literal |  |
| 0x132 | `mtx1706` | 3360 | 280 | monolithic / mtx2884 literal |  |
| 0x133 | `mtx847` | 3336 | 278 | monolithic / mtx2884 literal |  |
| 0x134 | `mtx1654` | 996 | 83 | monolithic / mtx2884 literal |  |
| 0x135 | `mtx1463` | 1320 | 110 | monolithic / mtx2884 literal |  |
| 0x136 | `mtx769` | 996 | 83 | monolithic / mtx2884 literal |  |
| 0x137 | `mtx2264` | 192 | 16 | monolithic / mtx2884 literal |  |
| 0x138 | `mtx1033` | 2580 | 215 | monolithic / mtx2884 literal |  |
| 0x139 | `mtx1225` | 2628 | 219 | monolithic / mtx2884 literal |  |
| 0x13a | `mtx1521` | 360 | 30 | monolithic / mtx2884 literal |  |
| 0x13b | `mtx2897` | 24 | 2 | monolithic / mtx2884 literal |  |
| 0x13c | `mtx2648` | 96 | 8 | monolithic / mtx2884 literal |  |
| 0x13d | `mtx4036` | 24 | 2 | monolithic / mtx2884 literal |  |
| 0x13e | `mtx1591` | 492 | 41 | monolithic / mtx2884 literal |  |
| 0x13f | `mtx3605` | 552 | 46 | canned kinds 6/7/9/10/12/17 | = canned kind 6 (main T&L A) |
| 0x140 | `mtx3694` | 564 | 47 | canned kinds 6/7/9/10/12/17 | = canned kind 7 (main T&L B) |
| 0x141 | `mtx1739` | 492 | 41 | canned kinds 6/7/9/10/12/17 | = canned kind 9 (post A) |
| 0x142 | `mtx458` | 504 | 42 | canned kinds 6/7/9/10/12/17 | = canned kind 10 (post B) |
| 0x143 | `mtx2068` | 120 | 10 | canned kinds 6/7/9/10/12/17 | = canned kind 12 (tail) |
| 0x144 | `mtx3032` | 4932 | 411 | canned kinds 6/7/9/10/12/17 | = canned kind 17 (software WARP) |
| 0x145 | `mtx3012` | 132 | 11 | monolithic program |  |
| 0x146 | `mtx3707` | 168 | 14 | monolithic program |  |
| 0x147 | `mtx2375` | 1596 | 133 | monolithic program |  |
| 0x148 | `mtx1634` | 3204 | 267 | monolithic program |  |
| 0x149 | `mtx331` | 3204 | 267 | monolithic program |  |
| 0x14a | `mtx3192` | 3168 | 264 | monolithic program |  |
| 0x14b | `mtx2733` | 3168 | 264 | monolithic program |  |
| 0x14c | `mtx3881` | 1740 | 145 | monolithic program |  |
| 0x14d | `mtx775` | 3360 | 280 | monolithic program |  |
| 0x14e | `mtx457` | 3360 | 280 | monolithic program |  |
| 0x14f | `mtx2240` | 3336 | 278 | monolithic program |  |
| 0x150 | `mtx2369` | 3336 | 278 | monolithic program |  |
| 0x151 | `mtx1066` | 4092 | 341 | monolithic program |  |
| 0x152 | `mtx3596` | 5292 | 441 | monolithic program |  |
| 0x153 | `mtx2348` | 816 | 68 | mtx324 vertex-shader path |  |
| 0x154 | `mtx2613` | 48 | 4 | mtx324 vertex-shader path |  |
| 0x155 | `mtx3674` | 5928 | 494 | mtx324 vertex-shader path |  |
| 0x156 | `mtx3090` | 1836 | 153 | mtx324 vertex-shader path |  |
| 0x157 | `mtx1868` | 2676 | 223 | mtx324 vertex-shader path |  |
| 0x158 | `mtx1038` | 3852 | 321 | mtx324 vertex-shader path |  |
| 0x159 | `mtx3672` | 72 | 6 | mtx2884 literal / monolithic | 6 instr, patched dst0 = n+2 |
| 0x15a | `mtx4019` | 3276 | 273 | mtx2884 literal / monolithic |  |
| 0x15b | `mtx1267` | 2892 | 241 | mtx2884 literal / monolithic |  |
| 0x15c | `mtx1610` | 4296 | 358 | mtx2884 literal / monolithic |  |
| 0x15d | `mtx2101` | 4296 | 358 | mtx2884 literal / monolithic |  |
| 0x15e | `mtx2796` | 3972 | 331 | mtx2884 literal / monolithic |  |
| 0x15f | `mtx1571` | 4668 | 389 | mtx2884 literal / monolithic |  |
| 0x160 | `mtx2698` | 4044 | 337 | mtx2884 literal / monolithic |  |
| 0x161 | `mtx350` | 5712 | 476 | mtx2884 literal / monolithic |  |
| 0x162 | `mtx278` | 5712 | 476 | mtx2884 literal / monolithic |  |
| 0x163 | `mtx785` | 5400 | 450 | mtx2884 literal / monolithic |  |
| 0x164 | `mtx3274` | 5004 | 417 | mtx2884 literal / monolithic |  |
| 0x165 | `mtx2867` | 4368 | 364 | mtx2884 literal / monolithic |  |
| 0x166 | `mtx232` | 6060 | 505 | mtx2884 literal / monolithic |  |
| 0x167 | `mtx2569` | 6060 | 505 | mtx2884 literal / monolithic |  |
| 0x168 | `mtx3660` | 5736 | 478 | mtx2884 literal / monolithic |  |
| 0x169 | `mtx749` | 5340 | 445 | mtx2884 literal / monolithic |  |
| 0x16a | `mtx1847` | 6468 | 539 | mtx2884 literal / monolithic |  |
| 0x16b | `mtx2687` | 48 | 4 | LIGHT: directional | aDI1L  1 directional, infinite viewer |
| 0x16c | `mtx3519` | 96 | 8 | LIGHT: directional | aDI2L  2 directional, infinite |
| 0x16d | `mtx3485` | 60 | 5 | LIGHT: directional |  |
| 0x16e | `mtx870` | 120 | 10 | LIGHT: directional |  |
| 0x16f | `mtx3079` | 84 | 7 | LIGHT: directional |  |
| 0x170 | `mtx1773` | 168 | 14 | LIGHT: directional |  |
| 0x171 | `mtx1910` | 120 | 10 | LIGHT: directional |  |
| 0x172 | `mtx3702` | 204 | 17 | LIGHT: directional |  |
| 0x173 | `mtx1801` | 96 | 8 | LIGHT: directional |  |
| 0x174 | `mtx2363` | 180 | 15 | LIGHT: directional |  |
| 0x175 | `mtx1064` | 132 | 11 | LIGHT: directional |  |
| 0x176 | `mtx670` | 216 | 18 | LIGHT: directional |  |
| 0x177 | `mtx3599` | 60 | 5 | LIGHT: directional | aDL1L  1 directional, local viewer |
| 0x178 | `mtx2637` | 120 | 10 | LIGHT: directional | aDL2L  2 directional, local |
| 0x179 | `mtx752` | 84 | 7 | LIGHT: directional |  |
| 0x17a | `mtx1699` | 168 | 14 | LIGHT: directional |  |
| 0x17b | `mtx3336` | 108 | 9 | LIGHT: directional |  |
| 0x17c | `mtx3089` | 216 | 18 | LIGHT: directional |  |
| 0x17d | `mtx1553` | 144 | 12 | LIGHT: directional |  |
| 0x17e | `mtx1729` | 252 | 21 | LIGHT: directional |  |
| 0x17f | `mtx2746` | 132 | 11 | LIGHT: directional |  |
| 0x180 | `mtx3469` | 216 | 18 | LIGHT: directional |  |
| 0x181 | `mtx2975` | 168 | 14 | LIGHT: directional |  |
| 0x182 | `mtx1335` | 252 | 21 | LIGHT: directional |  |
| 0x183 | `mtx1532` | 120 | 10 | LIGHT: point | aPI1L  1 point, infinite |
| 0x184 | `mtx3976` | 144 | 12 | LIGHT: point |  |
| 0x185 | `mtx401` | 168 | 14 | LIGHT: point |  |
| 0x186 | `mtx327` | 204 | 17 | LIGHT: point |  |
| 0x187 | `mtx283` | 228 | 19 | LIGHT: point | aPI2L  2 point, infinite |
| 0x188 | `mtx1496` | 240 | 20 | LIGHT: point |  |
| 0x189 | `mtx1188` | 288 | 24 | LIGHT: point |  |
| 0x18a | `mtx2111` | 324 | 27 | LIGHT: point |  |
| 0x18b | `mtx960` | 156 | 13 | LIGHT: point | aPDI2L point+directional, infinite |
| 0x18c | `mtx841` | 192 | 16 | LIGHT: point |  |
| 0x18d | `mtx2362` | 240 | 20 | LIGHT: point |  |
| 0x18e | `mtx3447` | 276 | 23 | LIGHT: point |  |
| 0x18f | `mtx329` | 276 | 23 | LIGHT: point |  |
| 0x190 | `mtx2481` | 312 | 26 | LIGHT: point |  |
| 0x191 | `mtx1169` | 480 | 40 | LIGHT: point |  |
| 0x192 | `mtx354` | 552 | 46 | LIGHT: point |  |
| 0x193 | `mtx540` | 348 | 29 | LIGHT: point |  |
| 0x194 | `mtx2756` | 420 | 35 | LIGHT: point |  |
| 0x195 | `mtx1240` | 120 | 10 | LIGHT: point | aPL1L  1 point, local |
| 0x196 | `mtx306` | 144 | 12 | LIGHT: point |  |
| 0x197 | `mtx940` | 168 | 14 | LIGHT: point |  |
| 0x198 | `mtx3813` | 204 | 17 | LIGHT: point |  |
| 0x199 | `mtx1075` | 240 | 20 | LIGHT: point | aPL2L  2 point, local |
| 0x19a | `mtx2258` | 240 | 20 | LIGHT: point |  |
| 0x19b | `mtx2716` | 288 | 24 | LIGHT: point |  |
| 0x19c | `mtx1396` | 324 | 27 | LIGHT: point |  |
| 0x19d | `mtx3493` | 156 | 13 | LIGHT: point |  |
| 0x19e | `mtx552` | 204 | 17 | LIGHT: point |  |
| 0x19f | `mtx3109` | 252 | 21 | LIGHT: point |  |
| 0x1a0 | `mtx240` | 288 | 24 | LIGHT: point |  |
| 0x1a1 | `mtx1380` | 264 | 22 | LIGHT: point |  |
| 0x1a2 | `mtx2018` | 300 | 25 | LIGHT: point |  |
| 0x1a3 | `mtx4023` | 480 | 40 | LIGHT: point |  |
| 0x1a4 | `mtx1279` | 552 | 46 | LIGHT: point |  |
| 0x1a5 | `mtx3198` | 384 | 32 | LIGHT: point |  |
| 0x1a6 | `mtx1978` | 456 | 38 | LIGHT: point |  |
| 0x1a7 | `mtx2219` | 132 | 11 | LIGHT: spot | aSI1L  1 spot, infinite |
| 0x1a8 | `mtx2109` | 156 | 13 | LIGHT: spot |  |
| 0x1a9 | `mtx4000` | 180 | 15 | LIGHT: spot |  |
| 0x1aa | `mtx449` | 216 | 18 | LIGHT: spot |  |
| 0x1ab | `mtx1095` | 264 | 22 | LIGHT: spot | aSI2L  2 spot, infinite |
| 0x1ac | `mtx1073` | 312 | 26 | LIGHT: spot |  |
| 0x1ad | `mtx931` | 360 | 30 | LIGHT: spot |  |
| 0x1ae | `mtx3160` | 396 | 33 | LIGHT: spot |  |
| 0x1af | `mtx2285` | 180 | 15 | LIGHT: spot | aSDI2L spot+directional, infinite |
| 0x1b0 | `mtx1717` | 216 | 18 | LIGHT: spot |  |
| 0x1b1 | `mtx2693` | 264 | 22 | LIGHT: spot |  |
| 0x1b2 | `mtx2309` | 300 | 25 | LIGHT: spot |  |
| 0x1b3 | `mtx3571` | 240 | 20 | LIGHT: spot |  |
| 0x1b4 | `mtx1353` | 288 | 24 | LIGHT: spot |  |
| 0x1b5 | `mtx2654` | 336 | 28 | LIGHT: spot |  |
| 0x1b6 | `mtx338` | 372 | 31 | LIGHT: spot |  |
| 0x1b7 | `mtx3918` | 372 | 31 | LIGHT: spot |  |
| 0x1b8 | `mtx3537` | 408 | 34 | LIGHT: spot |  |
| 0x1b9 | `mtx2993` | 744 | 62 | LIGHT: spot |  |
| 0x1ba | `mtx2639` | 816 | 68 | LIGHT: spot |  |
| 0x1bb | `mtx628` | 468 | 39 | LIGHT: spot |  |
| 0x1bc | `mtx385` | 504 | 42 | LIGHT: spot |  |
| 0x1bd | `mtx4010` | 636 | 53 | LIGHT: spot |  |
| 0x1be | `mtx3421` | 708 | 59 | LIGHT: spot |  |
| 0x1bf | `mtx1865` | 132 | 11 | LIGHT: spot | aSL1L  1 spot, local |
| 0x1c0 | `mtx534` | 156 | 13 | LIGHT: spot |  |
| 0x1c1 | `mtx2737` | 180 | 15 | LIGHT: spot |  |
| 0x1c2 | `mtx3154` | 216 | 18 | LIGHT: spot |  |
| 0x1c3 | `mtx2979` | 264 | 22 | LIGHT: spot | aSL2L  2 spot, local |
| 0x1c4 | `mtx3296` | 312 | 26 | LIGHT: spot |  |
| 0x1c5 | `mtx1720` | 360 | 30 | LIGHT: spot |  |
| 0x1c6 | `mtx2106` | 396 | 33 | LIGHT: spot |  |
| 0x1c7 | `mtx334` | 192 | 16 | LIGHT: spot | aSDL2L spot+directional, local |
| 0x1c8 | `mtx2249` | 240 | 20 | LIGHT: spot |  |
| 0x1c9 | `mtx1505` | 288 | 24 | LIGHT: spot |  |
| 0x1ca | `mtx1019` | 324 | 27 | LIGHT: spot |  |
| 0x1cb | `mtx671` | 240 | 20 | LIGHT: spot | aSPL2L spot+point, local |
| 0x1cc | `mtx404` | 288 | 24 | LIGHT: spot |  |
| 0x1cd | `mtx1820` | 336 | 28 | LIGHT: spot |  |
| 0x1ce | `mtx2832` | 372 | 31 | LIGHT: spot |  |
| 0x1cf | `mtx1315` | 372 | 31 | LIGHT: spot |  |
| 0x1d0 | `mtx1222` | 408 | 34 | LIGHT: spot |  |
| 0x1d1 | `mtx1409` | 744 | 62 | LIGHT: spot |  |
| 0x1d2 | `mtx634` | 816 | 68 | LIGHT: spot |  |
| 0x1d3 | `mtx2020` | 504 | 42 | LIGHT: spot |  |
| 0x1d4 | `mtx3894` | 540 | 45 | LIGHT: spot |  |
| 0x1d5 | `mtx1791` | 636 | 53 | LIGHT: spot |  |
| 0x1d6 | `mtx1329` | 708 | 59 | LIGHT: spot |  |
| 0x1d7 | `mtx843` | 24 | 2 | material colour source (epi*_A) | front material constant (r84) |
| 0x1d8 | `mtx445` | 24 | 2 | material colour source (epi*_A) | front from primary colour (r5) |
| 0x1d9 | `mtx1962` | 24 | 2 | material colour source (epi*_A) | front from secondary colour (r6) |
| 0x1da | `mtx2213` | 24 | 2 | material colour source (epi*_A) | back material constant (r118) |
| 0x1db | `mtx1421` | 24 | 2 | material colour source (epi*_A) | back from primary colour (r5) |
| 0x1dc | `mtx2607` | 24 | 2 | material colour source (epi*_A) | back from secondary colour (r6) |
| 0x1dd | `mtx3357` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1de | `mtx970` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1df | `mtx726` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1e0 | `mtx4006` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1e1 | `mtx1200` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1e2 | `mtx1728` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1e3 | `mtx3563` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1e4 | `mtx1698` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1e5 | `mtx1115` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1e6 | `mtx2127` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1e7 | `mtx574` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1e8 | `mtx1892` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1e9 | `mtx805` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1ea | `mtx2918` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1eb | `mtx321` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1ec | `mtx1762` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1ed | `mtx2670` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1ee | `mtx3456` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1ef | `mtx3389` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f0 | `mtx1236` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f1 | `mtx2424` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f2 | `mtx2664` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f3 | `mtx1751` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f4 | `mtx3492` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f5 | `mtx2330` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f6 | `mtx1639` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f7 | `mtx839` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f8 | `mtx2950` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1f9 | `mtx2980` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1fa | `mtx1316` | 24 | 2 | material colour source (epi*_A) |  |
| 0x1fb | `mtx358` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1fc | `mtx2335` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1fd | `mtx977` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1fe | `mtx3053` | 36 | 3 | material colour source (epi*_A) |  |
| 0x1ff | `mtx1154` | 36 | 3 | material colour source (epi*_A) |  |
| 0x200 | `mtx3684` | 36 | 3 | material colour source (epi*_A) |  |
| 0x201 | `mtx2983` | 24 | 2 | material colour source (epi*_A) |  |
| 0x202 | `mtx3142` | 24 | 2 | material colour source (epi*_A) |  |
| 0x203 | `mtx2786` | 24 | 2 | material colour source (epi*_A) |  |
| 0x204 | `mtx3394` | 24 | 2 | material colour source (epi*_A) |  |
| 0x205 | `mtx2352` | 24 | 2 | material colour source (epi*_A) |  |
| 0x206 | `mtx1930` | 24 | 2 | material colour source (epi*_A) |  |
| 0x207 | `mtx718` | 36 | 3 | material colour source (epi*_A) |  |
| 0x208 | `mtx2267` | 24 | 2 | material colour source (epi*_A) |  |
| 0x209 | `mtx2079` | 24 | 2 | material colour source (epi*_A) |  |
| 0x20a | `mtx388` | 36 | 3 | material colour source (epi*_A) |  |
| 0x20b | `mtx2050` | 24 | 2 | material colour source (epi*_A) |  |
| 0x20c | `mtx1476` | 24 | 2 | material colour source (epi*_A) |  |
| 0x20d | `mtx3900` | 48 | 4 | material colour source (epi*_A) |  |
| 0x20e | `mtx3692` | 36 | 3 | material colour source (epi*_A) |  |
| 0x20f | `mtx3147` | 36 | 3 | material colour source (epi*_A) |  |
| 0x210 | `mtx672` | 48 | 4 | material colour source (epi*_A) |  |
| 0x211 | `mtx2478` | 36 | 3 | material colour source (epi*_A) |  |
| 0x212 | `mtx1754` | 36 | 3 | material colour source (epi*_A) |  |
| 0x213 | `mtx3874` | 36 | 3 | material colour source (epi*_A) |  |
| 0x214 | `mtx1667` | 24 | 2 | material colour source (epi*_A) |  |
| 0x215 | `mtx264` | 24 | 2 | material colour source (epi*_A) |  |
| 0x216 | `mtx2916` | 36 | 3 | material colour source (epi*_A) |  |
| 0x217 | `mtx3718` | 24 | 2 | material colour source (epi*_A) |  |
| 0x218 | `mtx3155` | 24 | 2 | material colour source (epi*_A) |  |
| 0x219 | `mtx474` | 48 | 4 | material colour source (epi*_A) |  |
| 0x21a | `mtx1936` | 36 | 3 | material colour source (epi*_A) |  |
| 0x21b | `mtx533` | 36 | 3 | material colour source (epi*_A) |  |
| 0x21c | `mtx1138` | 48 | 4 | material colour source (epi*_A) |  |
| 0x21d | `mtx2650` | 36 | 3 | material colour source (epi*_A) |  |
| 0x21e | `mtx1979` | 36 | 3 | material colour source (epi*_A) |  |
| 0x21f | `mtx523` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x220 | `mtx1487` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x221 | `mtx2869` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x222 | `mtx943` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x223 | `mtx897` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x224 | `mtx3965` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x225 | `mtx1812` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x226 | `mtx1191` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x227 | `mtx2042` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x228 | `mtx2973` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x229 | `mtx3929` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x22a | `mtx1781` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x22b | `mtx3350` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x22c | `mtx3169` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x22d | `mtx1172` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x22e | `mtx3942` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x22f | `mtx3737` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x230 | `mtx3225` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x231 | `mtx2519` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x232 | `mtx500` | 24 | 2 | accumulate word (epi*_B) |  |
| 0x233 | `AddEfm` | 12 | 1 | accumulate word (epi*_B) | AddEfm  material constant (r87) |
| 0x234 | `AddEfd` | 12 | 1 | accumulate word (epi*_B) | AddEfd  primary colour (r5) |
| 0x235 | `AddEfs` | 12 | 1 | accumulate word (epi*_B) | AddEfs  secondary colour (r6) |
| 0x236 | `mtx3312` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x237 | `mtx2282` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x238 | `mtx2791` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x239 | `mtx507` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x23a | `AddAfd` | 12 | 1 | accumulate word (epi*_B) | AddAfd  primary colour (r5) |
| 0x23b | `AddAfs` | 12 | 1 | accumulate word (epi*_B) | AddAfs  secondary colour (r6) |
| 0x23c | `mtx1811` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x23d | `mtx2033` | 12 | 1 | accumulate word (epi*_B) |  |
| 0x23e | `mtx2940` | 12 | 1 | accumulate word (epi*_B) |  |
