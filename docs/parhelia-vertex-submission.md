# Parhelia vertex submission — every way geometry reaches the hardware

How the vendor ICD gets vertices to the WARP unit. Builds on
docs/parhelia-command-construction.md (draw packet, vertex ring, stride table)
and docs/parhelia-gl-feature-map.md; those results are used, not re-derived.

Sources, and what each can prove:

| Source | Weight |
|---|---|
| `matroxdriver-x86_64-cvs-20090511/opengl6/lib64/libGL.so.1.3.0` (disassembly in `scratchpad/libGL64.asm`) | exact code, so encodings and conditions are **CONFIRMED** where the instruction writes the literal |
| `captures/glsweep-0528-prims-formats.txt` — 36-op live sweep on a 0x0528 | **CONFIRMED [HW]**: what the hardware actually received |
| `captures/mtxtrace-5draws.txt` — per-draw buffer diffs | **CONFIRMED [HW]**: per-draw byte cost |

Tags: **CONFIRMED** = the code writes it literally, or the capture shows it (or
both). **INFERRED** = consistent reading, not proven. Addresses are file offsets
in the 64-bit `libGL.so.1.3.0`.

---

## 1. The indexed draw — `0x07` packed 16-bit index lists

### 1.1 Packet format  [CONFIRMED — code + HW]

```
[0x07000000 | count][idx1:idx0][idx3:idx2] ...
```

- Opcode byte `0x07` = `0x03` (DRAW) | `0x04` (bit26 set, bit27 **clear** = index
  list). Bit27 set gives `0x8f` = sequential ranges. The header is always built
  as `0x03000000 | ctx[0xa040] | ctx[0xa044] | count` (`1c0289`, `1c4bb5`).
- **Two 16-bit indices per payload dword, element 0 in the LOW half.**
- **`count` = number of indices − 1**, *not* payload dwords − 1. The builder
  writes the index count into the header's low 16 bits and the flush decrements
  it once (§1.3).
- Indices are **slot-relative**, in exactly the same space as `first`/`last` of
  the `0x8f` packet: vertex *k* is at `align64(slotStart) + k*stride`. The
  decomposer adds `ctx[0xa0dc]` — the batch's base vertex index, i.e. the same
  value that becomes `first` — to every index (`1c5510` adds `r9d`, loaded from
  `movzx r9d, WORD [rbp+0xa0dc]` at `1c4989`). Absolute addressing does not
  exist at this level.

Live proof, `p_lineloop` in `captures/glsweep-0528-prims-formats.txt`
(region 5, slot 0x8000):

```
0x8000: 8f000001   draw ranges, count 1
0x8004: 00000001   first = 1
0x8008: 00000004   last  = 4        (one past end)
0x800c: 07000001   draw packed indices, count 1  -> 2 indices
0x8010: 00010003   idx0 = 0x0003 (= last-1), idx1 = 0x0001 (= first)
0x8014: 00000002   register list, 2 registers
0x8018: 21212425   index tags: 0x25 -> 0x1094, 0x24 -> 0x1090, 0x21 = END
0x801c: 00000006   fence value
0x8020: 1a0d7000   fence address
```

`count = 1` with one payload dword and two indices confirms the
"indices − 1" rule; `2 − 1 = 1`. The closing segment runs *last vertex → first
vertex*, which is the GL_LINE_LOOP closure.

The emitting code (`1c4c95`, and the identical copy at `1c4f39`):

```
mov DWORD PTR [rax],     0x8f000001
mov DWORD PTR [rax+0x4], r13d          ; first
mov DWORD PTR [rax+0x8], r14d          ; last
mov DWORD PTR [rax+0xc], 0x07000001
mov WORD  PTR [rax+0x10], dx           ; dx = last-1   -> low half
mov WORD  PTR [rax+0x12], r13w         ; first         -> high half
add rax, 0x14
```

### 1.2 Alignment and padding  [CONFIRMED]

`1c0240` is the index-space reservation routine, `uint16_t *ReserveIndices(ctx, n)`:

```
1c02bc: lea edx,[r14+r14*1+0x3]   ; 2*n + 3
1c02c1: and edx,0xfffffffc        ; align4
1c02c4: mov WORD PTR [rax],r14w   ; header low16 = n  (rax = header ptr)
1c02c8: mov rax,[rbp+0x20]        ; return = current cursor
1c02cf: lea rdx,[rax+rdx*1]
1c02d3: mov [rbp+0x20],rdx        ; cursor += align4(2n)
```

