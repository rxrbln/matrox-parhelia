# M9 program constants: GL-to-MxMD transport

This note closes the static join from OpenGL uniform updates to the private
MxMGLX/MxMD constant-bank handlers. It also corrects an earlier hypothesis:
the GL vertex and fragment constant banks do **not** become entries in MxMD's
64-record sparse queue. They use private opcodes `0x65` and `0x66`, persistent
stage arrays, and the shader-resource/bus-master path instead.

Addresses below are PE32 virtual addresses or ELF text addresses. `[BIN]`
means direct disassembly and `[DER]` means arithmetic derived from it.
`tools/m9-program-constant-evidence.py` checks the important byte ranges.

The checked images are:

| image | SHA-256 |
|---|---|
| `MSerX/MxMGLX.dll` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| Linux `m9x_drv.so` | `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273` |
| Linux `libGL.so.2.0.0` | `42e70d7ab63e5203505da53a44a26fbb71459a9259d79bffa42e1e537150f18b` |

## GL locations and native stage indices

The Windows `glUniform4fv` thunk at MxMGLX `0x100e92b0` dispatches through
context-vtable slot `+0x504`. The concrete table at `0x107f4a70` resolves that
to `0x100b9f70`, which loads the native context from `+0x238` and calls the
linked-program updater. The 1fv, 2fv, and 3fv thunks use the adjacent slots
`+0x4f8`, `+0x4fc`, and `+0x500` [BIN].

`0x10555cd0` proves that a non-negative GL location selects a 64-byte metadata
record in linked-program array `+0x617c..+0x6180`. Common updater `0x10559510`
reads two native indices from that record. `0xffffffff` means absent from the
stage [BIN]:

| metadata | stage | value array | high-water | dirty vector |
|---:|---|---:|---:|---:|
| `+0x24` | vertex | program `+0x410 + 16*index` | `+0x3e4` | `+0x3f0` |
| `+0x28` | fragment | program `+0x2fd0 + 16*index` | `+0x3e8` | `+0x400` |

The stage names are independently fixed by selector `0x1054f020`: context
`+0x54` is the active vertex program when presence byte `+0x61ac` is set, and
`+0x58` is the active fragment program when `+0x61ad` is set. Program retirement
at `0x105660e0` clears `+0x54`'s `+0x3f0` vector and `+0x58`'s `+0x400` vector
[BIN].

The Linux build independently uses metadata `+0x18` for the vertex native
index and `+0x1c` for the fragment native index. Its program layout is
ABI-shifted [BIN]:

| stage | value array | high-water | dirty vector begin/end/capacity |
|---|---:|---:|---:|
| vertex | `+0x338` | `+0x2f8` | `+0x308/+0x310/+0x318` |
| fragment | `+0x2ef8` | `+0x2fc` | `+0x320/+0x328/+0x330` |

The saved Linux `libGL.so.2.0.0` public thunks at `0x76380` and `0x79b10`
preserve `glUniform4fv` and `glProgramEnvParameter4fvARB` arguments and use
Linux context slots `+0x894` and `+0x720`. Equal vtable offsets across the two
ABIs must not be assumed [BIN].

The ARB environment path reaches the same linked update mechanism. Windows
`0x10567100` maps `GL_VERTEX_PROGRAM_ARB` (`0x8620`) to handle `+0x18` and the
20-byte record array at `+0x44`; `GL_FRAGMENT_PROGRAM_ARB` (`0x8804`) maps to
handle `+0x1c` and array `+0x58`. Dependency walker `0x1056b610` reaches
`0x10569170`, which validates one vec4 and calls the common uniform updater
[BIN].

## The staging allocator is not a register allocator

Windows helper `0x105502d0` receives a vec4 count and manages a CPU staging
arena [BIN]:

- context `+0x80` is a page index;
- context `+0x82` is a vec4 cursor within that page;
- one page holds `0x2bc` vec4s and is allocated as `0x2bc0` bytes;
- the return value is `page[page_index] + 16*cursor`;
- allocation advances the cursor by `count` and rolls to the next page when
  `cursor + count > 0x2bc`.

Linux `0x6aaa00` independently implements the same algorithm with page vector
`+0xe8/+0xf0/+0xf8`, page index `+0x100`, and vec4 cursor `+0x102` [BIN].
Neither pair of 16-bit fields carries a native register, sparse key, parity, or
hardware line.

Windows `0x105503b0` resets `+0x80/+0x82` only when the active vertex vector
`+0x3f0` and active fragment vector `+0x400` are both empty. Linux `0x6a9680`
does the same for `+0x308` and `+0x320`, resetting `+0x100/+0x102` [BIN]. This
makes staging lifetime explicit: queued records keep their staged values valid
until both stage queues have drained.

## Exact dirty record

Both vendor builds use a 16-byte range record [BIN]:

```c
struct m9_constant_range {
        uint32_t first_native_index;
        uint32_t vec4_count;
        uint64_t staged_values_ref;
};
```

On 64-bit Linux the last field is the staging pointer consumed at record `+8`.
The 32-bit Windows build materializes an eight-byte private staging reference
through the helper pointer at `0x108a4a24`. It is not a sparse key.

Activation helper Windows `0x105564d0` and Linux `0x6b1530` first discard old
queued ranges, then queue `{0, high_water, staged_full_bank}`. Both process the
vertex bank before the fragment bank. Ordinary Linux update `0x6b1730` and
Windows `0x10559510` use the metadata native index as `first_native_index`, the
changed element count as `vec4_count`, and a newly staged tightly packed vec4
range as the reference. They append vertex first and fragment second when a
uniform is live in both stages [BIN].

Updates compare bytes before staging, so unchanged values create no record.
The queue stores ranges in update order and does not sort by native index.

## Submission batching and order

Windows submission helper `0x100a05e0` provides the complete private protocol
[BIN]:

1. Resolve active vertex program `+0x54` and fragment program `+0x58`.
2. Drain the vertex `+0x3f0` vector in FIFO slices of at most `0x12c` (300)
   records. Each private allocation uses opcode `0x65`, record size `0x10`, and
   the slice count. Copy the records unchanged, flush, and repeat.
3. Reset vertex end to begin.
4. Drain fragment `+0x400` identically with opcode `0x66`.
5. Reset fragment end to begin.

Linux `0x697740` independently proves the same schedule. It computes record
counts as `(end-begin)>>4`, calls the private allocator at `0x5b6f30` with
`(opcode, 0x10, min(remaining, 300))`, applies the slice through `0x5b65c0` or
`0x5b6570`, flushes at `0x5b7110`, and finally tail-calls the staging reset
helper [BIN].

For `N` queued records, the private batch sizes are therefore
`300, 300, ..., N % 300`; a zero remainder adds no batch. Vertex batches always
precede fragment batches. Ranges within each stage retain insertion order
[DER].

## MxMD receives native indices directly

The combined Linux binary exposes the other side of the private interface:

| opcode | wrapper | MxMD handler | destination |
|---:|---:|---:|---:|
| `0x65` | `0x5b65c0` | `0x3b8160` | vertex persistent array at state `+0x91` |
| `0x66` | `0x5b6570` | `0x3b8430` | fragment persistent array at state `+0xba` |

Each wrapper loads record `+0` as `first_native_index`, `+4` as `vec4_count`,
and `+8` as the staged pointer. The handler clamps the range and performs [BIN]:

```c
memcpy(stage_bank + 16 * first_native_index,
       staged_values,
       16 * vec4_count);
```

The old-engine branch, selected when the hardware descriptor's `+8` field is
zero and generation `+0` is at most 5, clamps the vertex bank to indices
`0..255` and fragment bank to `0..223`. The newer branch allows indices
`0..0x2bb` in either handler. These are backend storage capacities; the saved
GL compiler/API advertises only 128 vec4 uniforms per stage [BIN].

