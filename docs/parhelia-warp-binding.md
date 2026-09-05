# Parhelia WARP program binding — where the entry point goes, and what runs it

Copyright (C) 2026 René Rebe <rene@exactco.de>

Closes the gap left by `docs/parhelia-vertex-pipeline.md`: program *selection*
was recovered there, but not where the resulting `prog->[0x0c]` dword is
delivered to the hardware. It is delivered as an ordinary **register write in a
DMA `0x2121xxxx` tag packet**, not as a direct MMIO poke and not through
`0x920`.

Source: `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (unstripped),
read as `objdump -d -M intel -r`. Addresses are `.text` offsets. Tags:
**[CONFIRMED]** = read out of the instruction stream; **[INFERRED]** = best
reading, self-consistent, not proven; **[OPEN]** = not determined, with the
experiment that settles it.

---

## 0. Headline — the entry-point destination

The entry dword goes to **register index `0x40`, i.e. MMIO `0x1100`**, written
through the byte-index DMA namespace. [CONFIRMED]

Three independent producers in the object build the identical packet, and one of
them is the boot-time WARP self-test whose packet dwords the repo has already
recorded verbatim:

| producer | `.text` | what it binds |
|---|---|---|
| `mtx693` @`0x436a0` | `0x43748` | the kind-0 program (`mtx1079`) |
| `mtx1814` @`0x43140` | `0x43260` / `0x43214` | kind 3, kind 4[n−1], kind 1, each with a constant block |
| `mtx676` @`0xa0230` | `0xa054e` | the self-test image `mtx2785` |

The shared shape, `mtx693` @`0x43748` [CONFIRMED]:

```
dword0 = 0x00000004                 ; header: register-write list
dword1 = 0x403c3cdd                 ; index bytes, LSB first: 0xdd, 0x3c, 0x3c, 0x40
dword2 = <not written>              ; -> 0x1374   data is a don't-care
dword3 = MODE                       ; -> 0x10f0
dword4 = MODE                       ; -> 0x10f0
dword5 = entry                      ; -> 0x1100   <-- prog->[0x0c]
```

`mtx676`'s variant, `0xa054e`, is the same thing with one more register and a
trailing repeat [CONFIRMED]:

```
[0x00000006][0x41403c3c][MODE][MODE][entry][entry][0x2121213c][MODE]
;            0x3c 0x3c 0x40 0x41                    0x3c
```

so it writes the entry to **both** `0x1100` (index `0x40`) and `0x1104`
(index `0x41`), then re-writes `0x10f0`. `0x41403c3c`, `0x212c1000` and
`0x2121213c` are exactly the self-test dwords `docs/parhelia-vertex-isa.md`
§12 lists without decoding them — they decode cleanly under
`reg = 0x1000 + index*4`, which is independent corroboration that this is the
right namespace and the right index order.

`MODE = (x & 0x8fffffff) | 0x20000000` with `x` seeded `(x & ~0xf) | 8 | 0x20`,
so **`MODE = 0x20000028`** on all three of these paths [CONFIRMED at `0x436ec`,
`0x431b1`, `0xa04d8`].

### What is *not* the destination — two corrections

* **`0x920` does not carry the entry point.** `docs/parhelia-vertex-pipeline.md`
  §1.4 and `docs/parhelia-kernel-abi.md` §9.4 say `mtx656` ORs the WARP-enable
  bit into the entry value and writes it to `0x920`. Re-read at `0xa0e40`:
  `[rsp+0xc]` is initialised to **0**, bit 31 is cleared, bit 0 is set (or
  cleared when `engine[0xa3114] != 0`), and *that byte* is what reaches `0x920`.
  `mtx656` calls `mtx3806` for its side effects only and ignores its result.
  So `0x920 = 1` literally means "WARP enabled", nothing more — which is exactly
  what the mmiotrace shows. [CONFIRMED]
* **`mtx3806` returns nothing useful.** It tail-jumps into `mtx2822`
  (`0x9fdd1`); `docs/parhelia-vertex-isa.md`'s "returns the entry-point value
  written to `0x920`" is wrong. [CONFIRMED]

---

## 1. The register file the WARP is bound through

`reg = 0x1000 + index*4`, indices consumed LSB-first out of the tag dword, index
`0x21` (`0x1084`) ends the tag, index `0x20` (`0x1080`) is the pad — the
convention already CONFIRMED in `docs/parhelia-state-slot-map.md` §5.2,
`docs/parhelia-drm-design.md` §379 and `docs/parhelia-kernel-abi.md` (indices
`dd, af, af, c0` → `0x1374, 0x12BC, 0x12BC, 0x1300`).

Every index below is read directly out of a tag dword the kernel builds.

| idx | MMIO | written by | source value | role |
|---|---|---|---|---|
| `0x00` | `0x1000` | `mtx1814`/`mtx693`/`mtx676` preamble | `0` | pipe preamble |
| `0x10` | `0x1040` | same | `4` | pipe preamble |
| `0x2c` | `0x10b0` | same | `0x0002305c` | primitive type (already documented) |
| `0x2d` | `0x10b4` | same | `0` | pipe preamble |
| `0x2e` | `0x10b8` | same | `0x83c00000` | pipe preamble |
| `0x30` | `0x10c0` | `mtx3400` @`0x43f38` | `S[0x5d8]` | **stage-0 entry** |
| `0x32` | `0x10c8` | `mtx3400` @`0x43f09` | `S[0x5dc]` | **stage-2 entry** |
| `0x33` | `0x10cc` | `mtx3400` @`0x43f12` | `S[0x5e0]` | **stage-3 entry** |
| `0x34` | `0x10d0` | `mtx3400` @`0x43f22` | `S[0x5e4]` | **stage-4 entry** |
| `0x35` | `0x10d4` | `mtx3400` @`0x43f2b` | `S[0x5e8]` | **stage-5 entry** |
| `0x36` | `0x10d8` | `mtx3400` @`0x43f31` | `S[0x5ec]` | **stage-6 entry** |
| `0x3c` | `0x10f0` | everywhere | `MODE` | WARP mode/latch, written 2–3× per bind |
| `0x3e` | `0x10f8` | `mtx3400` @`0x43d89` | `S[0x5a0]` | **entry, shader-object slot 2** |
| `0x40` | `0x1100` | `mtx693`/`mtx1814`/`mtx676`/`mtx3400` | entry | **primary entry point** |
| `0x41` | `0x1104` | `mtx676`; ICD line-strip path | entry / `parity<<2` | companion entry/offset |
| `0x44` | `0x1110` | `mtx3400` @`0x43c5a` | `S[0x61c] \| S[0x608]` | vertex-attribute format |
| `0x45` | `0x1114` | `mtx3400` @`0x43c63` | `S[0x60c]` | vertex-attribute format |
| `0x46` | `0x1118` | `mtx3400` @`0x43c6c` | `S[0x610]` | vertex-attribute format |
| `0x47` | `0x111c` | `mtx3400` @`0x4401b` | `S[0x5f4]` | — |
| `0x48`–`0x4b` | `0x1120`–`0x112c` | `mtx3400` @`0x43c06` | `S[0x5f8..0x604]` | default attribute vector, defaults `0,0,0,1.0f` |
| `0x4c` | `0x1130` | `mtx3400` @`0x43a81`, `0x44017` | `S[0x5f0]` (`\| 0x200`) | WARP pipeline control |
| `0x51` | `0x1144` | `mtx3400` @`0x4402a` | `S[0x618]` | — |
| `0xdd` | `0x1374` | leads every WARP tag | **don't care** | strobe |

Two entries deserve a note.

**Index `0xDD` (`0x1374`) is a strobe.** In `mtx3400`'s two big packets
(`0x43d5b`, `0x43eed`) and in `mtx1814`/`mtx693`, the tag names `0xdd` but the
data dword for it is *deliberately never written* — the ring memory is left
holding whatever was there. `mtx190` in `mtx_drv.so` (`0x5022e`) writes `0` in
the same slot. The 2D side already records `0x1374` as the "soft-trap
companion, always written 0". A register whose payload the kernel does not
bother to initialise before a DMA is a register whose payload the hardware
ignores. [CONFIRMED behaviour; the "strobe" reading is INFERRED]

**Index `0x3C` (`0x10f0`) is written twice, back to back, with the same value,
before every entry write.** `0x10f0` is therefore a port or a double-buffered
latch, not a plain state register. `mtx676` adds a third write *after* the
entry. Bits observed: `[3:0]` = a 4-bit mode selector (8 on every bind-and-run
path; `mtx3654` @`0x8b923` keeps this nibble as a state shadow in `S[0x57c]`),
bit 5 always set, bits `[30:28]` = 2, bit 31 forced 0 except on the
software-WARP cache-invalidate path (`mtx3400` @`0x43ff1`, `mtx1814` @`0x432cc`)
where it is set for one write.

### Namespace caution

`docs/parhelia-draw-recipe.md` §"Default render state, record `0x0c`" lists a
"published reg" column that maps *record-0x0c ids* onto `0x10c0`–`0x10d8` as
stencil operations. That table is explicitly marked unverified there, and
`docs/parhelia-fragment-ops.md` derives the stencil names from the same
record-id namespace, which `docs/parhelia-state-slot-map.md` §5.2 shows is a
**different** namespace (a shadow at `hwmgr + id*4`, flushed as opcode-`0x41`
block loads). The indices in the table above are DMA tag bytes, and for those
`reg = 0x1000 + index*4` is confirmed. If both readings were right the same
MMIO dwords would hold a stencil enum and a WARP entry at once, which they
cannot; the byte-index reading is the one with direct evidence, so the stencil
attribution for `0x10c0`–`0x10d8` should be treated as suspect until the
experiment in §7 is run.

---

## 2. Entry-point value format, and its relation to the `0x918` load position

`mtx1391` @`0x9f6b0`, return path re-read instruction by instruction at
`0x9fb0b`–`0x9fb4a` [CONFIRMED]:

```c
old = engine->[0xa3118];              /* byte write cursor, BEFORE this load  */
engine->[0xa3118] = old + prog->[0x18];
return (old / 3) + 4 * prog->[0x2c + 4*entryIdx];
```

`old` is always a multiple of 12, so `old/3 == 4 * (old/12)`, and `old/12` is
precisely the value written to `0x918` at the start of the burst. Therefore:

```
entry = 4 * ( base_instruction_index + prog->entryOffset[entryIdx] )
      = 4 * ( value_written_to_0x918 + prog->entryOffset[entryIdx] )
