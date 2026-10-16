# M9/Phoenix `GL_ARB_occlusion_query`: static command and result contract

This note records the statically joined query path in the saved M-Series
Windows driver.  It applies to `PCI\VEN_102B&DEV_0540` (M9/Phoenix), not to
Parhelia-family `0x0527/0x0528` hardware.

The checked files are:

| file | image base | SHA-256 |
|---|---:|---|
| `MSerX/MxMGLX.dll` | `0x10000000` | `d4e04dc371809f079651e9938b285878afd42acabcba2eaabef2e65d218e9f86` |
| `MSerX/MxMD.dll` | `0x00010000` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |

Run `tools/m9-occlusion-query-evidence.py` from the `xf86-video-parhelia`
directory to recheck the hashes, dispatch table, vtables, complete packet-
builder caller census, result/retirement slices, preserved result hashes, and
fixed packet words quoted below.

## OpenGL entry points and restrictions

The entry points are contiguous in `MxMGLX.dll`:

| VA | operation | joined behavior |
|---:|---|---|
| `0x100b94f0` | `GenQueries` | rejects negative `n` with `0x0501`; rejects generation during an active query with `0x0502`; calls `0x10095930` |
| `0x100b9540` | `DeleteQueries` | same two checks; tail-jumps to `0x100959c0` |
| `0x100b9590` | `IsQuery` | rejects zero and searches the manager map at `+0x10` |
| `0x100b95c0` | `BeginQuery` | accepts only target `0x8914` (`GL_SAMPLES_PASSED_ARB`), requires a nonzero name and no active query, flushes the current GL stream, calls `0x10095ab0` |
| `0x100b9630` | `EndQuery` | accepts only `0x8914`, requires an active query, flushes, calls `0x10095b70` |
| `0x100b9690` | `GetQueryiv` | `0x8864` (`GL_QUERY_COUNTER_BITS`) returns `0x20`; `0x8865` returns the active name |
| `0x100b96f0` | `GetQueryObject{iv,uiv}` | supports result `0x8866` and availability `0x8867`; rejects the active object |

The result path waits for availability for `GL_QUERY_RESULT`, calls
`0x10097240`, and then clears the four bytes referenced by manager field
`+0x34`.  When the context's multisample flag at `+0x258:+0x20` equals one,
the returned value is multiplied by the dword at `+0x258:+0x08`.  This is the
ICD's sample-count scaling, after the hardware query values have been summed.

## The four internal MxMGLX records

`0x100030c0` is the internal stream allocator.  It stores an eight-byte header
`{opcode,count}`, coalesces adjacent records with the same opcode, and returns
space for each payload.  Every query record has one four-byte payload: the
driver-private handle stored at query object offset `+0x00`.

| operation | emission site | opcode | payload |
|---|---:|---:|---|
| create | `0x1009715b` | `0x54` | handle |
| begin | `0x10095b23` | `0x55` | handle |
| end | `0x10095b8e` | `0x56` | handle |
| delete | `0x10095a05` | `0x57` | handle |

Create first obtains the handle through `0x10030c70` and stores it at object
`+0x00`.  Begin records the active GL name at manager `+0x28`.  End marks the
object pending (`object +0x0c = 1`), increments the outstanding count through
the pointer at object `+0x14`, and clears the active name.

## MxMGLX aggregation layout

The query-object constructor at `0x10097050` allocates exactly `0x20` bytes at
object `+0x10`.  `0x100970a0` initializes four eight-byte records:

```text
offset +0x00, +0x08, +0x10, +0x18:
    byte  +0  valid
    bytes +1..+3 padding / not consumed by this code
    u32   +4  value
```

The two completion paths at `0x1002e87c` and `0x1002e99b` call updater
`0x100971e0` with a slot index, a 32-bit result, and the four-byte enabled-slot
mask.  The updater stores the value, sets that slot valid, and changes state
from pending to ready (`2`) only when every enabled slot is valid.  It then
decrements the outstanding count.  `0x10097240` sums all valid `u32` values,
clears the four records, and caches the sum at object `+0x18`.

The four records therefore represent up to four enabled driver instances or
heads feeding one OpenGL query.  Static code does not identify them as four
raster pipes, and a new driver should not assume that interpretation.

## Exact MxMD dispatch join

The DP2 parser at `0xd0770` reads `{u32 opcode,u32 count}` and sets the payload
pointer to header `+8`.  At `0xd080d` it translates opcodes through the byte
table at `0xd15c4`; at `0xd0814` it jumps through the dword table at
`0xd14d0`.  The exact entries are:

| opcode | compressed index | handler |
|---:|---:|---:|
| `0x54` | `0x2c` | `0xd1032` |
| `0x55` | `0x2d` | `0xd1059` |
| `0x56` | `0x2e` | `0xd1080` |
| `0x57` | `0x2f` | `0xd1091` |

All four handlers obtain an interface through context vtable slot `+0x1c`.
The outer context vtable is `0x1f4abc`; its slot `+0x1c` is `0x13fd60`, which
returns the embedded object at context `+0xa8`.  The constructor at
`0x13c910` installs query-interface vtable `0x1f49fc` at that address.  This
closes the indirect-call boundary:

| DP2 operation | interface slot | concrete method |
|---|---:|---:|
| create `(handle,9)` | `+0x04` | `0x119650` |
| delete `(handle)` | `+0x0c` | `0x119720` |
| begin `(handle,0)` | `+0x24` | `0x1198b0` |
| end `(handle,1)` | `+0x24` | `0x1198b0` |

The constant `9` is consequently the MxMD query-object kind selected by the
OpenGL create record.  Create allocates a `0x3c`-byte object through
`0x144970`, initializes it at `0x144a80`, and keeps it in a handle-indexed
array.  Begin and end call `0x144b40` with Boolean zero and one respectively.

## Per-query snapshots and result calculation

Each MxMD object owns separate begin and end resources (`+0x10` and `+0x14`)
and their encoded address pairs (`+0x18/+0x1c` and `+0x20/+0x24`).  The four
captured words are:

```text
+0x28  begin snapshot word 0
+0x2c  begin snapshot word 1
+0x30  end snapshot word 0
+0x34  end snapshot word 1
```

`0x144b40` resets the selected pair to `0xffffffff` and queues its encoded
address.  The polling path `0x144d90..0x144e31` obtains an eight-byte result,
copies its two dwords into the selected pair, and marks the query object ready.
For kind `9`, result method `0x1199a0` returns exactly unsigned 32-bit
`end_word0 - begin_word0`.  It ignores word 1.  Kind `0x14`, which is outside
the OpenGL path described here, additionally returns the word-1 delta.

This proves a 32-bit wrapping counter-difference contract and agrees with the
ICD's advertised 32 query-counter bits.  It does not prove that the backing
allocation itself is only eight bytes: the allocation callbacks receive
opaque `(2,2)` and mapping-mode `5` arguments whose units are not named.

## Fixed selector pair and final M9 indexed record

Query begin and end each queue one snapshot address.  `0x119ae0` drains that
queue through `0xfc3a0`, whose per-address helper is `0xfc4b0`.  There are
exactly two direct calls to `0xfc4b0` in `.text` **[BIN]**:

| caller | purpose | literal selector base | encoded pair |
|---:|---|---:|---|
| `0xf9004` | context-switch record | 1 | 1, 2 |
| `0xfc470` | queued query snapshot | 0 | 0, 1 |

The query drain does not obtain, increment, retire, or wrap a per-query slot.
Every OpenGL begin/end snapshot uses selector pair 0/1.  The four-bit fields
inside the two packet words are source selectors, not evidence of a 16-entry
query allocator **[BIN][DER]**.  Query-object reuse allocates fresh snapshot
resources but leaves this selector pair unchanged.

Helper `0xfc4b0` uses six dwords of command-buffer framing and emits this exact
five-dword indexed body (`base` is the literal caller argument):

```text
body dword 0  0xe6e6e61e                     indices 1e,e6,e6,e6
body dword 1  <writeback address, relocation selector 0x3d>
body dword 2  0x64000004 | ((base & 0xf) << 8)
body dword 3  0x64000001 | (((base + 1) & 0xf) << 8)
body dword 4  0x40008000
```

One builder mode explicitly prefixes the body with count `4`; the other moves
the cursor back one dword and reuses generic indexed-packet framing.  The fixed
body words and selector occur at `0xfc596`, `0xfc51f`, `0xfc525`,
`0xfc5c4`, and `0xfc5b6`.  The address is registered through command-buffer
vtable slot `+0x54`; therefore dword 2 is relocation-patched and cannot be
recovered as one fixed literal.  For queries, the selector words are fixed at
`0x64000004` and `0x64000101`.  The first returned snapshot word is joined to
the first payload by ordering, and it is the word differenced for the OpenGL
samples-passed result.  The second word is captured by the second payload but
ignored by OpenGL kind `9`.

## Availability and retirement