Initialization at `0x3b743e` loads the vertex destination pointer from MxMD
object `+0x2500` into state `+0x91`. Initialization at `0x3b7360` loads the
fragment destination from MxMD object `+0x4b0` into state `+0xba` [BIN].

After a non-empty vertex write, `0x3b81ae` increments version `+0x8d` and sets
state byte `+1` bit 0. Fragment `0x3b8480` increments version `+0xb6` and sets
state byte `+0` bit 1. State propagation at `0x3c0f8b` and `0x3c3b39` compares
those versions, copies a changed version, and sets the corresponding bit in the
new state [BIN].

The arrays reach the argument boundary of two distinct shader-resource
builders. Vertex `0x3b8d60` passes state pointer `+0x91` with program fields
`+0x9d/+0xb1` to `0x435b30`; fragment `0x3b8a50` passes `+0xba` with
`+0xc6/+0xda` to `0x4360c0` [BIN]. Passing the pointer is not evidence that
the ordinary creation path uploads its contents. The builder data flow below
fixes that distinction.

## Program resource builders and the constant-bank boundary

The two builders share this leading interface [BIN]:

```c
build(device, mode, handle_out, primary_byte_length, primary_bytes, ...)
```

They search a stage-specific cache by `primary_byte_length` and an exact byte
comparison against the saved primary bytes. A new CPU resource object has the
following statically observed fields [BIN]:

| resource offset | contents |
|---:|---|
| `+0x34` | reference count, initially one |
| `+0x38` | live/busy count used during replacement |
| `+0x40` | primary byte length |
| `+0x48` | separately allocated CPU copy of the primary bytes |
| `+0x60..+0xff` | copied 160-byte compiler result |
| `+0x128` | parsed program object |
| `+0x130` | optional default/dependent-value lists |
| `+0x138` | one caller-supplied program field |
| `+0x140` | optional linked value list on the vertex side |

Vertex uses device cache/list bases `+0x250/+0x1c0/+0x1d0`; fragment uses
`+0x130/+0xa0/+0xb0`. `0x431880` performs the length-and-byte lookup and
increments `+0x34` on a hit. `0x4330c0` registers a new object. The vertex
builder copies the bytes and length at `0x435e79..0x435ebc`; fragment does so
at `0x43643d..0x436493`. These are CPU cache fields, not a GPU address or a BM
descriptor [BIN].

The ordinary wrappers select builder mode 1. In that mode, vertex calls
`0x432370` with program fields `+0xb1/+0x9d`; fragment calls it with
`+0xda/+0xc6`. The passed persistent-bank pointer is not read by either mode-1
branch [BIN]. This rules out the tempting model in which every program create
blindly concatenates the current constant bank.

Exported alternate wrappers `0x3b8df0` (vertex) and `0x3b8ae0` (fragment)
select mode 2. Each can replace its constant-bank argument with null under a
caller flag. Only the mode-2 branch guarded by an additional non-null caller
argument forwards that selected bank to common helper `0x4354d0`; the null
case follows the parser/`0x432510` path without it. If `0x4354d0` returns one,
vertex continues through `0x432800` and fragment through `0x432650` [BIN]. No
direct static call site to either alternate wrapper is present in the saved
ELF image, so its external/callback trigger and the object produced by
`0x4354d0` remain the exact unresolved join.

The guarded helper does expose the CPU compiler's view of that bank [BIN].
When its seventh argument is non-null, `0x4354d0` allocates a `0x60750`-byte
compiler object and a separate `0x78`-byte table stored at compiler-object
offset `+0x60008`.  The table contains 15 pointers:

```text
table[i] = bank_base + i * 0x8000, for i = 0..14
```