```

**The entry point is an instruction index in the same address space as `0x918`,
scaled by 4.** The low two bits are free. [CONFIRMED]

`prog->[0x2c]` is an **11-entry** table of per-variant entry offsets *in
instructions*: the descriptor constructor `mtx376` @`0xa67e0` does
`LocMemSet(prog + 0x2c, 0, 0x2c)` — 0x2c bytes = 11 dwords, indices 0..10, and
the highest index the code ever passes is `0xa` (`mtx3400` @`0x443d9`).
[CONFIRMED]

Consequence for a from-scratch driver: **for all fifteen built-in `.data`
programs the entry-offset table is all zeros**, because only `mtx2051` fills
their descriptors and it sets `+0x18`, `+0x20`, `+0x28` and `+0x4c` — never
`+0x2c[]`. So for a canned program:

```
entry = 4 * (byte offset at which it was streamed) / 12
```

Non-zero entry offsets exist only for the linker output of `mtx3558`, where a
single uploaded image carries several entry points (`mtx3400` calls `mtx2316`
on the *same* buffer `S[0x5b0]` with `entryIdx` 2, 3, 4 and 5 —
`0x43df1`, `0x43e1b`, `0x43e38`, `0x43e5b`). [CONFIRMED]

`mtx2316` @`0x9fc70` is the wrapper that ties the two halves together
[CONFIRMED]:

```c
u32 mtx2316(engine *e, prog *p, u8 bReset, int entryIdx)
{
    u32 v = (e->[0xa3114] == 0) ? mtx1391(e, p, bReset, entryIdx)   /* HW  */
                                : mtx1650(e, p, bReset, entryIdx);  /* SW  */
    mtx3740(p, v);        /* p->[0x0c] = v   (mtx3740 is a one-line setter) */
    return v;
}
```

`mtx3301` @`0xa7450` is `return p->[0x0c];`. So `prog->[0x0c]` is a *cache* of
the last value `mtx2316` returned; the value that actually reaches hardware is
the freshly returned one, and `prog->[0x0c]` exists so `mtx3654` can detect a
change without re-uploading.

**The "disabled" entry value is `2`.** The `S` constructor at `0x4210c`–`0x422b2`
initialises `S[0x598]`, `S[0x59c]`, `S[0x5a0]`, `S[0x5d8]`, `S[0x5dc]`,
`S[0x5e0]`, `S[0x5e4]`, `S[0x5e8]`, `S[0x5ec]` — every one of the nine entry
shadow words — to `2`, and the reset paths at `0x42ae3`/`0x42bf3` set them to
`0`. Real entries are always multiples of 4, so `2` is a value no program can
produce; bit 1 reads as "this stage is not bound". [CONFIRMED that 2 is the
default; the bit-1 semantics is INFERRED]

---

## 3. The 18-entry table — complete kind map

`mtx2475` @`0x9f180` indexes `engine + 0x10 + kind*8`. The slot fillers are all
in `mtx2051`; every assignment below is a relocation read out of the object,
with the blob length taken from `readelf -s`. [CONFIRMED]

| kind | slot | blob | bytes | instr | bound where |
|---|---|---|---|---|---|
| 0 | `+0x10` | `mtx1079` | 168 | 14 | `S[0xb0]` → `mtx693`, run standalone |
| 1 | `+0x18` | `mtx251` | 348 | 29 | `S[0x98]` + const `S[0x88]`/`0x10` → `mtx1814` |
| 2 | `+0x20` | `mtx367` | 300 | 25 | not referenced by any binder found |
| 3 | `+0x28` | `mtx1824` | 204 | 17 | `S[0x20]` + const `S[0x10]`/`S[0x18]` → `mtx1814` |
| 4 | `+0x30` | `mtx1461`…`mtx3013`, 16 descriptors | 120…408 | 10…34 | `S[0x80]` + const `S[0x70]`/`S[0x78]` → `mtx1814`; index = lights−1 |
| 5 | — | unused (`mtx2475` mask `0x39fcf`) | | | |
| 6 | `+0x40` | `mtx3605` | 552 | 46 | `S[0x28]`, entry idx 2 |
| 7 | `+0x48` | `mtx3694` | 564 | 47 | `S[0x28]` alternate (`ctx[0xe0] & 4`) |
| 8 | `+0x50` | `mtx1223` | 240 | 20 | `S[0x38]`, entry idx 2 |
| 9 | `+0x58` | `mtx1739` | 492 | 41 | `S[0x30]`, entry idx 2 |
| 10 | `+0x60` | `mtx458` | 504 | 42 | `S[0x30]` alternate |
| 11 | `+0x68` | `mtx3213` | 192 | 16 | `S[0x40]`, entry idx 2 |
| 12 | `+0x70` | `mtx2068` | 120 | 10 | `S[0x60]`, entry idx 2 |
| 13, 14 | — | unused | | | |
| 15 | `+0x88` | `mtx2655` | 684 | 57 | not referenced by any binder found |
| 16 | `+0x90` | `mtx223` | 120 | 10 | not referenced by any binder found |
| 17 | `+0x98` | `mtx3032` | 4932 | 411 | `S[0xb8]`; software-WARP image only |

Kinds 6+7, 9+10 are the two halves of one either/or choice, so the *seven*
programs `mtx3806` bursts (kinds 6..12, 2664 bytes, 222 instructions) are the
complete canned set — matching what the hardware run already streamed
successfully.

### Which kinds must be bound for a plain transform draw

`mtx3400` @`0x439c0` is the per-draw emitter (`mtx1651` @`0x41d83` calls it with
`rdi = VS + 0x1c0`, which is the `S` struct `mtx3654` and `mtx1881` fill —
`S = VS + 0x1c0` is CONFIRMED by that `lea`). On the **hardware** WARP path
(`engine[0xa3114] == 0`, held in `[rsp+0x1b]`) it does, in this exact order:

1. `S[0x20]` (kind 3) — bind-and-run through `mtx1814`, with a constant block.
2. `S[0x80]` (kind 4[lights−1]) — bind-and-run through `mtx1814`.
3. `S[0xb0]` (kind 0) — bind-and-run through `mtx693`, no constant block.
4. `S[0x98]` (kind 1) — bind-and-run through `mtx1814`.
5. vertex-format packets → `0x1120`–`0x112c`, then `0x1110`–`0x1118`.
6. **re-upload** the canned set: `S[0x28]` with `bReset = 1` (cursor → 0), then
   `S[0x30]`, `S[0x38]`, `S[0x40]`, `S[0x60]`, all `bReset = 0`, all entry idx 2
   (`0x4445c`, `0x4441b`, `0x44441`, `0x443f3`, `0x43ce7`). The returned entries
   land in `S[0x48]`, `S[0x4c]`, `S[0x50]`, `S[0x54]`, `S[0x68]`.
7. upload the shader-object programs `S[0x580]`/`S[0x590]` (entry idx 8 / 0xa)
   and emit `0x1100 ← S[0x598]`, `0x10f8 ← S[0x5a0]`.
8. upload the generated program `S[0x5d0]` with entry idx 6 (`0x442fa`) and
   emit `0x10c0`, `0x10c8`, `0x10cc`, `0x10d0`, `0x10d4`, `0x10d8`.

**The entries of the canned kinds 6..12 are never themselves emitted to a
register.** `S[0x48]`, `S[0x4c]`, `S[0x50]`, `S[0x54]`, `S[0x68]` are written in
step 6 and read nowhere else in the object except `mtx3654` @`0x8c7bf`ff, where
they only feed a change-detection compare against `mtx3301(prog)`. Kinds 6..12
are therefore **resident library code** that the generated program reaches, not
independently bound stages. Their upload order matters because it fixes their
instruction addresses; step 6's `bReset = 1` on the first of them is what makes
those addresses reproducible. [CONFIRMED negative — an exhaustive scan for reads
of those offsets found only the compare]

The six stage registers are always all six written, every flush, whether or not
a stage is in use (they are in one tag, `mtx3400` @`0x43ee6`). On the hardware
path `mtx3400` @`0x442ff` stores **the same** entry — variant 6's — into
`S[0x5dc]`, `S[0x5e0]`, `S[0x5e4]`, `S[0x5e8]`, `S[0x5ec]`, so stages 2..6 all
point at one program. Only on the software-WARP path do they diverge, taking
entry indices 2, 3, 4, 5, 6 out of the same linked image.

So: **unbound stages are not left stale — they are explicitly written, with
`2`.** A driver must write all six.

---

## 4. What makes the WARP execute

There is **no separate start or go register**. [CONFIRMED for the standalone
case, INFERRED for the per-vertex case]

The evidence is `mtx676`, the boot self-test, which is the only place in the
object where a WARP program is bound and then demonstrably run with data whose
result is read back:

```
0xa051a  [3][0x212c1000][0][4][0x0002305c]     ; 0x1000=0, 0x1040=4, 0x10b0=0x2305c
0xa054e  [6][0x41403c3c][MODE][MODE][entry][entry][0x2121213c][MODE]
0xa05da  mtx1937(...)                           ; flush the ring
0xa05f0  loop 0x63..0:
             ring packet header 0x4106000f + 16 dwords of vertex attributes
             poll 0x8f0 until stable
             mtx612 — read the register file back through 0x910/0x914/0x938