Availability method `0x119a20` polls both begin and end snapshots through
`0x144e40` and `0x144d90` **[BIN]**.  If either is pending, it drains the query
queue through `0x119ae0`, forces command progress through `0x119b50`, and
polls again.  `0x144d90` copies exactly two dwords into the query object only
after the snapshot resource reports a completed mapping.  `GL_QUERY_RESULT`
loops until this availability method succeeds before consuming the unsigned
delta **[BIN]**.  Query availability therefore retires both snapshot writes;
it is not inferred from command submission alone.

The four preserved runs are pinned by the checker **[CAP]**:

| run | result | availability before finish |
|---|---:|---:|
| visible | 16384 | 0 |
| partial | 8192 | 0 |
| hidden | 0 | 0 |
| deleted-name reuse | 16384 | 0 |

These runs close ordinary fixed-pair reuse and the asynchronous availability
transition.  They did not drive the physical counter through overflow and did
not use a multisample surface.

This is enough static evidence to construct an experimental hardware query:
allocate two GPU-visible two-dword snapshots, emit this record at begin and
end with the normal M9 relocation machinery, wait for both writes, and subtract
the first end/begin dwords modulo 32 bits.

## What a live query needed beyond the record

The recipe above is necessary but not sufficient.  Building it on the M9128
under the clean-room Mesa driver (`src/gallium/drivers/m9x/m9_query.c`) needed
three further things, each **[MEASURED]** on that card rather than recovered
from the binaries:

1. **The counter is off until the `a7`/`c6` pair names it.**  Emitting only
   the record -- alone in a job, as a begin/end pair alone in one job, or
   bracketing a real draw inside one job -- publishes `0/0` every time.  The
   `a7 = 0x00ffff7f`, `c6 = 0x00ffff09` pair the vendor states in the raster
   range of every query case, and in none of the seven non-query cases of the
   same capture, is what makes the record publish a count.  With the pair a
   100x70 draw reads exactly 7000.  The `a5 = 0`, `d3 = 0` writes and the
   `2e`/`2f` block of the same capture are *not* needed for the count:
   neither alone nor together do they make the counter run, and the count is
   exact without them.

2. **The counter is free running and shared by the context, not reset per
   job.**  A begin snapshot reads a value that grows over a context's life
   (0x1528e, 0x2aa08, 0x3d608, 0x17e896 across consecutive cases of one
   process), so the result is the difference of the two readings and never one
   of them.  This also means a query may span several jobs, provided the
   enable pair reaches each of them.

3. **The closing record has to wait for the draws.**  Emitted in stream order
   behind a draw with no dependency token, it runs while the draw is still
   rasterising and publishes a partial count: 5919, 5578 and 5297 on three
   runs of the same 7000-sample draw.  Framing it the way the vendor frames
   its own -- `e6 = 3, e3 = T` to end the block and raise a token, then
   `43 = T` to wait on it -- makes the reading exact and repeatable.

The kernel side needed the validator to admit index `1e` as a write-only
`QUERY` relocation, the three `e6` literals by position inside that one packet
shape, and the `a7`/`c6` pair by exact value.

## Remaining capture boundary

The final record encoding, query selector use, 32-bit software wrap rule, and
software retirement path are recovered.  The meanings of all subfields inside
`0x64000004`, `0x64000101`, and `0x40008000` remain unnamed.  Static code does
not establish whether the second dword is a second statistic, a completion
value, or both.  A differential capture should verify:

1. the relocated dword-2 address and allocation alignment;
2. that snapshot word 0 changes by the expected visible-sample count while
   snapshot word 1 and the terminal word provide the required completion or
   secondary-statistic behavior;
3. a begin/end pair whose physical counter crosses `0xffffffff`, confirming
   that hardware publication is coherent with the exact modulo-32 software
   subtraction; and
4. one multisample surface, comparing raw snapshot delta with the ICD's exact
   32-bit post-multiply result and the actual samples-passed definition.

Item 2 of that list is now answered **[MEASURED]**: snapshot word 0 changes by
exactly the visible sample count (7000 for a fully visible 100x70 draw, 4130
with an occluder over 41 of its columns, 0 for a hidden one), and word 1 is a
small counter that advances by three per published record rather than a second
sample statistic.  The wrap and multisample checks are still open, and the
allocation alignment is taken from the vendor's own `0x20` quantum.

The M9 Mesa driver advertises `occlusion_query` on the strength of the
measurements above.  A range passing more than 2^32 samples remains outside
what the 32-bit counter can describe, while core Mesa reports 64 for
`GL_QUERY_COUNTER_BITS`.