So the index array is **dword-aligned, padded up to a whole dword**, and for an
odd index count the high half of the final dword is left **uninitialised** — the
header's count, not the payload length, is what bounds the read. A reimplementation
must not assume the pad half is zero, and must not read it.

### 1.3 Where the count comes from  [CONFIRMED]

There is one shared "open draw header" slot per command chunk
(`ctx[0xa034]` = open flag, `ctx[0xa018]` = header address):

- opening writes `0x03000000 | a040 | a044 | 0` (`1c0289`);
- a **range** append does `add DWORD PTR [hdr], 2` then writes `first, last`
  (`1c4be5`);
- an **index** reservation does `mov WORD PTR [hdr], n` (`1c02c4`);
- the flush (`1c0510`) does `dec WORD PTR [hdr]` exactly once (`1c056d`), then
  memcpy's the chunk into the command ring.

Both cases therefore land on *elements − 1*, where the element is a dword for
ranges and a 16-bit index for index lists. A self-contained packet emitted while
another header is already open carries its final count inline (`0x8f000001`,
`0x07000001`) and is not touched by the flush decrement, because `a018` still
points at the older header.

### 1.4 Maximum indices per packet  [CONFIRMED / INFERRED]

| Bound | Value | Source |
|---|---|---|
| header count field | 65536 indices | low 16 bits of the header, count = n−1 [CONFIRMED] |
| command-chunk space | `(0x2000 − 0x24 − used)/2` ≈ **4062** from an empty chunk | `1c47a1`: `if (cursor + 2*nIdx + 0x24 >= base+0x2000) flush` [CONFIRMED] |
| what the decomposer can actually produce | `1.5 × maxVerts` ≤ **1020** | maxVerts ≤ 680 (§5) [CONFIRMED] |

The 0x24 is reserved head-room for the draw header plus the trailing register/fence
packet.

### 1.5 The second, unrelated index mechanism: 32-bit index buffers  [CONFIRMED]

`glDrawElements` does **not** use the packed 16-bit list. It widens every index
to 32 bits into a dedicated staging buffer and submits it by byte range. Three
type-specialised copiers, selected in `d8c80`:

| index type | routine | index load |
|---|---|---|
| `GL_UNSIGNED_BYTE` (0x1401) | `d96c0` | `movzx eax, BYTE [rdx+rdi]` (`d980a`) |
| `GL_UNSIGNED_SHORT` (0x1403) | `da3d0` | `movzx eax, WORD [rcx+rdx*2]` (`da6fc`) |
| `GL_UNSIGNED_INT` (0x1405) | `db0f0` | `mov eax, DWORD [rcx+rdx*4]` (`db41c`) |

Each writes `DWORD [idxbuf + i*4] = index[i] + base` into a ring at
`ctx+0xa470/a480/a490/a4a8`, aligns the cursor to 4, then submits the block as a
`(startByteOffset, endByteOffset | mode)` pair via `1bfe00`, where `mode` (0/2/3)
comes from the buffer's memory class (`1b2040` returns 1/2/4). That low-2-bits
convention is the same one the kernel uses on `0x408 = end|3`
(docs/parhelia-3d-driving.md).

`1bfd70` opens a record for these pairs and **starts a new record once its count
exceeds 0x7ff**, i.e. at most **1024 (start,end) pairs per record**
[CONFIRMED at `1bfd9d`].

For `GL_LINE_LOOP` this path appends one extra 32-bit index equal to the first
(`d9819`: `eax = idx[0]; idx[n] = eax`), which is the same closure the immediate
path does with the `0x07` packet — a different encoding of the same fix.
[CONFIRMED]

**Not claimed:** the wire format the `(start,end|mode)` pairs finally take. They
are byte ranges of a GPU-visible index buffer, but this document does not decode
the descriptor the kernel builds from them.

---

## 2. Two submission paths, and which primitive takes which

### 2.1 The two builders  [CONFIRMED]

| | immediate / small-batch | large vertex-array |
|---|---|---|
| object | `ctx+0x2e8`, command emitter at `ctx+0x318` | `ctx+0x270`, staging rings at `ctx+0xa3f0…a4a8` |
| CPU vertex buffer | `new char[0x2080]`, aligned to 128 (`1c01ea`) | ring, arbitrary size |
| batch ceiling | `0x2000` bytes of vertices **and** `0x2000` bytes of command | none of that shape |
| wire | `0x8f` / `0x07` packets into region 5 at slot `n*0x2000` | region-2 records; **no region-5 traffic at all** |