```

Bind first, feed vertices second, and the vertices are what makes the program
run. Nothing between the `0x1100` write and the vertex push resembles a trigger.
`mtx693` and `mtx1814` end the same way — the entry write, then `mtx1937`, and
in `mtx1814`'s case an opcode-`0x41` block load of the program's constants
immediately before it.

For the per-primitive case the trigger is the DRAW packet itself. `0x1100` and
the six stage registers are **latched state**: `mtx3400` writes them once per
dirty-state flush, not once per primitive, and the ICD then emits
`[0x8f000001][first][last]` per draw (`docs/parhelia-command-construction.md`,
CONFIRMED from a capture). A per-primitive entry latch is ruled out by
`mtx3400` being reached only from `mtx1651`, the dirty-state validator, which
runs on state change and not on every `glDrawArrays`.

What was **ruled out** as the trigger:

* `0x920` — carries only the enable bit (§0).
* `0x918`/`0x91c` — load address and instruction FIFO; `mtx1391` touches them
  and nothing else.
* a write-back of `prog->[0x0c]` anywhere — `mtx3301` has exactly eleven call
  sites, ten in `mtx3654` (compare only) and one in `mtx3400` @`0x4456d`
  (the kind-17 software image), and none of them reaches MMIO.
* an opcode-`0x41` block-namespace entry — the validators in `0x85000`–`0x8f000`
  mark indices `0x139`–`0x17e` via `mtx2702`, and none of the fifteen
  `mtx2702` calls in `mtx3654` carries a program entry.

The one thing not settled is whether `0x10f0` (index `0x3c`) is a passive mode
word or an active latch: it is written twice before the entry on every path and
a third time after it in the self-test. See §7.

---

## 5. Minimum viable binding — the ordered list

Preconditions: the engine is enabled (`0x920 = 1`), and the seven canned
programs are already in the instruction store — which the hardware run has
already proven works. All packets below go into the bus-master DMA ring
(`docs/parhelia-dma-ring-working.md`: fill, then `0x404 = start`,
`0x408 = end | 3`). `N` below is the value that was written to `0x918`
immediately before streaming the transform program.

1. **Record the load position.** When streaming program `P`, remember
   `N = byteCursor / 12` — the dword you wrote to `0x918`. Its entry is
   `entry = 4 * N` (built-in blobs have an all-zero entry-offset table, §2).
   Do the whole canned burst with `bReset = 1` on the first program only, so
   `N` is deterministic: kind 6 at `N = 0`, kind 9 at `N = 46`, kind 8 at
   `N = 87`, kind 11 at `N = 107`, kind 12 at `N = 123`, next free `N = 133`.
   (The order is `mtx3400`'s: `S[0x28]`, `S[0x30]`, `S[0x38]`, `S[0x40]`,
   `S[0x60]`.)

2. **Pipe preamble, once per flush** — `mtx1814` @`0x434b5`, `mtx676` @`0xa051a`:

   ```
   [0x00000003][0x212c1000][0x00000000][0x00000004][0x0002305c]
   ;             0x00 0x10 0x2c          0x1000=0     0x1040=4     0x10b0=0x2305c
   [0x00000002][0x21212e2d][0x00000000][0x83c00000]
   ;             0x2d 0x2e              0x10b4=0     0x10b8=0x83c00000
   ```

3. **Vertex-attribute format** — `mtx3400` @`0x43bf8` and `0x43c46`:

   ```
   [0x00000004][0x4b4a4948][a0][a1][a2][a3]      ; 0x1120..0x112c, defaults 0,0,0,1.0f
   [0x00000003][0x21464544][f0][f1][f2]          ; 0x1110, 0x1114, 0x1118
   ```

4. **Pipeline control** — `mtx3400` @`0x43a69`:

   ```
   [0x00000001][0x2121214c][ctl]                 ; 0x1130
   ```

5. **Bind the program and light up the stages.** Emit `mtx3400`'s two packets,
   in this order:

   ```
   [0x00000006][0x212121dd][xx][0x3e403c3c][MODE][MODE][entry][2]
   ;             0xdd(strobe)  0x3c 0x3c 0x40 0x3e
   ;                                    0x1100 = entry     0x10f8 = 2 (unbound)

   [0x0000000b][0x212121dd][xx][0x33323c3c][MODE][MODE][e2][e3]
               [0x30363534][e4][e5][e6][e0]
   ;             0xdd        0x3c 0x3c 0x32 0x33   0x34 0x35 0x36 0x30
   ```

   with `MODE = 0x20000028`, `xx` a don't-care dword, and for a single
   transform program `e0 = 2` (stage 0 unused) and
   `e2 = e3 = e4 = e5 = e6 = entry` — which is exactly what the vendor does on
   the hardware path (`mtx3400` @`0x442ff`). Write `2` into any stage you do not
   use; do not leave it stale.

6. **Flush the ring** (`0x404`/`0x408`), then submit the draw:
   `[0x8f000001][first][last]` plus the vertex data. The WARP runs the bound
   program per vertex; nothing further arms it.

The absolute shortest form, if only one program is bound and nothing else is
dirty, is step 2 + one packet:

```
[0x00000004][0x403c3cdd][xx][0x20000028][0x20000028][entry]
```

which is byte-for-byte the packet `mtx693` builds at `0x43748`.

---

## 6. Corrections this file makes to existing docs

| doc | claim | correction |
|---|---|---|
| `parhelia-vertex-pipeline.md` §1.4 | "`mtx656` ORs the WARP-enable bit into the value it finally writes to `0x920`" | `0x920` receives a byte that is 0 or 1; the entry never reaches it (§0) |
| `parhelia-vertex-isa.md` §12 call chain | "`mtx3806` … returns the entry-point value written to `0x920`" | `mtx3806` tail-jumps to `mtx2822`; its result is ignored |
| `parhelia-vertex-pipeline.md` §1.4 | `prog->[0x0c]` = "the hardware entry address" | correct as a value, but it is a *cache*; hardware gets the live `mtx2316` return |
| `parhelia-command-construction.md` §"per-draw command packets" | "Register-list counts are not decremented — count = number of registers" | true only for a single-tag packet. The general rule, from six packets across `mtx3400`, `mtx1651`, `mtx1814`, `mtx676`: **header = (number of payload dwords) − 1**, where payload = tag dwords + data dwords. `[6][tag][d][tag][d][d][d][d]` = 7 payload, header 6 |
| `parhelia-command-construction.md` §"Index 0x21 terminates" | "the next dword after a `0x21` is read as a new header" | inside a register-list packet a `0x21` ends the *tag*; the next dword is a new **tag**, and the header's count says how many payload dwords the whole packet spans (`mtx3400` @`0x43d54` would otherwise parse as opcode `0x3e`) |
| `parhelia-vertex-isa.md` §12 / `parhelia-vertex-pipeline.md` §1.1 | slot table | complete map in §3: kinds 0/1/2/15/16 are `mtx1079`/`mtx251`/`mtx367`/`mtx2655`/`mtx223`, previously unlisted |
| `parhelia-draw-recipe.md` "published reg" column | `0x10c0`–`0x10d8` = stencil | those MMIO dwords are WARP stage-entry registers in the DMA byte-index namespace; see the caution in §1 |

---

## 7. Open items and the experiments that close them

| open | experiment |
|---|---|
| Is `0x10f0` (index `0x3c`) a latch that must be written twice, or is one write enough? | Bind one program with the §5 short form, draw a triangle; then repeat with a single `0x10f0` write, and again with the write omitted entirely. Three runs, one register changed. |
| Does index `0xDD` (`0x1374`) really ignore its data? | Emit the §5 short form twice, once with the strobe dword `0`, once with `0xdeadbeef`. Identical output confirms the strobe reading. |
| `0x10c0`–`0x10d8`: WARP stage entries or stencil ops? | Write a known entry to `0x10d8` via a tag packet, then read back `0x1000`–`0x13fc`. Compare against a run that sets `glStencilOp` through the record path. They must land in different storage. |
| Bit 1 of an entry word = "stage disabled"? | Bind the transform program in stage 6 and write `2` vs. `0` vs. the same entry into stage 5; watch whether stage 5 runs. |
| The `ctl` word for `0x1130` (`S[0x5f0]`) and the mode nibble of `0x10f0` (`S[0x57c]`) | Both are bit-packed shadows built across `mtx3654` (`0x8b923`, `0x8c091`, `0x8c230`, `0x8c4ab`, `0x8c533`) and `0x8655x`. Decoding them is a separate pass; for a first triangle use `MODE = 0x20000028` and sweep `0x1130` from the vendor's value captured with a `mtxtrace` of one draw. |
| `S[0x59c]` — the entry of shader-object slot 1 (entry idx 9) is computed at `0x443ba` but never emitted | Either a dead store or a register emitted by a path not yet read. Low priority; slot 1 is unused on the fixed-function path. |
| Kinds 2, 15, 16 (`mtx367`, `mtx2655`, `mtx223`) have no binder in this object | Likely D3D-only or dead. Not needed for a first triangle. |

None of this has been executed on a Parhelia. The cheapest confirmation is the
§5 short form on top of the instruction-FIFO streaming that already passes its
gates: five dwords into the ring, then the existing triangle submission.
