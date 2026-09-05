# The region-2 record stream: grammar and item layouts

> **SOLVED 2026-09-05.** The framing below supersedes every earlier attempt in
> this repository, including one I declared solved on a single capture and had
> to retract.

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

All 3D state reaches the engine through this stream. The command ring carries
only a draw packet and its fence; everything else -- matrices, lights, combiner
programs, texture state -- is written here as records that the kernel translates
into register writes and opcode-0x41 block loads.

Derived from the vendor ICD and HAL binaries and validated by exact consumption
across 39 captured streams. Where a field is inferred rather than read out of
the binary it is marked.

## Key 0x46 -- the combiner program object, 414 dwords

Parsed by **`mtx2132`** (`.text+0x6e1d0`); `mtx3577` (`.text+0x74160`) is only a
cache lookup over the 0x29a8-byte program object.

    item[0..1]     u64 program id / serial
                   (mtx558 compares id+1 against hwstate+0x12ef to skip
                    redundant uploads)
    item[2]        N = number of ACTIVE combiner blocks
    item[3..372]   10 x 37-dword combiner blocks; only the first N are walked
    item[373]      byte flag -> progobj+0x2a80; ==1 enables the texture-unit pass
    item[374]      texture unit count
    item[375..410] 4 x 9-dword texture-unit descriptors, stride 0x24
    item[411..413] not read on any traced path

The array is dimensioned exactly **10** because `4 + 10*0x94 = 0x5CC` is where
the next field begins -- an eleventh block would overlap it.

    6e387:  mov edx,DWORD PTR [rdx]      ; N = P[0]
    6e389:  add r13,0x4                  ; first block at P+4
    6e3ab:  add r13,0x94                 ; stride 148 bytes = 37 dwords
    6e3b5:  add DWORD PTR [rsp+0x18],0x2 ; hw combiner slot counter += 2 per block

### A block is a combiner-slot PAIR, not a texture stage

Each 37-dword block programs **two** hardware combiner slots, so ten blocks =
**20 combiner stages**, which is what the RGB/ALPHA instruction pairing in
`parhelia-fragment-isa.md` describes. The **four real texture units** are the
separate `0x24`-stride section at item[375..410].

    +0x00  dw 0..12   RGB half, 13 dwords            -> mtx1653(dst+0x158, ...)
    +0x34  dw 13      byte: alpha half separate?
                      if 0 the ALPHA half ALIASES the RGB half (cmove @6e402)
    +0x38  dw 14..26  ALPHA half, 13 dwords          -> mtx2619(dst+0x1c8, ...)
    +0x6c  dw 27      byte, only read when mode == 2
    +0x70  dw 28..31  reserved
    +0x80  dw 32      byte: constant present
    +0x84  dw 33..36  4 floats = stage constant

The constant is converted `x * 1024.0f`, `cvttss2si`, clamped to
`[-1024, +1023]` -- i.e. **Q1.10**, matching the constant encoding already
documented for the fragment ISA. Two independent derivations of the same format.

Observed across captures, which is what pins item[2] and the texture section:

| stream | id | N | item[374] | texdesc[0].dw0 |
|---|---|---|---|---|
| glxgears / clear_color (untextured) | 2 | 1 | 4 | 0 |
| tex_tri_nearest (`glEnable(GL_TEXTURE_2D)`) | 3 | 1 | 4 | **1** |
| matrix_rotate | 5 | 1 | 4 | 0 |

## Key 0x21 -- lights

`mtx227` (`.text+0x46e20`): **op 0 = enable, op 1 = disable, op 2 = set
parameters.** Only op 2 carries a payload (`46f68: add r15d,0x68` = 104 bytes =
26 dwords; item advance 0x70).

There are **12 light slots**, not 8 (`46f92: cmp ecx,0xb; jg` over
`hwstate+0x1c20`), with the active count in `hwstate+0x1be0` bits 14..17.