The capture proves both exist. Every ordinary draw in the sweep writes region 5
(`0x8f000001 …`). The `p_linewidth4` op writes **184 bytes of region 2 and
nothing in region 5**, starting with a record `0x0c000000, 2, 1, 1, 7, 0, 0, 0,
0x507, 0x50f, 0x00180003, 0x104 …` that matches no region-5 descriptor shape.
This settles the "AMBIGUOUS — the submission that carried this geometry is not
fully visible" note in docs/parhelia-gl-feature-map.md §3: it was visible, in a
different buffer, because a different builder emitted it. Its record grammar is
**not decoded here**.

### 2.2 Primitive-by-primitive  [CONFIRMED unless noted]

`ctx[0xa0d8]` holds the **raw GL primitive enum**. The hardware primitive code
`p` (what reaches `reg 0x10b0` and the DRAW record at `ctx[0xa080]`) comes from
the table at **0x369900**:

```
GL enum  0  1  2  3  4  5  6  7  8  9
p        0  1  3  2  4  5  6  7  8  6
```

so `GL_LINE_LOOP(2) -> p=3`, `GL_LINE_STRIP(3) -> p=2`, `GL_POLYGON(9) -> p=6`
(= TRIANGLE_FAN), everything else identity. This confirms the `p` ordering in the
0x10b0 table of docs/parhelia-command-construction.md, which that table could not
distinguish on its own (LINE_STRIP and LINE_LOOP share the value 0x0004203d).

| GL primitive | reaches HW as | CPU work | condition |
|---|---|---|---|
| `GL_POINTS` | `p=0`, one `0x8f` range | none | always |
| `GL_LINES` | `p=1`, one `0x8f` range | none | always |
| `GL_LINE_STRIP` | `p=2`, one `0x8f` range **+ a `[1][0x21212141][a03c<<2]` register packet** | none | reg packet only when `a037 != 0 && a036 == 0` (`1c4cc0`) |
| `GL_LINE_LOOP` | `p=3`, one `0x8f` range **+ a `0x07` packet holding the closing segment** (+ the same register packet under the same condition) | the closing index pair, on the CPU | always (`1c4c33`, `1c4f20`) |
| `GL_TRIANGLES` | `p=4`, one `0x8f` range | none | always |
| `GL_TRIANGLE_STRIP` | `p=5`, one `0x8f` range | none | always |
| `GL_TRIANGLE_FAN` | `p=6`, one `0x8f` range | none | always |
| `GL_POLYGON` | `p=6` (fan), one `0x8f` range | none | always |
| `GL_QUADS` | `p=7` undecomposed, **or** `p=0xb` = a `0x07` indexed **triangle list** | 6 indices per quad | decomposed iff `ctx[0xa0e4]` |
| `GL_QUAD_STRIP` | `p=8` undecomposed, **or** `p=0xc` = a `0x07` indexed **triangle list** | 6 indices per quad | decomposed iff `ctx[0xa0e4]` |
| wide lines (`glLineWidth != 1.0`) | 4 × `GL_LINES`, window-space | full CPU expansion | `lineWidth != 1.0` |
| large points (`glPointSize`) | **no CPU work** — state only | none | — |

`p=0xb` and `p=0xc` both map to `reg 0x10b0 = 0x0004305c` — the plain TRIANGLES
encoding — via tables A@0x66f260 and B@0x66f2a0 (index 11 and 12 both give
A=0x5c, B=3). So a decomposed quad batch is, to the hardware, an ordinary
indexed triangle list. [CONFIRMED]

### 2.3 The quad-decomposition gate, resolved  [CONFIRMED code / INFERRED semantics]

The switch is *not* `a0e4` itself — `a0e4` is derived. The real gate is
`ctx[0xa4f2]`, computed in the state-validation pass at `c7b34`:

```
rax = glstate + 0x9e0
a4f2 = ( state[0x9f6] && state[0x9ec] == GL_FILL  )   /* 0x1b02 */
    || ( state[0x9f7] && state[0x9ec] == GL_LINE  )   /* 0x1b01 */
    || ( state[0x9f8] && state[0x9ec] == GL_POINT );  /* 0x1b00 */
a4f3 =    a4f2 && a4f0 && state[0x9ec] == GL_LINE && state[0x9e0];
a4f0 =    (state[0x9b0] == GL_FLAT);                  /* shade model */
```

Then, when the primitive is `GL_QUADS` or `GL_QUAD_STRIP` and `a4f2` is set,
`ctx[0xa0e4] = 1` and `E[0xa040] = 0x04000000` (bit27 cleared → index mode)
(`c55b0`/`d349c`). Otherwise `a0e4 = 0` and `E[0xa040] = 0x0c000000` (ranges).