Consumer `m9x899` at `0x488da6` selects a table entry and reads one vec4 as
four dwords at `table[selector] + 16 * index`.  It checks `index <= 0xfff`.
Consequently each window covers `0x10000` bytes, adjacent windows overlap by
`0x8000`, and their union spans `0x80000` bytes from `bank_base`.  The consumer
guard admits selector 15 although the constructor creates only 15 entries;
without a proved sixteenth entry or a live selector-15 producer, a new driver
must preserve that discrepancy instead of assuming a 16-entry table.

This proves how mode 2 presents persistent constants to the CPU compiler.  It
does not identify a GPU allocation or relocation for those bytes, nor does it
join the compiler result to final emitter fields `+0xa0/+0xa8`.  The remaining
capture/static target is therefore that result-to-emitter ownership path,
rather than the compiler's source-bank indexing.

Program binding does establish the stage identity without relying on a name.
Vertex handle lookup at `0x4314f0` replays resource `+0x130/+0x140` vec4
records through the already-proved vertex handler `0x3b8160`. Fragment lookup
at `0x431900` replays its lists through fragment handler `0x3b8430` [BIN].
This replay updates the persistent CPU banks; it is not final command-buffer
emission.

## Final relocation-backed resource/control packet

The common old-engine state emitter `0x4b9340` exposes a later, exact GPU
resource boundary [BIN]. Its state contains two opaque 64-bit values at
`+0xa0` and `+0xa8`. They must remain opaque because the saved image does not
join their internal fields to the persistent constant banks above.  Static
constructor and binding recovery proves that the resource emitted at index
`69` is the external fixed-fragment database, not programmable-program or
constant-bank storage; translation from its emitted address remains unresolved
[BIN/DER].

When state byte `+9` bit 1 is set, the emitter first calls command-buffer
vtable slot `+0x68` with `state[+0xa0]`, `state[+0xa8]`, and count one. When
state byte `+1` bit 1 is set, it then emits header `0x45454569` and calls
command-buffer vtable slot `+0xb0` with:

```text
resource_state = &state[+0xa0]
descriptor_word = 2
usage_or_relocation_type = 0x19
count = 1
```

If also dirty, index `0x6a` receives state dword `+0xb0`, followed by index
`0x6d` from state dword `+0xd0`. Thus the proved dirty order is resource track,
relocated `0x69`, plain `0x6a`, then plain `0x6d` [BIN]. The relocation
callback supplies the emitted GPU address; no byte length is read directly by
this emitter.

The alternative inline form takes a CPU pointer at `+0xbe0` and byte length at
`+0xbd8`, emits one 32-byte line at a time through selector index `0x68` and
ports `0x60..0x67`, and advances the selector by one line. That form proves
microcode length and ordering, but it does not prove a constant-bank layout.

The configuration names are present in both saved vendor builds, and their
object placement agrees exactly [BIN]:

| setting | Windows MxMD object | Linux object |
|---|---:|---:|
| `PsBmMicrocode` | `+0x13c` | `+0x240` |
| `PsBmConstants` | `+0x140` | `+0x248` |
| `CeBmMicrocode` | `+0x144` | `+0x250` |
| `CeBmConstants` | `+0x148` | `+0x258` |

Those fields hold setting-reader objects, not the resolved Boolean itself.
Their order proves four independent controls. It does not prove that `Ps` or
`Ce` denotes the GL vertex or fragment stage, nor does it join either
`*BmConstants` reader to the mode-2 bank argument. The similar spelling is not
used as semantic evidence here.

## Why there is no sparse key, parity, or line mapping

MxMD also contains a 64-entry sparse inline-update mechanism. Its record is
`0x1c` bytes with a stored key, four payload dwords, current dirty, and pending
dirty. Windows constructor `0xf6240`, promotion `0x13dabd`, and emitter
`0xf7312` prove [BIN][DER]:

```c
line = (key >> 1) + 0x80;
selector = 0x80000000 | ((line & 0x1ff) << 5);
ports = (key & 1) ? 0x67666564 : 0x63626160;
```