Proven fields of the 26-dword payload, from `mtx1770` (`.text+0x1b9b0`):

    dw0        light type: 2 = spot, 3 = directional
    dw13..15   direction
    dw16..18   position
    dw25       spot cutoff, in DEGREES as written by the client

The interior float groups (dw1..4, dw5..8, dw9..12) and dw19..21 / dw22..24 are
only `memcmp`/`memcpy`'d wholesale by the kernel. Labelling them diffuse /
specular / ambient / spot-direction is **inference from the GL defaults visible
in the capture, not read from the binary.**

### The kernel writes back INTO region 2

This matters for any replay tool and for the open kernel driver:

* for **type 3**, the kernel copies dw13..15 into dw16..18 **in place**;
* for **type 2**, it rewrites dw25 to `cosf(x * PI/180)` **in place**
  (`mtx3769` is a bare `fcos` wrapper).

So **region 2 is not read-only to the kernel.** An earlier reading of this --
that position appears twice because the client writes both an object-space and
an eye-space copy -- was wrong: the client writes it once and the kernel
duplicates it.

Note that no dword is `K`-flagged inside a light record in the current captures,
but that is *not* evidence against the write-back: every light in them is type 3,
so the cutoff conversion never runs, and both position copies were already equal
so the duplication changed nothing.

## Key 0x0c -- flat state ids

The id is a **flat 32-bit value**, with no 16-bit split anywhere in `mtx2617`'s
4758-byte body. State lives at `hwstate[0x10 + id*4]`, with a 256-bit override
bitmap at `hwstate+0x414`, and an explicit `cmp esi,0xab` feeding a **172-entry
jump table** at `.rodata+0xba0` -- matching the observed id range 0x00..0xAC.

### A decoder self-test, from a latent vendor bug

The bitmap probe indexes `id - 1` (`199c8: lea ecx,[rsi-0x1]`), so a literal
**id 0 would read ~512 MB past `hwstate`** in the vendor driver. Real streams
never contain id 0 -- ids start at 0x07. So **a decoder that starts emitting
key-0x0c id 0 is almost certainly mis-slicing the tail of a preceding
variable-length record.** Cheap to check and worth keeping permanently.


## THE FRAMING RULE (solved, both sides of the ABI)

    record = { u32 key; u32 count; payload }
    size   = 8 + count * itemsize(key)

`itemsize` is a per-key constant baked into the reader's jump table. There is
**no terminator**; the stream ends at a byte length the client supplies
out-of-band in the control page.

**The header is TWO dwords, not four.** Earlier notes here described a 4-dword
header; that was coincidence -- key 0x51's item happens to be 2 dwords, so its
payload looked like header fields. `p[0]` = key, `p[1]` = item count.

That also explains the "count says 13 but 66 pairs follow" contradiction that
falsified my earlier grammar: glxgears' first record is key 0x0d count 1, ten
dwords long, and the *next* record (key 0x0c, count 37) is what carries the
pairs.

### Consumer: `mtx558` @ `.text+0x44ef0`

    44f92: mov eax,[r13+0x0]        ; KEY
    44f96: lea rbp,[r13+0x8]        ; PAYLOAD
    44f9a: cmp eax,0x51
    44f9d: ja  452b8                ; key > 0x51 -> ABORT the whole submit
    44fa5: jmp [rax*8+.rodata+0x1b08]   ; 82-entry jump table

Each case advances by `count = [r13+4]` times its own stride, e.g.
`lea r13,[r13+rax*8+0x8]` for 8-byte items, `imul rax,rax,0x678` for key 0x46.
**The default case aborts** -- an unknown key discards the rest of the stream,
it is not skipped.

### Producer: `alloc_record(builder, key, itemsize, count)` @ libGL `0x1c9c50`

Writes `hdr[0]=key`, `hdr[1]=count`, returns the payload pointer -- **except**
that consecutive allocations with the *same key* merge by bumping the existing
count (`1c9d30: add [rax+0x4],r12d`) rather than opening a new record. That is
why counts get large and why ids inside a key-0x0c record run sorted ascending.