**Same flag, second effect — and this closes an open item.** `c7ba3`:

```
if (a4f2) E[0xa044] |=  0x40000000;
else      E[0xa044] &= ~0x40000000;
```

So **draw-header bit 30 is exactly `ctx[0xa4f2]`** — the docs' "bit30 gated on
`ctx+0xa4f2`" is not merely gated on it, it *is* it. [CONFIRMED]

`state[0x9f6..0x9f8]` are three per-polygon-mode enable bytes. Their GL identity
is **INFERRED, not proven**: the shape (three booleans indexed by polygon mode)
fits `GL_POLYGON_OFFSET_FILL/LINE/POINT`, and it is consistent with the reason
the ICD would decompose — the decomposers pick their index order by polygon mode
and shade model, i.e. to get wireframe edges and the flat-shading provoking
vertex right, which a hardware quad primitive cannot deliver. Do not build on
the GL name.

This also explains why the capture shows undecomposed `GL_QUADS`
(docs/parhelia-gl-feature-map.md conflict #2): the sweep enabled none of those
three, so `a4f2 = 0`.

### 2.4 The decomposers  [CONFIRMED]

`1c5510` (QUADS) and `1c5760` (QUAD_STRIP), signature
`(glstate**, nQuads, u16 *out, u32 *pattern, patStep, baseIndex, indexStep)`.
Called with `pattern = {0,1,2,3}` on the stack, `patStep = 0`,
`baseIndex = ctx[0xa0dc]`, and `indexStep = 4` for QUADS (`1c4989`) or `2` for
QUAD_STRIP (`1c47f0`). Six indices out per quad, then `base += indexStep`.

Emitted order, per quad `p = base + {0,1,2,3}`:

| case | GL_QUADS (`1c5510`) | GL_QUAD_STRIP (`1c5760`) |
|---|---|---|
| polygon mode FILL or POINT | `p1 p2 p3  p0 p1 p3` | `p0 p1 p3  p2 p0 p3` |
| polygon mode LINE, `shadeModel == GL_SMOOTH` | `p1 p2 p3  p1 p0 p3` | (matching variant at `1c5829`) |
| polygon mode LINE, `state[0x9e0]` set | `p1 p2 p3  p3 p0 p1` | (matching variant at `1c58a4`) |

The three orderings differ only in which vertex is last per triangle and which
interior edge is shared — the provoking-vertex / wireframe-edge fix. Index count:
`nVerts*3/2` for QUADS (`1c4790`), `nVerts*3 − 6` for QUAD_STRIP (`1c4864`).

The array path decomposes too, into **32-bit** indices, at `dba01`/`dba31`
(reached from `db3f5`/`db400` on `p == 0xb`/`0xc`).

### 2.5 Wide lines  [CONFIRMED code + HW]

Detection, in the same state-validation pass (`c7a80`):

```
lineWidth = state[0xad0 + 4];
sel = (lineWidth != 1.0f) ? 9 : 0;      /* lea eax,[rax+rax*8] */
ctx[0x26c] = ctx[0xa3c8] = ctx[0x2ac] = sel;
```

`0x367a0c` is `0x3f800000` = 1.0f. What selector 9 does is what the capture
shows: `glLineWidth(4.0)` expands **one** line into **four** parallel 1-pixel
`GL_LINES`, with **pre-transformed window-space** vertices at stride 32
(`p_linewidth4`, region 4 at 0xa0e0):

```
0xa0e0: 430ccccd 42e66666 3f000000 3f800000 ...   x=140.8  y=115.2  z=0.5  w=1.0
0xa120: 430ccccd 42e46666 ...                     y=114.2
0xa160: 430ccccd 42e86666 ...                     y=116.2
0xa1a0: 430ccccd 42e26666 ...                     y=113.2
```

8 vertices, each pair spanning x 140.8 → 243.2. This reproduces the earlier
reading exactly, from an independent capture run.

`glPointSize` is **not** CPU-emulated: `p_pointsize` in the same capture emits a
plain `0x8f000001` range with the ordinary 20-byte `[x,y,r,g,b]` stride. Point
size travels only as the `(0x36,1)` two-float record. [CONFIRMED HW]

### 2.6 Bonus: register 0x1104 (index 0x41), resolved  [CONFIRMED]

Listed as an open item ("alternating 0/4 on line-strip/loop draws") in
docs/parhelia-command-construction.md. It is emitted by the line-strip and
line-loop paths only, as a one-register packet, immediately before the draw
packet (`1c4c6e`, `1c4d10`, `1c4f7b`, `1c51de`):

```
[1][0x21212141][ctx[0xa03c] << 2]        ; index 0x41 -> reg 0x1104
xor DWORD PTR [ctx+0xa03c], 1            ; toggles 0 <-> 1 after every emit
```

so the value alternates 0, 4, 0, 4 … per line-strip/loop draw, which is exactly
what the capture showed. It is emitted only when `ctx[0xa037] != 0` and
`ctx[0xa036] == 0`. A per-draw parity bit; what the hardware does with it is not
established here.

---

## 3. The advertised array extensions — all four funnel into the same two paths

`libGL.so.1.3.0` advertises `GL_EXT_compiled_vertex_array`,
`GL_ATI_element_array`, `GL_EXT_element_array`, `GL_EXT_draw_range_elements`,
`GL_EXT_multi_draw_arrays`, `GL_EXT_vertex_array(_object)`,
`GL_ATI_vertex_array_object`.

Every `gl*` export is a thin pointer-to-member trampoline through a per-thread
context. The member pointers are installed by `d13ea`, `SetArrayPathMode(ctx,
mode)`, which stores `mode` in `ctx[0xa868]` and installs one of two tables:

| slot | entry point | mode 1 | mode 2 |
|---|---|---|---|
| +0x1c8 | `glDrawElements` | `d8c80` | `d8c80` (same) |
| +0x1d8 | `glDrawRangeElements` | `d27f0` | `d27f0` (same) |
| +0x1e8 | `glDrawArrays` | `d36c0` | `d3a80` |
| +0x1f8 | `glDrawElementArrayATI` | `d8bc0` | `d3cc0` |
| +0x218 | `glMultiDrawArrays` | `d3330` | `d3fc0` |
| +0x228 | `glMultiDrawElements` | `d4290` | `d4290` (same) |

### 3.1 `GL_EXT_draw_range_elements` — implemented *as* CVA  [CONFIRMED]

`d27f0`:

```
if (end < start) -> GL_INVALID_VALUE (0x501)
glLockArraysEXT(start, end - start + 1);        /* call b8bc8 <glLockArraysEXT@plt> */
this->glDrawElements(mode, count, type, indices);
```

It literally calls the compiled-vertex-array entry point. So
`glDrawRangeElements` is *not* a distinct hardware path — it is a hint that gets
turned into a CVA lock so the ICD stages exactly the referenced vertex range
once, then a normal `glDrawElements`. [CONFIRMED]

### 3.2 `GL_EXT_compiled_vertex_array` — a real staging optimisation  [CONFIRMED / INFERRED]

`glLockArraysEXT` = `15cfa0` [CONFIRMED]:

```
if (first < 0 || count <= 0)  -> GL_INVALID_VALUE  (0x501)
if (already locked)           -> GL_INVALID_OPERATION (0x502)
obj[0x000] = first; obj[0x004] = count; obj[0x230] = 1;
```

`glUnlockArraysEXT` = `15cff0`: `0x502` if not locked, else zeroes all three.

The lock flag is read by every large-array draw
(`d9a4f`, `da75f`, `db47e`, `dbaed`), all guarded by `ctx[0xa868] == 1`, i.e.
only path mode 1 honours CVA. When locked, the draw uses the *lock* range
`(first, count)` from `obj[0]/obj[4]` to size the staging allocation instead of
the per-call range (`d9a5c`…`d9a74`). That the staged copy is then *reused*
across draws rather than re-copied is the obvious purpose but is **INFERRED** —
the reuse cache check was not traced.

### 3.3 `GL_ATI_element_array` / `GL_EXT_element_array`  [CONFIRMED]

`glElementPointerATI(type, ptr)` stores into the array-state object at
`ctx[0xc9b0] + 0x178` (`+0x10` = pointer, `+0x180` = type, `+0x18`/`+0x20` = a
buffer-object name and offset).

`glDrawElementArrayATI` (`d8bc0`) reads those back and calls
`this->glDrawElements(mode, count, storedType, storedPtr)`; if the pointer is
null it resolves the bound buffer object through `15b340` and adds the offset.
`glDrawRangeElementArrayATI` (`d8b00`) does the same into `d27f0`. **No separate
hardware path.**

### 3.4 `GL_EXT_multi_draw_arrays`  [CONFIRMED]

`d3330` / `d3fc0`: validate `mode <= 9` (`GL_INVALID_ENUM` 0x500 otherwise) and
`primcount >= 0` **once**, resolve the primitive code and the decomposition flag
once, then a plain CPU loop:

```
for (i = 0; i < primcount; i++)
    if (first[i] >= 0 && count[i] > 0)
        DrawArraysRange(ctx, mode, first[i], count[i]);   /* d2d70 */
```

with a variant call (`d28d0`) substituted when `a4f2 && a4f3` and the primitive
is QUADS/QUAD_STRIP. The saving is the once-per-call state validation, nothing
more. Each element still produces its own `0x8f` range packet — but they
accumulate into the *same* open draw header, so a multi-draw of N ranges becomes
one draw packet with N `(first,last)` pairs and `count = 2N − 1`. That is the
one place where the "ranges" plural in the packet format is actually exercised.
[INFERRED — the accumulation follows from `1c4be5` + the loop, not observed on HW]

**Summary for question 3: none of the four reaches a different hardware
mechanism.** They all end at either the `0x8f`/`0x07` packets of §1 or the
32-bit index buffer of §1.5.

---

## 4. Vertex reuse — there is none across submissions

**Every submission freshly copies its vertices.**  [CONFIRMED — code + HW]

The flush sequence is identical in all five places it appears (`1c2723`,
`1c28fe`, `1c4887`, `d298ca`-equivalent `d98ca`, `d2e09`, `d30af`):

```
reserve 0x203f in the vertex ring and 0x203f in the command ring   (1b2060)
align both ring cursors up to 64
ctx[0xa078] = vertexCursor - vertexBase        /* offset published to the kernel */
ctx[0xa07c] = cmdCursor    - cmdBase
memcpy(vertexRing, cpuVertexBuffer, align64(nVerts * stride));
ctx[0x10] = 0;                                  /* accumulated vertices reset */
vertexRing.cursor += 0x2000;                    /* unconditional whole-slot bump */
Flush(E, cmdRing, 0x2000);                      /* memcpy the command chunk too */
```

The vertex ring cursor advances by a full `0x2000` whatever the batch held, and
the CPU-side count resets to zero. Nothing addresses a previous slot. The
`mtxtrace-5draws` capture agrees byte for byte: 60 bytes of vertex data per
triangle, slot base 0x2040, 0x4040, 0x6040, 0x8040, 0xa040 — a fresh copy each
time, never a reference back.

**Reuse does exist, but only *within* one submission.** Several draw packets can
sit in one command chunk and index the same vertex block: a state change, a
primitive change, or a `glMultiDrawArrays` element appends another
`(first,last)` pair (or another standalone packet) without re-copying vertices.
`ctx[0xa030]` counts these records, capped at 0x3ff (`1c47bf`, `1c2712`,
`1c24d2`), i.e. **at most 1024 draw records share one 0x2000 slot**.

The only cross-call staging is CVA (§3.2), and that lives in the large-array
rings, not in the 0x2000 slot model.

---

## 5. Batch capacity and splitting

### 5.1 Capacity  [CONFIRMED]

Two numbers per vertex-format change (`c5268`, `d2658`):

```
obj[0x1c] = stride;                 /* from the vertex-format object, +0x6c */
obj[0x14] = 0x2000 / stride;        /* raw capacity */
ctx[0xa0d8] = 0xffffffff;           /* force a primitive re-set */
```

Then `SetPrimitiveClass` (`1c2120`) picks the *effective* capacity `obj[0x18]`
from a lookup keyed `(rawGLprim << 16) | stride`, defaulting to `obj[0x14]`:

| stride | LINES (1) | TRIANGLES (4) | QUADS (7) |
|---|---|---|---|
| 12 | 682 | 681 | 680 |
| 20 | **408** | **408** | 408 |
| 24 | 340 | **339** | 340 |
| 28 | 292 | 291 | 292 |
| 32 | 256 | **255** | 256 |
| 36 | 226 | 225 | 224 |

which is exactly `floor(0x2000 / stride)` rounded **down to a multiple of the
primitive size** (2 / 3 / 4). Only these three classes have entries, because only
they need the rounding; points, strips, fans, loops, polygon and quad-strip use
the raw `0x2000/stride`. The three bolded values reproduce the
(TRIANGLES,24)→339, (LINES,20)→408, (TRIANGLES,32)→255 already recorded in
docs/parhelia-command-construction.md, from the table rather than from a
reconstruction.

`1c21e5` then trims the *current* fill level so the remaining room is an exact
multiple of the primitive size:

```
free = maxVerts - nVerts;
nVerts += (prim == LINES || prim == LINE_STRIP) ? (free & 1) : (free % primSize);
```

### 5.2 What forces a split  [CONFIRMED]

Four independent conditions, checked before each batch grows:

1. `nVerts + newVerts > maxVerts` — the 0x2000 vertex slot is full (`1c270b`, `1c2851`).
2. `cursor + 2*nIdx + 0x24 >= base + 0x2000` — the 0x2000 command chunk is full (`1c47ad`).
3. `E[0xa030] > 0x3ff` — 1024 draw records in this slot (`1c2712`, `1c47bf`).
4. `nVerts + primSize > rawCapacity` at a primitive change (`1c216b`).

In every case: if the command chunk is still empty (`cursor == base`) the split
is free and is skipped; otherwise the full flush of §4 runs.

### 5.3 What must be re-emitted  [CONFIRMED]

`1c2bc0` is the wrap routine. It splits the accumulated run
`ebx = nVerts − ctx[0xa0dc]` into a **complete-primitive part** (emitted now) and
a **carry-over** that is memcpy'd to the base of the fresh CPU buffer:

| GL primitive | complete part | carried to the next batch |
|---|---|---|
| POINTS | all | 0 |
| LINES | `n & ~1` | `n & 1` |
| LINE_LOOP / LINE_STRIP | all if `n >= 2` | **1 vertex** (`1c2efa`: `memcpy(newBase, last, stride)`) |
| TRIANGLES | `(n/3)*3` | `n % 3` |
| TRIANGLE_STRIP | all if `n >= 3` | **2 or 3 vertices** (`1c30f2`) |
| TRIANGLE_FAN / POLYGON | all if `n >= 3` | the fan pivot + the last vertex (`1c304e`) |
| QUADS | `n & ~3` | `n & 3` |
| QUAD_STRIP | `n & ~1` if `n > 3` | `n & 1`, plus the strip's trailing edge (`1c2fea`) |

`test al,0x92` at `1c2c41` (bits 1, 4, 7) is the shortcut for LINES / TRIANGLES /
QUADS — independent primitives, nothing to carry.

After the carry, the routine calls `SetPrim2` (`1c2450`) to re-open the
primitive on the fresh chunk, sets `ctx[0x10] = carried` and `ctx[0xa0dc] = 0`.
Because region 2 is rebuilt from offset 0 on every submission
(docs/parhelia-gl-feature-map.md §2), the split also re-emits **the entire state
stream**, not just the draw header — which is why a split is expensive and why
the ICD sizes chunks with `maxVerts − 6` of head-room (`d2fa4`) rather than
splitting exactly at the boundary.

For `GL_LINE_LOOP` the intermediate chunks must *not* emit the closing segment;
`1c2e75` handles this by substituting a different primitive code for the
non-final chunks.

The oversized-`glDrawArrays` loop (`d2f70`) uses the same machinery from the
outside:

```
chunk = perPrimRound(maxVerts - 6);
while (remaining > 0) {
    k = min(remaining, chunk);
    if (nVerts + k > maxVerts) dst = Wrap(builder, k);   /* 1c2bc0 */
    fetchVertices(dst, first, k);
    first += k; remaining -= k;
}
```

---

## 6. Hard limits on vertices per draw, and where each comes from

| Limit | Value | Origin | Tag |
|---|---|---|---|
| vertices in one draw's slot | `floor(0x2000/stride)` rounded to the primitive size — 682 at stride 12, **408 at stride 20**, 340 at 24, 256 at 32 | the 0x2000 vertex-ring slot and the matching `new char[0x2080]` CPU buffer | CONFIRMED |
| indices in one `0x07` packet | 65536 (header field); ~4062 (chunk space); ≤1020 in practice | 16-bit count field; `0x2000 − 0x24` chunk budget; `1.5 × maxVerts` | CONFIRMED |
| index value | 65535 | indices are 16-bit in the `0x07` packet | CONFIRMED |
| `(first,last)` ranges in one draw packet | 32767 by the count field; 1024 by the record cap | header count is 16 bits, `+2` per range; `E[0xa030] <= 0x3ff` | CONFIRMED |
| draw records per submission | 1024 | `E[0xa030] <= 0x3ff`, entries of 40 bytes at `E+0x18` | CONFIRMED |
| `(start,end)` pairs per array-path record | 1024 | `1bfd9d`: new record once count > 0x7ff | CONFIRMED |
| `first`/`last` themselves | 32-bit | full dwords in the `0x8f` payload | CONFIRMED |

Nothing in the ICD imposes a limit that is obviously a *hardware* register width
other than the 16-bit index and the 16-bit packet count. The 0x2000 figures are
software ring geometry, chosen by the ICD, and a reimplementation is free to
pick a different slot size — but the 16-bit index and the 16-bit count are in
the wire format and are not negotiable.

---

## 7. What a reimplementation needs, in order

1. Choose the vertex format; compute `stride` and
   `maxVerts = roundDown(0x2000/stride, primSize)`.
2. Set the primitive: emit `[1][0x2121212c][0x40000 | (B[p]<<12) | A[p]]` → reg
   0x10b0, but **only** when the old and new `p` fall in the same equivalence
   class of the table at 0x66f2e0 (`{0}{1,2}{3}{4,5,6}{7,8}{9}{10}{11}{12}{13}`);
   across a class boundary emit nothing here (`1c0800`).
3. Write the interleaved vertices into `align64(slotStart) + k*stride`.
4. Emit the draw:
   - sequential: `[0x8f000000 | (2N-1)]` then N × `(first, last)`;
   - indexed: `[0x07000000 | (nIdx-1)]` then `ceil(nIdx/2)` dwords, two 16-bit
     slot-relative indices each, low half first, last dword padded.
5. Append `[2][0x21212425][fenceValue][fenceAddr]` → regs 0x1094 / 0x1090.
6. Publish and let the kernel arm the engine.

The three things easiest to get wrong, in order of how quietly they fail:

- **the count rule differs per opcode** — dwords−1 for `0x8f`, indices−1 for
  `0x07`, and plain N for register lists;
- **index element 0 is the LOW half** of its dword, and an odd count leaves the
  high half of the last dword garbage that the hardware must not read;
- **`0x21` ends a packet, `0x20` pads inside one** — the wrong one produces a
  stream the engine mis-parses rather than an error.

---

## 9. What actually reaches the hardware — SOLVED [HW]

The sections above decode the ICD's *own* buffers, where indices genuinely
exist. This section answers the question those buffers cannot: **what the
command ring receives.** It is the only thing an open driver has to reproduce.

**There is no indexed draw in the ring. Every draw is a byte range of
consecutive vertices, opcode `0x97`, with the memory class in `0x1000`.**

Captured on the 0x0528 with the vendor MTX X driver, one variant per boot
(`tools/capture-gl-matrix.sh <host>` with `ONLY=<variant> WARMUP=1`), reading
the ring pages ending at `0x404` and every secondary section they call:

| variant | GL call | draw packets found |
|---|---|---|
| `idx16` | `glDrawElements(GL_TRIANGLES, 3, GL_UNSIGNED_SHORT, …)` | 6 × `970a0001`, 6 dw/vertex, **3 vertices** |
| `idx32` | `glDrawElements(…, GL_UNSIGNED_INT, …)` | 156 × `970a0001`, 6 dw/vertex, **3 vertices** |
| `varray` | `glDrawArrays(GL_TRIANGLES, 0, 3)` | 158 × `970a0001`, 6 dw/vertex, **3 vertices** |
| `idx16tri8` | `glDrawElements` over 8 triangles, 24 indices | 6 × `970a0001`, 6 dw/vertex, **24 vertices** |

The 16-bit indexed, the 32-bit indexed and the non-indexed draw are
**byte-identical on the wire** — same header, same operand shape, same vertex
count. No `0x07`, `0x8f`, `0x9b` or `0x9f` packet appears in any of them, and
no register carries an index base or an index count.

Three earlier captures already implied this and were not read that way. The
ICD decomposes `GL_QUADS` and `GL_QUAD_STRIP` into an **indexed** triangle list
(§2.2) and appends `GL_LINE_LOOP`'s closing segment as a packed 16-bit index
pair (§1.1), yet `quads-secs.txt`, `quadstrip-secs.txt`, `polygon-secs.txt` and
`lineloop-secs.txt` contain `0x97` range draws and nothing else, at 1, 2, 3 and
4 vertices per packet. The indices are resolved before the stream is built.

**Retracted.** An earlier reading of the packet opcode bits proposed
`0x9b`/`0x9f` as 16- and 32-bit indexed draw variants of `0x97`. That was
inference from bit patterns alone; no binary emits either constant and no
capture contains one. It is wrong.

### 9.1 What an open driver does

`glDrawElements` expands the indices into a linear vertex stream and draws that
range — `p3d_deindex()` in `src/parhelia3d/p3d_index.c`. It costs one copy of
the drawn vertices, which is what the vendor pays too.

`idx16tri8` also settles the batching question: **8 triangles arrive as one
24-vertex packet**, so a draw packet is not limited to a single primitive. The
limit is overlap, not count (docs/parhelia-gears-test.md).
