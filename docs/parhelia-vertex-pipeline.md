# Parhelia VERTEX pipeline binding — how a pipeline state picks a WARP program

Copyright (C) 2026 René Rebe <rene@exactco.de>

Companion to `docs/parhelia-vertex-isa.md` (which describes the *instruction
encoding*) and `docs/parhelia-command-construction.md` (the *submission*
layer). This file describes the layer between them: **which vertex program the
WARP runs for a given GL state, how that program is assembled, where its
constants come from, and how vertex attributes reach its input registers.**

Source unless stated otherwise: `matroxdriver-x86_64-cvs-20090511/kernel/
mtx_parhelia.o` (unstripped, `mtxNNNN` symbols), read from
`scratchpad/mtxk_r.asm` (`objdump -d -M intel -r`). Addresses are `.text`
offsets. Evidence tags: **[CONFIRMED]** = read out of the binary; **[INFERRED]**
= best reading, self-consistent but not proven; **[OPEN]** = not determined,
with the experiment that would settle it.

---

## 0. Headline — the answer to "which program runs?"

There is **no single vertex program**. The WARP engine object holds an
**18-entry program table**, and a draw binds **several programs at once** — the
hardware runs a small pipeline of them, each addressed by its own entry point.

The table is read by one accessor, `mtx2475` (`.text+0x9f180`), and its whole
body is the map [CONFIRMED]:

```c
void *mtx2475(engine *e, int kind, int index)
{
    if ((unsigned)kind > 0x11) return NULL;         /* 18 kinds, 0..0x11   */
    if ((1u << kind) & 0x39fcf) return e->slot[kind];        /* direct     */
    if ((1u << kind) & 0x10)                                 /* kind 4     */
        return (char *)e->slot[4] + index * 0x138;           /* 16-deep    */
    return NULL;                                    /* kinds 5, 13, 14 unused */
}
```

`e->slot[kind]` is the pointer array at **engine + 0x10 + kind*8**. Every slot
is a **0x138-byte program descriptor**; kind 4's slot points at an array of
**16** such descriptors.

The **caller that chooses** is `mtx1881` (`.text+0x85a10`), the fixed-function
vertex-state resolver. Its selection is a handful of bit tests on the pipeline
state key, and it is short enough to quote in full (see §2). The essential line
is:

```c
if (state->flags_e0 & 0x04) { a = 7; b = 10; }   /* variant B */
else                        { a = 6; b =  9; }   /* variant A */
S->prog_main  = mtx2475(engine, a, 0);           /* kind 6 or 7  */
S->prog_post  = mtx2475(engine, b, 0);           /* kind 9 or 10 */
```

plus two conditional extras (kinds 8 and 11) and one always-present tail
(kind 12), and — on the lit path — kind 4 indexed by the **number of enabled
lights minus one**.

A program's hardware handle is not a pointer but the dword at
`descriptor + 0x0c`: the **entry address** returned by the loader `mtx1391` and
stored there by `mtx3740`. `mtx3301` (`.text+0xa7450`, `return p->[0x0c];`) is
the getter, and the per-draw emitter caches those dwords in the program-set
struct and re-emits only when one changes [CONFIRMED].

---

## 1. The program table

### 1.1 Slot map [CONFIRMED — `mtx2051` @`.text+0xa2a30`, the engine constructor]

`mtx2051` allocates one `new(0x138)` descriptor per slot and fills
`{ +0x18 = byte length, +0x20 = .data blob, +0x28 = block count }`. Sizes below
were cross-checked against `readelf -s` (all live in `.data`, section 14) and
all are exact multiples of 12.

| kind | engine off | default blob | bytes | instr | role |
|---|---|---|---|---|---|
| 0 | +0x10 | `mtx1079` | 168 | 14 | bound once at context create (`mtx3995`) |
| 1 | +0x18 | `mtx251` | 348 | 29 | bound once at context create; also re-bound per state (`mtx3654`) |
| 2 | +0x20 | `mtx367` | 300 | 25 | — |
| 3 | +0x28 | `mtx1824` | 204 | 17 | bound twice at context create, count 4 then 8 |
| 4 | +0x30 | 16-entry array | see §1.2 | | **per-light-count family** |
| 5 | +0x38 | *unused* | | | `mtx2475` returns NULL |
| 6 | +0x40 | `mtx3605` | 552 | 46 | main T&L, variant A |
| 7 | +0x48 | `mtx3694` | 564 | 47 | main T&L, variant B |
| 8 | +0x50 | `mtx1223` | 240 | 20 | conditional extra A |
| 9 | +0x58 | `mtx1739` | 492 | 41 | post/output stage, variant A |
| 10 | +0x60 | `mtx458` | 504 | 42 | post/output stage, variant B |
| 11 | +0x68 | `mtx3213` | 192 | 16 | conditional extra B |
| 12 | +0x70 | `mtx2068` | 120 | 10 | always-present tail |
| 13 | +0x78 | *unused* | | | |
| 14 | +0x80 | *unused* | | | |
| 15 | +0x88 | `mtx2655` | 684 | 57 | — |
| 16 | +0x90 | `mtx223` | 120 | 10 | — |
| 17 | +0x98 | `mtx3032` | 4932 | 411 | software-WARP path only (§1.3) |

Roles marked "—" are slots that `mtx2051` fills but that no `mtx2475` call site
in the module reaches with that `kind`; they are reachable only through the
generic accessor [CONFIRMED negative, from an exhaustive `readelf -rW` scan:
`mtx2475` has exactly 13 references, in `mtx2393`, `mtx3995`, `mtx1881` and
`mtx3654`].

### 1.2 Kind 4 — the 16-program light family [CONFIRMED]

`engine+0x30` points at `new[](0x1388)` = 16 × 0x138 descriptors, filled in one
run by `mtx2051` (`.text+0xa33ab`..`0xa3806`):

| index | blob | bytes | instr |
|---|---|---|---|
| 0 | `mtx1461` | 120 | 10 |
| 1 | `mtx1507` | 120 | 10 |
| 2 | `mtx1140` | 120 | 10 |
| 3 | `mtx348` | 120 | 10 |
| 4 | `mtx894` | 144 | 12 |
| 5 | `mtx2621` | 168 | 14 |
| 6 | `mtx2378` | 192 | 16 |
| 7 | `mtx980` | 216 | 18 |
| 8 | `mtx3345` | 240 | 20 |
| 9 | `mtx3048` | 264 | 22 |
| 10 | `mtx2787` | 288 | 24 |
| 11 | `mtx2059` | 312 | 26 |
| 12 | `mtx1306` | 336 | 28 |
| 13 | `mtx3530` | 360 | 30 |
| 14 | `mtx3217` | 384 | 32 |
| 15 | `mtx3013` | 408 | 34 |