Linux `0x4ba589..0x4ba838` independently matches the 64 records, key
arithmetic, selector header `0x45454568`, even ports `60..63`, odd ports
`64..67`, inline vec4 copy, and dirty clear [BIN].

That sparse mechanism is not the destination of GL program constants:

- `0x65/0x66` preserve `first_native_index` and copy ranges directly into the
  two persistent arrays. They never compute `key >> 1`, parity, or line
  `+0x80`.
- The constant handlers set vertex flag `0x0100` and fragment flag `0x0002` in
  the little-endian state word. The sparse emitter is selected by flag
  `0x8000`, tested separately at Linux `0x4ba589`.
- The central Linux emitter dispatches the program group for
  `state_word & 0x0380` at `0x4ba57d`, then separately scans sparse records if
  byte `+1` has its sign bit set. It clears the groups separately at
  `0x4ba91e..0x4ba92a` and `0x4ba830..0x4ba838`.
- The old constant/program layout and sparse layout occupy a generation-specific
  union. Fragment program pointer `+0xda` would overlap the sparse payload
  interpretation beginning at `+0xd4`; both layouts cannot be live
  simultaneously.

There is consequently no valid function
`stage_native_index -> sparse_key/parity/line` for this GL transport. Assigning
vertex to even keys or fragment to odd keys would mix separate hardware paths.

## Driver guidance and remaining boundary

A compatible M9 implementation can reproduce the now-proved software side:

- keep isolated vertex and fragment vec4 banks;
- accept native range updates as `{first, count, values}`;
- cap old-M9 storage at 256 vertex vec4s and 224 fragment vec4s while exposing
  the compiler's lower 128-vec4 limit unless later evidence changes it;
- preserve range order, submit vertex before fragment, and cap a private-style
  batch at 300 records if that protocol is retained;
- hold staged storage until both queues are empty;
- version the two banks independently.

The persistent-bank destination, update ordering, stage association, resource
builder argument boundary, and final `69/6a/6d` packet shape are now fixed.
Static evidence does not connect the conditional mode-2 bank argument to the
opaque `+0xa0/+0xa8` pair, expose the backing allocation length, or associate
the two `*BmConstants` controls with a GL stage.

The old proposal to search the address-directed index-`69` BAR snapshot for
constant sentinels is retired: that snapshot is an opaque alias rather than a
linear view of the fixed-fragment database.  The remaining probe must compare
the `6c`-selected system record and its surrounding 0x21000-byte allocation
while changing only one stage's constants.  See
`parhelia-m9-program-resource-reconciliation.md` for the exact address join and
bounded VS/FS record experiment.

Run:

```sh
python3 tools/m9-program-constant-evidence.py
```

The checker verifies the hashes, producer records, allocator lifetime,
private batching, recipient bank copies and limits, dirty/version flags,
program-resource builders, configuration-object placement, final relocation
ordering, and the separate sparse packet arithmetic.

## September 2026 live constant-range closure

The follow-up same-program uniform A/B/A capture in
`captures/m9x-linux-gl2-matrix-20260914` closes the ordinary draw-time
constant boundary. Fragment and vertex updates append stage-specific data to
the 0x21000-byte resource at `0x40c3a000`, while raster state binds it through
indices `8c/8d/8e`. Fragment use produces `8c=0x00500400` with an 8d-to-8e
delta of `0x20`; vertex use produces `8c=0x00500200` with a delta of `0x10`.
The preprocessing pass consumes its following range with
`8c=0x00501500` or `0x00501700`, respectively. Full values and the executable
checker are documented in `parhelia-m9-gl2-runtime-capture.md`.

Both passes retain `8f=0` and emit no `90..9f` payload. This runtime evidence
agrees with the static result that normal GL selects the mode-1 resource
builders. It does not supply a trigger for the unreferenced mode-2 wrappers;
that route remains untriggered rather than disproved.