## Control page contract

    +0x000  command = 1 (state submit)
    +0x008  region cookie (u64)
    +0x010  start byte offset into the region (0 in this build)
    +0x230  STREAM LENGTH IN BYTES
    +0x234  status; kernel writes 0xffffffff on entry, leaves it on abort

The kernel bounds-checks `start + len <= region size` before touching anything.

## Key inventory (itemsize in bytes)

| key | item | carries |
|---|---|---|
| 0x00 | 8 | pairs (`mtx2488`) |
| 0x05 | 8 | `(id,value)` pairs, second flat table (`mtx1734`) |
| 0x06 | 8 | pairs, slot word split (see below) |
| 0x07 | 24 | `mtx651` |
| 0x08 | 32 | surface/buffer descriptor |
| 0x0a | 72 | inline ring/fence emit |
| **0x0c** | **8** | **flat render state `(id,value)`**, ids 0x07..0xAC, 172-entry table |
| 0x0d | 32 | stream opener variant A |
| **0x0e** | **8** | **per-texture-unit state**, slot word split |
| 0x12 | 16 | rect -> hwstate+0xcd9 |
| 0x15 | 36 | flush-class |
| 0x18 | 32+tail | |
| 0x19 | 32 | | 0x1a | 24 | | 0x1b | 40 | | 0x1c | 40+tail |
| **0x20** | **68** | **matrices** `{u32 type; float m[16]}`; kernel **transposes in place** |
| **0x21** | **8, or 112 when op==2 — op is the SECOND dword** | **lights**, 12 slots |
| 0x22 | 72 | material (18 dwords) |
| 0x23 | 16 | light-model ambient |
| 0x24 | 8 | light-model flags |
| **0x25** | **20** | **user clip planes** `{idx, x,y,z,w}`, count 6 |
| 0x26 | 16 | rect -> hwstate+0xcc1 |
| 0x27 | 8 | two floats |
| 0x35 | 16 | only the LAST item consumed |
| 0x36 | 8 | only the LAST item consumed |
| 0x37 | 16 | |
| **0x40** | **12 + payload[1] + payload[2]** | inline blob, self-describing |
| 0x41 / 0x42 | 4 (kernel) / 8 (writer) | **writer/reader disagree, unexercised** |
| **0x43** | **8 + payload[1]*16** | constant-range upload `{first,n}` + n vec4 |
| 0x44 | 48 | | 0x45 | 1648 | program object variant |
| **0x46** | **1656** | combiner program object (the 414-dword layout above) |
| 0x47 | 8 | | 0x48 | 24 | | 0x49 | 20 | | 0x50 | 16+tail | | 0x51 | 8 |

Keys in range that ABORT: `01 02 03 04 09 0b 0f 10 11 13 14 16 17 1d 1e 1f
28-34 38-3f 4a-4f`.

## Correction: the slot word split is unit-in-LOW-half

Earlier notes here said an array index sits in the UPPER half of the slot word.
**That is backwards.** For keys 0x0e and 0x06 the kernel reads
`movzx esi,WORD[rbx]` as the **unit** (bounds-checked <= 0xa, only 0..3 used --
the four texture units) and `movzx r8d,WORD[rbx+2]` as the **field**. So
`0x0006_0003` is **field 6 of unit 3**, not slot 6 instance 3.

Key 0x0c ids are a **flat 32-bit** value in a different space entirely.

## Slots pinned by the shading / texturing captures

Three `gltri` traces differing in exactly one GL feature each
(`parhelia-shading-texturing-capture.md`) name four things outright:

| record | field | meaning |
|---|---|---|
| `0x0c` | id `0x09` | shade model: **1 = `GL_FLAT`, 2 = `GL_SMOOTH`** (2 is the context-arm default, so a smooth draw simply omits the id) |
| `0x07` | payload dw0 bit 17 | **texcoord present** in the vertex (`0x10` untextured, `0x20010` textured) |
| `0x08` | payload dw4 | **vertex stride in bytes** (20 untextured, 28 with texcoords); dw2..3 are the vertex buffer's kernel pointer |
| `0x46` | item 375 | **texture unit 0 enable**, 0 -> 1 — the prediction in the table above, now confirmed on a draw verified pixel by pixel |

Also measured there: `0x0e` carries the sampler delta (unit 0 fields `0x00` and
`0x18` only), `0x06` unit 0 field 6 is rewritten with the integer 1000 at texture
bind time, and `0x0a` binds the texture object, carrying its kernel pointer and
its 64x64 dimensions. The `0x05` WARP bind is `(0x13,1) (0x14,2)` for flat,
smooth *and* textured draws — the vertex program is not respecialised, the format
change is entirely `0x07` plus `0x08`.

The context-arm stream enumerates `0x0e` and `0x06` fields for units 0,1,2,3 in
that order, which independently confirms the unit-in-LOW-half split above.

## Correction: key 0x21's op is the second dword (2026-09-05)

The note above described `mtx227` as dispatching on the item's **first** dword.
That is the wrong way round, and a parser built on it walks off the end of any
stream containing a lights record — which is every draw stream.

The item is `{u32 light_index; u32 op}`. Verified against the captures: a
count-24 record is 12 lights x two ops, with the first dword running
`0,0,1,1,2,2…` and the second running `1,2,1,2…`. Only op 2 (set parameters)
carries the 104-byte payload; ops 0 and 1 (enable/disable) are bare 8-byte
items.

With that fixed, `tools/parhelia-recdec.py` consumes three of the four
single-triangle captures **exactly** — every dword of a 1124-dword stream, 21
records, ending on the key-`0x1b` DRAW record:

    gltri-flat-0528-trace.txt      SUBMIT 17  21 recs, dw1124/1124  EXACT
    gltri-gouraud-0528-trace.txt   SUBMIT 17  21 recs, dw1122/1122  EXACT
    gltri-0528-trace.txt           SUBMIT 17  21 recs, dw1124/1124  EXACT

    0x0d 0x35 0x0c 0x12 0x46 0x05 0x26 0x36 0x0c 0x24 0x23 0x0c 0x21
    0x22 0x27 0x25 0x20 0x0c 0x07 0x08 0x1b

The textured capture leaves 11 dwords on its final `0x1b`, one short of the
48 a count-1 DRAW record needs. Either that region's dump is truncated by a
dword or key `0x1b`'s item is 36 bytes rather than 40 in some form; not yet
separated, and it is the only residue left in the grammar.

## Validation

Parsed with `advance = 8 + count*itemsize`:

* `captures/glxgears-0528-state.txt` -- **980/980 dwords, 16 records**
* `captures/ioq3-p690-working-draw.txt` -- **857/857 dwords, 8 records**

Those are truncated dumps, so the decisive check is against complete streams
where the trace also captured the client-declared length at `control+0x230`:

| stream | parse ends at | `control[0x230]` |
|---|---|---|
| mtxtrace-5draws sub 5 | dw1148 | 4592 B = 1148 dw |
| mtxtrace-5draws sub 17 | dw1122 | 4488 B = 1122 dw |
| mtxtrace-textured-submits sub 5 | dw1148 | 4592 B = 1148 dw |
| mtxtrace-textured-submits sub 17 | dw100 | 400 B = 100 dw |

Four exact hits with zero residue, and every one of ~60 record boundaries landed
on one of the 39 keys that have a handler -- any other value would have aborted
the vendor's own parser. Corroborating: key 0x25 count 6 = GL's six clip planes,
key 0x22 items are GL's default material, key 0x21 op-2 items end on 180.0 =
default `GL_SPOT_CUTOFF`, and the rects match each app's window size.