From index 3 upward the size is exactly `120 + (i-3)*24` bytes — **two extra
instructions per index**. The one call site that indexes it is `mtx3654`
(`.text+0x8b510`, at `+0x8cfce`) [CONFIRMED]:

```c
S->cbuf_ptr  = &ctx[0x4e4];
S->cbuf_size = n * 8;
S->prog_light = mtx2475(engine, 4, ctx[0x4e0] - 1);
```

`ctx[0x4e0]` is the count that `mtx309` (`.text+0x89cb0`, the light-setup
routine that walks the per-light structs at `ctx[0x3c88 + i*8]`) writes. So the
index is **(number of enabled lights) − 1**, and the family is the
"iterate N lights" loop unrolled 16 ways [INFERRED — the arithmetic is
confirmed, the identification of `ctx[0x4e0]` as the light count rests on
`mtx309`'s per-light loop writing it].

The two-instructions-per-light growth is consistent with the fragment-library
evidence in `parhelia-vertex-isa.md` §13 (per-light bodies built from a handful
of opcodes) but it is a *different*, smaller code path than the named `a*L`
lighting fragments — those belong to the generated-program path (§3).

### 1.3 What `mtx3806` actually preloads — correction [CONFIRMED]

`docs/parhelia-vertex-isa.md` §12 lists "eight programs at +0x40…+0x70 and
+0x98, 2868 bytes / 239 instructions". Re-reading `mtx2051` instruction by
instruction, that slot table is **shifted by one**: `mtx1824` sits at +0x28
(kind 3), not +0x40, and +0x98 holds `mtx3032`, not `mtx2068`.

`mtx3806` (`.text+0x9fcc0`) is [CONFIRMED]:

```c
mtx2316(this, slot[0x40], /*reset=*/1, /*entry=*/2, 0);   /* kind 6  */
mtx2316(this, slot[0x48..0x70], 0, 2, 0);                 /* kinds 7..12 */
if (this->[0xa3114] == 1)                                  /* software WARP */
    mtx2316(this, slot[0x98], 0, 2, 0);                    /* kind 17 */
for each of those: mtx2822(prog, 1);                       /* prog[0x10] = 1 */
```

So the boot-time burst is **seven programs, kinds 6..12, `mtx3605` + `mtx3694`
+ `mtx1223` + `mtx1739` + `mtx458` + `mtx3213` + `mtx2068` = 2664 bytes = 222
instructions**, with the first load carrying `reset = 1`. Kind 17 (`mtx3032`,
411 instructions) is loaded **only** when `engine[0xa3114] == 1`, which is the
same flag that routes `mtx2316` to the software emulator `mtx1650`/`mtx1397`
instead of the `0x91c` FIFO (`parhelia-kernel-abi.md` §9.5). That is
self-consistent: 2664 + 4932 = 7596 bytes would overflow the 0x1800-byte
instruction store, so kind 17 can only ever be a software-path image.

Kinds 0–4 and 15–17 are **not** preloaded; they are loaded on demand by other
callers of `mtx2316`/`mtx1391`.

### 1.4 Descriptor layout, corrected [CONFIRMED]

| off | meaning |
|---|---|
| +0x00 | "installed" byte, set to 1 by `mtx2051` |
| +0x02 | owns-the-buffer flag (1 for linker output, 0 for a `.data` blob) |
| +0x0c | **hardware entry address** — written by `mtx3740`, read by `mtx3301` |
| +0x10 | loaded flag — written by `mtx2822(prog, 1)` |
| +0x18 | byte length (multiple of 12) |
| +0x20 | pointer to the microcode |
| +0x28 | **block count = ceil(size / 1536)** |
| +0x2c[] | per-variant entry offsets, in instructions |
| +0x4c | cleared at init |
| +0xac.. | the cached state key (§4.2) |

`+0x28` is **not** the instruction count. `mtx976` computes it as
`((size-1) * 0xaaaaaaab >> 32) >> 10) + 1` = `(size-1)/1536 + 1`
(`.text+0xa65ae`), and `mtx2051` writes the literal 1 for every small blob and
**4** for the 4932-byte `mtx3032` — `(4932-1)/1536 + 1 = 4` ✔. 1536 bytes = 128
instructions, so `+0x28` counts **128-instruction pages**. (`mtx676`'s stack
descriptor for the 4812-byte self-test likewise carries 4 — the value the ABI
doc records as "nblocks = 4" — which is the same formula, not a coincidence.)
This corrects `parhelia-vertex-isa.md` §12's `prog[0x28] = (size-1)/12 + 1`.

`mtx1391`'s return value, re-read at `.text+0x9fb0b`:

```c
old = this->[0xa3118];                       /* byte write cursor, pre-load */
this->[0xa3118] = old + n;
return (old / 3) + 4 * pDesc->[0x2c + 4*entryIdx];
```

`old/3` = `4 * (old/12)` = 4 × the base instruction index, so the returned
handle is **4 × (absolute instruction index of the entry point)**; the low two
bits are free and `mtx656` ORs the WARP-enable bit into the value it finally
writes to `0x920` [CONFIRMED].

---

## 2. Selection — `mtx1881` @`.text+0x85a10`

`mtx1881(ctx, glstate, S)` resolves the fixed-function vertex pipeline. `S` is
the *program set* struct that the per-draw emitter `mtx3654` later reads.

### 2.1 The state key it builds first [CONFIRMED]

If `ctx[0x01] & 2` it recomputes a **two-byte program key** at `ctx+0x160`:

```c
ctx[0x160] = ctx[0xe4];
b  = ctx[0x161];
b  = (b & ~0x03) | (ctx[0xe5] & 1) | ((ctx[0xe0] >> 1) & 2);
b  = (b & ~0x04) | ((glstate[0x08] << 1) & 4);
b  = (b & ~0x18) | ((glstate[0x0c] >> 1) & 0x18);
b  = (b & 0x7f)  | ((glstate.f[0x278] >= LC1) << 7);
ctx[0x161] = b;
```

`ctx+0x160` is what the *generated*-program builders (`mtx324`, `mtx2884`) take
as their lookup key. The float compare at `glstate+0x278` against `.LC1` is a
threshold test on a single pipeline float [OPEN — the constant was not read].

### 2.2 The two paths [CONFIRMED]

```c
sw = engine->[0xa3114];
if (sw == 0) {                                   /* hardware WARP */
    if (ctx[0x6f8] == 0) { S[0x5a8] = NULL;
                           S[0x5d0] = mtx2884(engine, ctx+0x160, 6, glstate, ctx+0xdc, ctx+0x740); }
    else                 { S[0x5a8] = mtx324 (engine, ctx+0x160, 0, glstate, ctx+0xdc, ctx+0x740); }
} else {                                         /* software WARP */
    if (ctx[0x6f8] != 0)   S[0x5a8] = mtx324(engine, ctx+0x160, 0, ...);
    else {
        S[0x5a8] = NULL;
        S[0x5b0] = mtx2884(engine, key, 2, ...);
        if (S[0x5b0] && !S[0x5b0]->[4]) {
            S[0x5b8] = mtx2884(engine, key, 3, ...);
            S[0x5c0] = mtx2884(engine, key, 4, ...);
            S[0x5c8] = mtx2884(engine, key, 5, ...);
        } else S[0x5b8] = S[0x5c0] = S[0x5c8] = NULL;
        S[0x5d0] = mtx2884(engine, key, 6, ...);
        S[0xc0]  = ctx[0x6f9];
    }
}
```

The third argument of `mtx324`/`mtx2884` is a **variant index 0..6**; each
builds (or finds in a cache) a program *linked from fragments* rather than a
canned blob. See §3.

### 2.3 The fixed-function part — the actual answer to "which program" [CONFIRMED]

Immediately after, `mtx1881` fills the canned-program slots of `S`:

```c
/* optional pair, only on the lit/two-sided path */
if ((int8)glstate[0x14] < 0 && ctx[0x7e0] != 0 && (!S[0x38] || !S[0x40]))
    if (((glstate[0x0c] >> 4) & 3) == 3) {
        S[0x38] = mtx2475(engine, 0x08, 0);     /* mtx1223, 20 instr */
        S[0x40] = mtx2475(engine, 0x0b, 0);     /* mtx3213, 16 instr */
        changed = 1;
    }

/* the main pair */
if ((ctx[0xdf] & 2) || !S[0x28] || !S[0x30]) {
    if (ctx[0xe0] & 4) { a = 0x07; b = 0x0a; }  /* mtx3694 / mtx458  */
    else               { a = 0x06; b = 0x09; }  /* mtx3605 / mtx1739 */
    S[0x28] = mtx2475(engine, a, 0);
    S[0x30] = mtx2475(engine, b, 0);
    changed = 1;
}

/* the tail */
if (!S[0x60]) { S[0x60] = mtx2475(engine, 0x0c, 0); changed = 1; }  /* mtx2068 */

S->hw[1] = (S->hw[1] & 0x7f) | (changed << 7);   /* dirty bit for the emitter */
```

So, in table form — the complete fixed-function state → program map:

| `S` field | selected by | kind | program |
|---|---|---|---|
| `S[0x28]` | `ctx[0xe0] & 4` clear | 6 | `mtx3605` (46 instr) |
| `S[0x28]` | `ctx[0xe0] & 4` set | 7 | `mtx3694` (47 instr) |
| `S[0x30]` | `ctx[0xe0] & 4` clear | 9 | `mtx1739` (41 instr) |
| `S[0x30]` | `ctx[0xe0] & 4` set | 10 | `mtx458` (42 instr) |
| `S[0x38]` | `glstate[0x14] < 0` ∧ `ctx[0x7e0]` ∧ `(glstate[0x0c]>>4)&3 == 3` | 8 | `mtx1223` (20 instr) |
| `S[0x40]` | same condition | 11 | `mtx3213` (16 instr) |
| `S[0x60]` | unconditional | 12 | `mtx2068` (10 instr) |
| `S[0x80]` (+ const block `S[0x70]`=`&ctx[0x4e4]`, `S[0x78]`=`n*8`) | `ctx[0x4e0] != 0` (lights enabled) | 4[n−1] | `mtx1461`…`mtx3013` |
| `S[0x98]` (+ const block `S[0x88]`=`&ctx[0x28]`, `S[0x90]`=0x10) | `ctx[0x7d9] != ctx[0x7a0]` (`mtx3654` @`0x8cb8d`) | 1 | `mtx251` |
| `S[0x5a8/0x5b0/…/0x5d0]` | see §2.2 | — | linked from fragments |

The single bit `ctx[0xe0] & 4` is therefore **the** fixed-function program
selector for the main pair. `ctx+0xdc` is the state-key struct (§4.2), so
`ctx[0xe0]` is key byte 4; the GL meaning of bit 2 of that byte was not pinned
[OPEN — see §8].

### 2.4 Where the selection reaches hardware [CONFIRMED]

`mtx3654` (`.text+0x8b510`, the per-draw state emitter) does, for each bound
program:

```c
old = S->cached_entry;                    /* S[0x48],[0x4c],[0x50],[0x54],
                                             [0x58],[0x5c],[0x68] … */
if (old != mtx3301(S->prog))              /* prog->[0x0c] */
    { S->cached_entry = mtx3301(S->prog);
      hwstate->[0x1a] |= 1; }             /* dirty bit */
```

The pairing observed: `S[0x28] → S[0x48]`, `S[0x30] → S[0x4c]`,
`S[0x38] → S[0x50]`, `S[0x60] → S[0x68]` [CONFIRMED at `.text+0x8c7bf`,
`0x8c80d`, `0x8cf98`, `0x8c868`]. So the per-draw cost of a program switch is
**one dword per stage**, not a re-upload.

---

## 3. The generated-program path — how the 574 registry fragments become a program

### 3.1 The three vectors on the engine object [CONFIRMED]

| engine off | element | built by | consumed by |
|---|---|---|---|
| +0xa0 | `{u32 frag_index; u32 n_patches; u32 n_relocs}` | `mtx1503`, `mtx2634` | `mtx3558` |
| +0xc0 | `{u32 instr_index; u32 field_kind; u32 value}` | `mtx2634` | `mtx3558` field patcher |
| +0xe0 | `{u32 instr_index; u32 kind; u32 value}` | (relocation appender) | `mtx3558` relocator |
| +0x100 | accumulated program size in bytes | `mtx1503` | `mtx976` (`new[]` size) |

`mtx1503` (`.text+0xa20e0`) is the fragment appender, and it is four
instructions of real work [CONFIRMED]:

```c
void mtx1503(engine *e, u32 frag_index, u32 len)
{
    struct rec { u32 frag, npatch, nreloc; } r = { frag_index, 0, 0 };
    vector_insert(&e->vec_a0, e->vec_a0.end, 1, &r);      /* mtx1102 */
    e->[0x100] += len;
}
```

Every call site passes `len` as `engine[0x9c108 + frag_index*0x18]`, i.e. the
registry entry's own `len` field — confirming the registry stride 0x18 and the
`len` at offset 0 [CONFIRMED; e.g. `mtx1503(e, 0x4a, e[0x9c7f8])` and
`0x9c108 + 0x4a*0x18 = 0x9c7f8`].

`mtx2634` (`.text+0xa1fa0`) appends a **field patch** and bumps the *last*
recipe entry's `n_patches`:

```c
void mtx2634(engine *e, u32 instr_index, u32 field_kind, u32 value)
{
    struct { u32 i, k, v; } p = { instr_index, field_kind, value };
    vector_insert(&e->vec_c0, e->vec_c0.end, 1, &p);      /* mtx1501 */
    last = &e->vec_a0.begin[3*(count-1)];
    last[1]++;                                            /* n_patches */
}
```

A structurally identical appender exists for the relocation vector at +0xe0
(bumping `last[2]`); it was not isolated by name [OPEN — cosmetic only, the
record format is fixed by `mtx3558`'s reader].

### 3.2 The linker `mtx3558` @`.text+0xa0f20` [CONFIRMED]

```c
void mtx3558(engine *e, void *recipe_unused, u32 *buf)
{
    n = (e->vec_a0.end - e->vec_a0.begin) / 12;
    patch_cursor = 0; reloc_cursor = 0;
    for (i = 0; i < n; i++) {
        rec = &e->vec_a0.begin[3*i];
        len  = e->reg[rec[0]].len;                    /* engine+0x9c108 + idx*0x18 */
        code = e->reg[rec[0]].code;                   /*                    + 0x08 */
        LocMemCpy(buf, code, len);

        for (k = 0; k < rec[2]; k++, reloc_cursor++)  /* relocations */
            apply_reloc(buf, &e->vec_e0.begin[3*reloc_cursor]);

        for (k = 0; k < rec[1]; k++, patch_cursor++)  /* field patches */
            apply_patch(buf, &e->vec_c0.begin[3*patch_cursor]);

        buf += len/4;
    }
}
```

Both cursors are **global**, not per-fragment: the relocation and patch lists
are flat and are consumed in order, `rec[1]`/`rec[2]` entries at a time. An open
driver reproducing this must keep the same ordering discipline.

The relocation and field-patch bit semantics are fully documented in
`docs/parhelia-vertex-isa.md` PART 2 §2 and §4; this pass re-derived both,
handler by handler, and they agree exactly — including the `+7*value` register
stride, the `w1[19:17]` `extsel` write, and the 36-entry jump table at
`.rodata+0x2eb0`. Nothing to add there. Two points worth restating here because
they matter to a program *builder* rather than a disassembler:

- **Relocation `value` is a block index, not a register index.** The relocator
  adds `7*value` to the operand, so the driver's per-item constant blocks are
  **7 vec4 registers wide** [CONFIRMED arithmetic, INFERRED interpretation].
- **Only one operand per instruction can be relocated above register 15**,
  because there is a single shared `exthi` field and a single `extsel` selector.
  A generated program that needs two high operands in one instruction cannot be
  expressed; the fragment library must already be written around that.

### 3.3 The fragment-selection tree — `mtx2179` @`.text+0xa2400` [CONFIRMED]

`mtx2179(engine, glstate, key2)` is the recipe generator: it allocates the
program descriptor, walks a decision tree over the state key, and emits one
`mtx1503` call per fragment. It ends with `mtx332` (§4.2), which stamps the key
into the descriptor for the cache.

Fragment indices actually emitted, per generator (extracted mechanically from
every `mtx1503` call site in the module):

| generator | reached when | fragment indices emitted |
|---|---|---|
| `mtx2179` (root) | always | 0x01, 0x02, 0x04, 0x05, 0x4a, 0x4f, 0x5a, 0xb3, 0xb6, 0xbf, 0xc5, 0xcb, 0x159, and `0x65+i` in a loop |
| `mtx2947` (`.text+0xbdc70`, via `0xa24b3`) | main T&L body | 0x08, 0x0b, 0x0c, 0x1f–0x2f (contiguous run), 0x30, 0x31, 0x32, 0x34, 0x37, 0x3b, 0x3c, 0x3e, 0x44, 0x45, 0x48, 0x4e, 0x68, 0x69 |
| `mtx1270` (`.text+0xbf2c0`, via `0xa2742`, when `key2[0x18]==0 && key2[0x1c]==0`) | alternate T&L body | 0x06, 0x10–0x1e (contiguous run), 0x31, 0x32, 0x34, 0x37, 0x3c, 0x3e, 0x44, 0x45, 0x48, 0x4c, 0x4e, 0x0b, 0x0c |
| `mtx2177` (`.text+0xc0380`, via `0xa2502`) | texture stage | 0x41, 0x6e, 0xa7–0xaf |
| `mtx2921` (`.text+0xbd780`, via `0xa280d`) | when `((glstate[8]>>3)&1)==0` or no-lights | 0x06, 0x08, 0x0a, 0x0c, 0x43, 0x44, 0x45, 0x46, 0x48, 0x4e, 0x50, 0x52, 0x54, 0x68, 0x69 |
| `mtx2755` (`.text+0xc0ef0`, via `0xa2950`) | when `glstate[0x2a8]->[0x1b] == 1` | 0xbb, 0xbc (+3 computed) |
| `mtx324` (`.text+0xa4fd0`) variant 0 | vertex-shader path A | 0x153, 0x154, 0x157, 0x158 (+3 computed) |
| `mtx2884` (`.text+0xa5870`) variants 2..6 | vertex-shader path B | 0x131, 0x136, 0x138, 0x13b, 0x13c, 0x13d, 0x13e (+ 9 computed) |

The bands are meaningful: the registry is **organised by function**, with the
lighting fragments the `a*L` relocation lists point at (0x16b–0x1cb, see
`parhelia-vertex-isa.md` §13) sitting in yet another band that none of the
recipes above reaches by a *literal* index — those come from the computed sites
(`lea esi,[reg+imm]`) that this scan could not resolve. So the literal table
above is **complete for constant indices and incomplete for computed ones**
[CONFIRMED as far as it goes].

Some concrete guards read out of `mtx2179` [CONFIRMED]:

| condition | emits |
|---|---|
| `key2[0x10] == 0` (no lighting) | frag 0x01, then the unlit chain, then 0x02 |
| `(key2[0x06] & 0x30) == 0x10` | branch to `0xa2792` |
| `(key2[0x04] & 0x04) && key2[0x48] != 3` | frag 0xc5 |
| `(key2[0x08] & 0x1c) != 0x10` ∧ `(key2[4] & 8) == 0` ∧ (`(key2[4]&0x10)` ∧ lighting ∧ `(key2[8]&0x1c) ∈ {0x0c,0x14}` ∨ `(key2[4]&4)`) | frag 0x4f |
| `glstate[0x2a0]->[0x5c] & 4` | loop `mtx1503(e, 0x65+i, …)` over `glstate[0x2a0]->[0x08]` items |
| lighting ∧ `(key2[8] & 0xe0) != 0` | 7-way jump table `.rodata+0x2fd0` on `key2[8]>>5`, emitting 0xb3 / 0xb6 |
| `key2[4] & 0x20` | frag 0xcb |

`key2[8] >> 5` is a 3-bit selector with a **7-entry** jump table — three bits of
the state byte at `ctx+0xe4` choose among seven lighting-model fragment sets
[CONFIRMED structure, INFERRED meaning].

---

## 4. The program cache and its key

### 4.1 `mtx976` @`.text+0xa6470` [CONFIRMED]

```c
prog *mtx976(engine *e, cache *c, int *rr, void **slots5, void *key1, void *key2)
{
    for (i = 0; i <= 4; i++)                       /* 5 hot slots */
        if (slots5[i] && mtx3930(slots5[i], key1, key2)) { *rr = next(*rr); return slots5[*rr] = slots5[i]; }
    for (p = c->list.head; p != &c->list; p = p->next)   /* LRU list */
        if (mtx3930(p->prog, key1, key2)) { *rr = next(*rr); return slots5[*rr] = p->prog; }

    e->[0x100] = 0;
    clear(e->vec_a0); clear(e->vec_e0); clear(e->vec_c0);   /* mtx1374/3075/2310 */
    prog = mtx2179(e, key1, key2);                 /* build the recipe */
    buf  = new[](e->[0x100]);
    mtx3558(e, prog, buf);                         /* link                */
    prog->[0x02] = 1; prog->[0x20] = buf;
    prog->[0x18] = e->[0x100];
    prog->[0x28] = (e->[0x100]-1)/1536 + 1;
    if (c->count > 0x2a9) mtx2894(e, c, slots5);   /* evict */
    list_push_front(c, prog); c->count++;
    *rr = (*rr + 1 == 5) ? 0 : *rr + 1;
    return slots5[*rr] = prog;
}
```

Cache geometry: **5 round-robin hot slots + an LRU list capped at 0x2a9 = 681
entries** [CONFIRMED].

### 4.2 The key — `ctx+0xdc`, stamped by `mtx332` @`.text+0xa6980` [CONFIRMED]

`mtx332(prog, glstate, key2)` copies the key into the descriptor so `mtx3930`
can compare it later. That copy *is* the field list of the key:

| descriptor | source | width |
|---|---|---|
| `+0xac` | `key2[0x08] & 3` | 2 bits |
| `+0xb0` | `key2[0x10]` | dword |
| `+0xb4` | `(key2[0x08] >> 2) & 7` | 3 bits |
| `+0xb8` | `key2[0x48]` | dword |
| `+0xbc` | `key2[0x04]` | dword |
| `+0xc0` | `key2[0x24]` | dword |
| `+0xc4` | `key2[0x08] >> 5` | 3 bits |
| `+0xc8` | `key2[0x18]` | dword |
| `+0xcc` | `key2[0x1c]` | dword |
| `+0xd0` | `key2[0x20]` | dword |
| `+0xd4[0..1]` | `key2[0x28 + i*4]` | 2 dwords |
| `+0xdc[0..1]` | `key2[0x30 + i*4]` | 2 dwords |
| `+0xe4[0..1]` | `key2[0x38 + i*4]` | 2 dwords |
| `+0xec[0..1]` | `key2[0x40 + i*4]` | 2 dwords |
| `+0xf4` | `glstate[0x2a0]->[0x08]` (item count) | dword |
| `+0xf8[0..n]` | `glstate[0x2a0]->[0x18 + i*0x14]` | per-item dword |
| `+0x108[0..3]` | `key2[0x4c + i*0xc]` | 4 dwords |
| `+0x118` | `glstate[0x0d] & 3` | 2 bits |

So the vertex-pipeline state key is the **~0x58-byte struct at `ctx+0xdc`**,
plus a variable-length per-texture-stage tail taken from the GL state object.
`ctx+0xe0` (key byte 4) and `ctx+0xe4` (key byte 8) are the two bytes that both
the cache key *and* the fixed-function selector of §2.3 read — they are the core
of the pipeline key.

`mtx3930` (`.text+0xa6e60`) is the comparator; it short-circuits on
`key2[0x10]` (lighting) and `prog[0xb0]` first, which is consistent with
lighting being the coarsest split.

---

## 5. Constants and uniforms

Everything below is on the **ICD → kernel command-list** path (`mtx558`
@`.text+0x44ef0`), whose grammar is `u32 opcode; u32 count; <payload>` with
opcodes 0x00–0x51 dispatched through the 82-entry jump table at
`.rodata+0x1b08`. `HW` below is `ctx->[0x30] + 0x78`, the kernel-side hardware
state shadow.

### 5.1 Opcode 0x20 — matrices and vertex-shader constants [CONFIRMED]

Handler `.text+0x458cb`. Record = **0x44 bytes**: `{ u32 slot; float m[16] }`.

**Before anything else the kernel transposes the 4×4 in place**
(`.text+0x45925`..`0x459ac`), swapping dwords `(1,4) (2,8) (3,12) (6,9) (7,13)
(11,14)` — exactly the six off-diagonal pairs. So the ICD ships a matrix in one
order and the hardware wants the other; this is what the live capture saw as a
"reversed 4×4 identity" [CONFIRMED, and it settles the layout question: the
kernel's stored form is the transpose of what the command record carries].

Then a 24-entry jump table at `.rodata+0x1d98` on `slot`:

| slot | destination | dirty bit |
|---|---|---|
| 0x01 | `HW + 0x1f78` = matrix **2** | `HW[0x1bf4] |= 1`; `HW[0x1bec] |= 8` |
| 0x02 | `HW + 0x1ef8` = matrix **0** | `HW[0x1bf0] |= 1` (only if changed) |
| 0x03 | `HW + 0x1f38` = matrix **1** | `HW[0x1bf0] |= 2`, `HW[0xd54] |= 4` (only if changed) |
| 0x04–0x06 | matrix **3, 4, 5** | `HW[0x1bec] |= 8` |
| 0x10–0x17 | matrix **34…41** | `HW[0x1bf0] |= 1 << (slot-0x10+2)` |
| 0x100–0x11f | matrix **2…33** | `HW[0x1bf4] |= 1 << (slot-0x100)`; `HW[0x1bec] |= 8` |
| all others | ignored | |

The store is an array of **42 × 64 bytes** at `HW+0x1ef8`, confirmed
independently by the initialiser `mtx2556` (`.text+0x1bb30`), which loops
`ebp = 0x29` down to −1 = 42 iterations with `rbx += 0x40`. `HW+0x1c50 + i*8`
is a parallel table of 42 *current* pointers, maintained by `mtx3699`
(`.text+0x1b590`), and `HW[0x1ef4]` is the active count.

**Slot 1 and slot 0x100 alias the same matrix (index 2).** `mtx1941`
(`.text+0x13f90`), the emitter, resolves any slot ≥ 0x100 as `slot − 0xfe`,
which is the same mapping [CONFIRMED, independent confirmation].

Reading of the slot numbers [INFERRED]:

| slot | almost certainly |
|---|---|
| 0x02 | the primary transform matrix (index 0), compared-before-store, i.e. the one that changes most |
| 0x03 | the second transform (index 1) — also dirties `HW[0xd54]`, a *setup/raster* block, so this is the one the triangle setup needs too, i.e. the projection/composite |
| 0x04–0x06 | three texture matrices |
| 0x10–0x17 | eight further blocks at the top of the array |
| 0x100+k | **vertex-shader constant registers c[4k..4k+3]** — 32 slots × 4 vec4 = 128 vec4 = exactly the WARP constant file size |

The last row is the strongest inference in this section: 32 slots of 64 bytes
is 128 vec4, and `parhelia-vertex-isa.md` records the WARP register file as 128
entries. Nothing in the kernel names them, so it stays [INFERRED].

**What is NOT resolved:** the final MMIO/ring encoding that pushes
`HW+0x1ef8` into the WARP constant file. `mtx1941` hands each dirty matrix to a
vtable-dispatched consumer object (`rdi+0x378`), and the actual register writes
happen behind that indirection [OPEN]. Experiment: with the vendor stack
running, `glLoadMatrixf` a recognisable matrix (e.g. `1,2,3,…,16`) and diff the
DMA command ring — the sixteen floats must appear verbatim, and the
(register-index, value) pairs that carry them name the constant port.

### 5.2 Opcode 0x21 — lights [CONFIRMED]

Handler `.text+0x458a7` → `mtx227` (`.text+0x46e20`). Record header is
`{ u32 lightId; u32 op }`:

| `op` | meaning | record size |
|---|---|---|
| 0 | bind `lightId+1` to a free hardware light slot | 8 bytes |
| 1 | release the slot holding `lightId+1` | 8 bytes |
| 2 | update parameters | 8 + 0x68 = **0x70 bytes** |

The slot table is `HW + 0x1c20`, **12 dwords** (slots 0..0x0b), holding
`lightId+1` or 0; the active count lives in `HW[0x1be0]` bits **17:14** and is
capped at 0x0c. So the fixed-function pipeline supports **12 hardware lights**
[CONFIRMED]. `HW[0x1bec] |= 4` is the lights dirty bit.

`op == 2` calls `mtx1770` (`.text+0x1b9b0`):

```c
dst = mtx1059(HW + 0x1c08, &out, lightId+1);      /* find the slot's block */
if (!HW->[0]->[0x78]) {
    if (p[0] == 2) p.f[0x64] = cosf(p.f[0x64] * 3.14159274f / 180.0f);
    if (p[0] == 3) { p[0x40] = p[0x34]; p[0x44] = p[0x38]; p[0x48] = p[0x3c]; }
}
if (memcmp(dst, p, 0x68)) { memcpy(dst, p, 0x68); HW[0x1bec] |= 4; }
```

The two float constants were read out of `.rodata.cst4` at offsets 0xc0/0xc4:
**3.1415927** and **180.0** [CONFIRMED]. So:

- `p[0x00]` = **light type**; `2` = spot, `3` = a type whose direction vector is
  duplicated from `p[0x34..0x3c]` into `p[0x40..0x48]`.
- `p[0x64]` = **`GL_SPOT_CUTOFF` in degrees on the wire**, converted by the
  kernel to `cos(cutoff)`. This is exactly the 180.0 the live capture found in
  the `(0x21, N)` record — it is the GL default cutoff, still in degrees,
  because the conversion happens *after* the ICD wrote the record [CONFIRMED —
  this reconciles the capture].
- `p[0x34..0x3c]` and `p[0x40..0x48]` are two vec3 direction slots.
- The remaining 104 − 4 − 4 − 24 = 72 bytes are the colour/position/attenuation
  parameters; 8 consecutive 1.0f in the capture is `GL_LIGHT0`'s diffuse +
  specular default, so those are two vec4s inside the block, but their exact
  offsets were not pinned [OPEN].

**Correction to `docs/parhelia-gl-feature-map.md`:** the record is **28 dwords
(0x70 bytes)**, not 30 — two header dwords plus a 26-dword (0x68) parameter
block. The "30 dwords" reading included the `(0x21, N)` opcode pair.

### 5.3 The other constant-carrying opcodes [CONFIRMED]

| opcode | handler | record | destination |
|---|---|---|---|
| 0x07 | `mtx651` | 1 record | pipeline/shader key |
| 0x08 | `mtx2530` | 0x20 B × N | vertex streams — §6 |
| 0x0c | `mtx2617` | 8 B × N | main register bank, `0x1000 + idx*4` |
| 0x12 | inline @`0x45f5f` | 16 B × N | scissor rect → `HW+0xcd9`; `HW[0xc87] |= 0x10` |
| 0x15 | inline @`0x45d83` | | CLEAR — issues the ring packet directly |
| 0x1b | `mtx2147` | 0x20 B × N | DRAW |
| 0x21 | `mtx227` | see §5.2 | lights → `HW+0x1c20` / per-slot 0x68 blocks |
| 0x22 | `mtx2441` | 36 B × N | — |
| 0x23 | `mtx2046` | 8 B × N | light-model ambient |
| 0x24 | `mtx3554` | 4 B × N | — |
| 0x25 | inline @`0x457c1` | 0x14 B × N: `{u32 idx; float x,y,z,w}`, `idx ≤ 5` | **user clip planes** → `HW + 0x1da0 + idx*16` |
| 0x26 | inline @`0x45727` | 16 B × N: `{x, y, w, h}` | **viewport rect** → `HW+0xcc1`; `HW[0xc87] |= 0x20` |
| 0x27 | inline @`0x4567e` | 8 B: 2 floats | **depth range** → `HW+0x1c00`/`0x1c04`; also `HW[0xd8c] = f0`, `HW[0xd54] |= 4`, `HW[0x1bee] |= 0x40` |
| 0x36 | `mtx1688` | 8 B | line width + point size |
| 0x45 | `mtx3443` | **0x670 B** × N | vertex-program descriptor image (the 1636-byte blob) |
| 0x46 | `mtx3577` | **0x678 B** × N | same, with an 8-byte serial prefix |
| 0x48 | inline @`0x452ff` | **0x18 B** × N | **attribute declarations** — §6 |

Opcode 0x25's `idx ≤ 5` bound is the OpenGL minimum of six user clip planes
[CONFIRMED]; the array is 6 × 16 bytes at `HW+0x1da0`, immediately below the
42-pointer matrix table at `HW+0x1c50`.

Record sizes for 0x45/0x46 match the ICD side exactly (`reserve(0x45,
elemSize=0x670, 1)` + `memcpy(…, shader+0x70, 0x664)` in
`parhelia-vertex-isa.md` §"push-buffer command-record layer") — an independent
confirmation of that reading from the *other* end of the interface.

---

## 6. Vertex attribute → program input binding

Two separate mechanisms, and the distinction matters:

### 6.1 Streams — opcode 0x08 → `mtx2530` → `mtx1450` [CONFIRMED]

`mtx1450` (`.text+0x4c760`):

```c
stream = rec[0x00];                 /* must be <= 7 */
addr   = mtx1132(handle, 0);        /* GPU address of the buffer */
stride = rec[0x10];
HW[0x2978] |= 1 << stream;                       /* stream enable mask */
e = HW + 0x2986 + stream * 0x14;                 /* 5 dwords */
e[0] = 1;  e[1] = addr;  e[2] = stride;  e[3] = handle->[0x10];  e[4] = 0;
HW[0x2979] |= 4;                                 /* dirty */
```

So: **8 vertex streams**, each `{valid, base address, stride, flags}` at
`HW + 0x2986 + stream*0x14`, with an 8-bit enable mask at `HW[0x2978]`.
`ctx[0x118]` caches the stride the ICD announced.

### 6.2 Declarations — opcode 0x48 [CONFIRMED]

Handler `.text+0x452ff`, one 0x18-byte record per attribute:

```c
a = rec[0];  b = rec[1];                          /* both small indices */
memcpy(HW + 0x12ff + b*0x20 + a*0x10, &rec[2], 0x10);   /* 16-byte descriptor */
mtx856(hw2, HW[0x12ef], rec);
HW[0xdbb] |= 5;
HW[0xdbf] |= 1 << (a + b*2);
```

The geometry is unambiguous: stream stride `0x20`, slot stride `0x10`, so
**2 slots per stream × 8 streams = 16 binding descriptors of 16 bytes each**,
and the enable mask `HW[0xdbf]` uses bit `a + 2*b`, i.e. a flat index 0..15.

**Sixteen bindings is exactly the WARP's input-register count** (16 / 256 / 256
/ 12 = input / temp / constant / output, `parhelia-vertex-isa.md` PART 1). The
natural reading is therefore:

> binding index `k = stream*2 + slot` ↔ WARP input register `v[k]`,
> and the 16-byte descriptor at `HW + 0x12ff + stream*0x20 + slot*0x10`
> describes the component type, count and byte offset within the stream.

[INFERRED — the index arithmetic and the count are CONFIRMED; the identification
of index `k` with input register `v[k]` is the inference, and the internal
field layout of the 16-byte descriptor was not decoded.]

Experiment that would settle it and decode the descriptor in one go: with the
vendor stack, draw with a vertex format that adds one attribute at a time
(`glColor` only; then `+glNormal`; then `+glTexCoord`) and diff the 0x48
records the ICD emits. The 16 bytes change in a small, structured way per
attribute, and the prefix-sum offsets already documented in
`parhelia-command-construction.md` give the expected byte offsets to match
against.

### 6.3 Fixed function does not use 0x48

`docs/parhelia-gl-feature-map.md`'s sweep never shows a `(0x48, N)` record for
plain `glBegin`/`glVertex` draws — the fixed-function programs come from the
canned table of §1 and know their own input layout, because the ICD's attribute
order is fixed by construction (the prefix-sum stride table). The declaration
records belong to the **programmable** path (`GL_EXT_vertex_shader` / the
0x45/0x46 descriptor upload) [INFERRED, consistent with the capture's absence].

### 6.4 Registers 0x1100–0x117c [OPEN]

The base state writes indices 0x40–0x5f (registers `0x1100`–`0x117c`) to
`0xffffffff`. Nothing in the kernel's constant-store or declaration paths
touches `HW` offsets that map there, and no literal `0x1100` appears in
`mtx_parhelia.o`. They are written by the ICD as ordinary `(index, value)` pairs
in the opcode-0x0c bank. Their meaning stays unresolved; the "attribute register
file init" reading in earlier docs is a guess. Experiment: clear them one at a
time on a working native draw and observe which attribute stops arriving.

---

## 7. The viewport and depth-range transform

Findings, all [CONFIRMED] unless marked:

- The **viewport rectangle** arrives as opcode `(0x26, N)`, four dwords of
  **screen pixels with Y from the bottom**, and lands at `HW+0xcc1..0xccd` with
  dirty bit `HW[0xc87] bit 5`. It is emitted onward by `mtx273` from `mtx3136`
  (`.text+0x1b680`) at `+0x1b6b8`; `mtx273` is `.text+0x15680`.
- The **depth range** arrives as opcode `(0x27, 1)`, two floats — GL default
  `(0.5, 0.5)`, i.e. already reduced to **(scale, bias)** rather than
  `(near, far)`. It lands at `HW+0x1c00`/`0x1c04`, *and* the first float is
  copied to `HW+0xd8c` with `HW[0xd54] |= 4`. `HW+0xd54` is the same dirty word
  that matrix slot 3 sets — a **setup/rasteriser** block, not the WARP constant
  store.
- Neither the viewport rect nor the depth range is ever written into the 42-slot
  matrix/constant array at `HW+0x1ef8`. **Nothing in the constant path carries a
  viewport scale/bias.**
- The vertex buffer holds **raw object-space `glVertex` values** (two independent
  captures, `parhelia-command-construction.md`), so the transform is not CPU
  side either — except on the software wide-line path, which pre-transforms and
  is a separate code path.

Conclusion: the vertex program produces **clip-space** position, and the
NDC→window transform is applied by the **fixed-function setup/clip unit** from
the viewport rectangle and the depth-range pair, not by the WARP program and not
by the CPU. [INFERRED, but from three converging negatives — no viewport
constant, raw coordinates in the buffer, and the viewport/depth-range values
landing in a raster-side register block.]

The CPU/GPU split, restated for an open driver:

| stage | where |
|---|---|
| object → clip (modelview × projection) | WARP program, from constant matrices (§5.1) |
| lighting | WARP program, kind 4 + the linked lighting fragments |
| user clip planes | setup unit, from `HW+0x1da0` (§5.3) |
| perspective divide | setup unit [INFERRED] |
| NDC → window (viewport scale/bias) | setup unit, from `HW+0xcc1` |
| depth range | setup unit, from `HW+0x1c00`/`0x1c04` and `HW+0xd8c` |
| scissor | setup unit, from `HW+0xcd9` |

---

## 8. What is still open, and how to close it

| open item | experiment |
|---|---|
| The GL meaning of `ctx[0xe0] bit 2` — the fixed-function program selector | Run the vendor stack under the existing `tools/parhelia-glsweep` harness with one GL state toggled at a time and watch which toggle flips the emitted program entry address at `S[0x48]`. The entry dword is visible in the ring; `mtx3605` and `mtx3694` differ by one instruction in length, so their entry points differ. |
| The 16-byte attribute descriptor of opcode 0x48 | §6.2 — incremental vertex formats, diff the records. |
| Whether binding index `stream*2+slot` really is WARP input register `v[k]` | Upload a one-instruction program that copies `v[k]` to an output, vary `k`, read the register file back through the `0x910`/`0x914`/`0x938` port (`parhelia-vertex-isa.md` §11). No rasteriser needed. |
| The MMIO encoding of the constant upload | §5.1 — `glLoadMatrixf` a recognisable matrix, diff the ring. |
| Which of matrix slots 2/3/4-6/0x10-0x17 is which GL matrix | Issue `glMatrixMode(GL_TEXTURE)` + a distinctive `glLoadMatrixf`, and separately `GL_MODELVIEW` and `GL_PROJECTION`, and read the slot number out of the 0x20 record. Cheap and decisive. |
| The interior of the 0x68 light block | Same method: set one `glLight*` parameter to a distinctive value at a time. |
| The computed `mtx1503` fragment indices (`lea esi,[reg+imm]` sites) | Static: emulate the few enclosing loops. There are only 8 such sites. |
| `key2[0x08]>>5`'s 7-way jump table (lighting model) | Toggle `GL_LIGHT_MODEL_LOCAL_VIEWER` / `TWO_SIDE` / `glColorMaterial` and read the emitted fragment indices from a kernel trace. |
| `.LC1`, the float threshold in the §2.1 key build | Read `.rodata.cst4`; trivial, just not done here. |

---

## 9. Corrections this pass makes to existing docs

1. **`parhelia-vertex-isa.md` §12 slot table is shifted by one.** `mtx1824` is
   kind 3 at engine+0x28, not kind 6 at +0x40; +0x98 holds `mtx3032` (4932 B),
   not `mtx2068`. The boot burst is **seven** programs / 2664 bytes / 222
   instructions, not eight / 2868 / 239. The 2868 figure came from including
   `mtx1824` in the burst. (§1.1, §1.3)
2. **`prog[0x28]` is `ceil(size/1536)`**, the number of 128-instruction pages,
   not `(size-1)/12+1`. Verified against the literal 4 that `mtx2051` writes for
   the 4932-byte `mtx3032`. (§1.4)
3. **Kind 17 is a software-WARP image.** `mtx3806` loads +0x98 only when
   `engine[0xa3114] == 1`, the same flag that selects the software emulator.
   It cannot coexist with the hardware set in the 512-instruction store. (§1.3)
4. **`parhelia-gl-feature-map.md`: the `(0x21)` light record is 28 dwords**
   (2 header + 26 payload = 0x70 bytes), not 30. (§5.2)
5. **The "reversed 4×4 identity" in the capture is a transpose, and it is the
   kernel that performs it** — `mtx558`'s 0x20 handler transposes every incoming
   matrix before storing. (§5.1)
6. **The 180.0 in the capture is `GL_SPOT_CUTOFF` in degrees**, at payload
   offset 0x64, converted by the kernel to `cos(cutoff·π/180)`. Both float
   constants read from `.rodata.cst4`. (§5.2)
7. **The viewport transform is neither CPU-side nor in the vertex program.**
   Earlier drafts said "CPU-applied" and then "not CPU-applied"; both were
   incomplete. It is in the setup unit, fed from a raster-side register block.
   (§7)

---

## 10. From-scratch recipe

For an open driver that wants hardware T&L, in dependency order:

1. **Extract the canned programs by symbol name** from `mtx_parhelia.o`:
   kinds 6..12 (`mtx3605 mtx3694 mtx1223 mtx1739 mtx458 mtx3213 mtx2068`,
   2664 bytes) and the 16-entry kind-4 family (`mtx1461`…`mtx3013`, 3648 bytes).
   `tools/parhelia-extract-ucode.c` already resolves `ET_REL` symbols correctly;
   point it at these names. Total 6312 bytes — over the 6144-byte store if you
   load everything, so load kind 4's members on demand.
2. **Load** with the `mtx1391` sequence: full engine-idle poll, then
   `0x918 = byte_cursor/12`, then `0x91c` × 3 per instruction. First load
   `reset = 1`. Record, per program, `entry = 4*(base_instr + entry_off)`.
3. **Enable** the engine: `0x920 = entry_or_flags | 1` (the `mtx656` value).
4. **Select** per draw by writing the cached entry dwords — one dword per
   pipeline stage, using the §2.3 table. Start with the unlit path: kind 6 +
   kind 9 + kind 12 and nothing else.
5. **Constants**: build the 42 × 16-float store, transposed relative to the GL
   column-major convention (§5.1), and push matrix index 0 and 1 first. The
   register→constant binding follows the ×7 relocation stride (§3.2) only for
   *linked* programs; the canned programs have their constant registers baked in
   and must be matched by putting the matrices where they expect them — which is
   the one thing this pass could not read, and which the §8 experiment settles.
6. **Vertex data**: bind stream 0 (`{valid, address, stride, flags}` semantics
   of §6.1) and use the ICD's fixed prefix-sum attribute order. Skip opcode-0x48
   declarations entirely for fixed function.
7. **Setup unit**: program the viewport rect, scissor rect and depth-range pair
   (§7) — the vertex program will not do it for you.

Do **not** replay `tools/warp_ucode.h` (`mtx2785`): it is the register-file
self-test, as established in `parhelia-vertex-isa.md`. It remains useful as a
first sign of life: load it, run it, and read the register file back through
`0x910`/`0x914`/`0x938`.
