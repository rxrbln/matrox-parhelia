# M9 linked-pair target placement: native-selector closure

This note closes the control descriptor's symbolic-anchor-to-native-selector
chain and records a negative classification in the saved Linux translator. It
does not infer a selector-`6c` record byte layout. Addresses are virtual
addresses. `[BIN]` marks direct binary evidence and `[DER]` a conclusion from
it.

The pinned inputs are:

* Linux `m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* Windows `MSerX/MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`.

`tools/m9-linked-pair-target-evidence.py` checks both identities, every byte
range and every direct call below without loading vendor code.

## The Linux 0x6d8 object is a per-target translation frame

Linux `m9x25697` at `0x3f6460` allocates
`target_count * (0xdb * 8) + 8` bytes at `0x3f64e2`, constructs objects at a
`0x6d8` stride at `0x3f6520`, and initializes each object through `m9x4836`
at `0x3f6682` [BIN].

The call argument and store sequences give this exact pointer map [BIN]:

| frame field | input |
|---:|---|
| `+0x30` | pipeline outer `+0x140` |
| `+0x38` | pipeline outer `+0x130` |
| `+0x40` | pipeline outer `+0x138`, the linked state |
| `+0x48` | per-target backend object |
| `+0x50` | another per-target input object |

`m9x919` selects `owner+0x30 + target_index*0x6d8` at `0x3f6224` and calls
`m9x5672` at `0x3f6231`. That helper calls `m9x11630` at `0x3f1443`.
`m9x11630` reads frame `+0x48`, selects backend `+0x728` at
`0x3ef9fc..0x3efa0d`, and reads the linked-state source from frame `+0x40` at
`0x3efa7f` [BIN].

The 0x6d8 object is therefore a host-side per-target translation frame joining
the linked state to a backend destination [DER]. It is not evidence for a
0x6d8-byte GPU-visible selector-`6c` record, a VS/FS split, or an instruction
offset. This removes a tempting false serializer candidate [DER].

## REP and LOOP carry a source anchor

The Windows REP lowerer at `0x1a7970` computes node `+0x24` at
`0x1a7a56..0x1a7a66` as [BIN]:

```text
anchor = ([token_argument + 0x2c]->dword_04 >> 1) & 0xffff
```

The LOOP lowerer at `0x1a7f20` performs the identical transform at
`0x1a8006..0x1a8016`, using token argument `+0x30` [BIN]. The exact semantic
unit of the source value before the shift remains unnamed; the recovered
value is called the **source anchor** here.

When compiler context `+0x270` is zero, both lowerers call scanner `0x1a6f70`
with the anchor. The scanner walks the current graph and returns the number of
type-1 and type-2 nodes whose `+0x24` equals that anchor. The result becomes
the new node's `+0x28` [BIN]. When context `+0x270` is nonzero, the lowerers
store `0xffffffff` at node `+0x28` instead [BIN]. Thus `+0x28` is an exact
same-anchor multiplicity or a disabled-scan sentinel, not an arbitrary
predecessor or native PC [DER]. The scanner body at `0x1a6f70..0x1a700a` has
SHA-256 `78c0da0ba8629c28f95644ff9dfdd5ae563ae6b8b41c2462389b770d8486f79a`.

Both lowerers then pass the same four cdecl arguments to builder `0x1ba2c0`
[BIN]:

```text
builder(compiler_context, anchor, context_flag, same_anchor_multiplicity)
```

## The builder preserves the anchor in typed descriptors

Builder `0x1ba2c0` snapshots `compiler_context+0x59c` and increments that
serial at `0x1ba2c8..0x1ba2d9` [BIN]. Let `A` be the 16-bit source anchor and
`S` the old serial. The descriptor fields written before object publication
are [BIN]:

| multiplicity | fixup object | descriptor fields |
|---:|---:|---|
| zero | 0 | `+0x14 = 0x94000000+A`; `+0x1c = 0xc4000000+A` |
| nonzero | 0 | `+0x14 = 0x94000000+A`; `+0x1c = 0xb4000000+S` |
| nonzero | 1 | `+0x14 = +0x1c = 0xc4000000+A` |
| nonzero | 2 | `+0x14 = +0x1c = 0xb4000000+S` |
| nonzero | 3 | `+0x14 = +0x1c = 0xc4000000+A`; `+0x18 = 0xb4000000+S` |

The zero-multiplicity sequence is at `0x1ba4fa..0x1ba520`; the nonzero
sequences are at `0x1ba565..0x1ba6f5` [BIN]. These are 0x20-byte compiler
fixup objects. They must not be decoded as packed two-dword native
instructions [DER].

This proves that the source anchor survives lowering as the low 16 bits of
typed `0x94` and `0xc4` descriptor families. Only the `0xb4` family uses the
new compiler serial [DER]. A driver implementation must not substitute the
serial for the branch target.

## Late mapping replacement and modifier preservation

The later transform at `0x1cef80` walks source descriptors and a table of
0x1c-byte mapping entries. At `0x1cf00b..0x1cf03c`, an entry matches when
[BIN]:

```text
(entry.key & 0xffff) == (source & 0xffff)
((entry.key ^ source) & 0xf0000000) == 0
```

On a match it reads `entry+0x04` as the replacement and performs the exact
merge at `0x1cf040..0x1cf055` [BIN]:

```text
resolved = replacement ^ ((replacement ^ source) & 0x0c000000)
```

It writes the result back to the same ordinal through helper `0x1a2b10` at
`0x1cf061` [BIN]. Therefore the replacement supplies every bit except 27:26;
those two bits remain from the source descriptor [DER].

## Mapping dword 1 is the paired physical-converter output

Windows compiler pass `0x1d1d40` keeps two same-length dword arrays. At
`0x1d2c31..0x1d2c47` it obtains source descriptor `D` from ordinal lookup
`0x1a2e60` and writes `D` to the first array. It then passes the same `D` to
physical converter `0x1df900` at `0x1d2c5d`, with the corresponding second
array element as the output pointer [BIN]. Thus the arrays are paired by graph
ordinal [DER]:

```text
source[i]      = graph_descriptor(i)
replacement[i] = physical_convert(source[i])
```

The publication loops at `0x1d341d..0x1d343f` and
`0x1d3508..0x1d352a` read the same ordinal from both arrays and call
constructor `0x1a2ae0`. The constructor writes its first argument to mapping
dword 0, its second argument to dword 1, and zeros the remaining four dwords
[BIN]. The 0x18-byte value is appended in a 0x1c-byte vector element. The
completed table reaches transform `0x1cef80` through its only compiler call at
`0x1d3579` [BIN]. Therefore the previously unnamed `entry+0x04` replacement is
exactly the physical converter's output for `entry+0x00`, not a guessed byte
PC or external-record offset [DER].

The saved Linux compiler independently proves the same relationship. In
`m9x14892`, `0x532a47..0x532a71` obtains a source descriptor and writes the
first dword array. Call `0x532a94` sends that descriptor to converter
`m9x17578` (`0x4c30f0`) with the same-ordinal element of the second array as
its output [BIN]. At `0x532560..0x5325cf`, the publication loop reads the two
arrays, writes them as dwords 0 and 1 of a zero-filled 0x1c-byte record, and
appends it [BIN]. Transform `m9x7815` applies the same replacement while
preserving source bits 27:26 at `0x527fd0..0x527ff4` [BIN]. This is an
independent 64-bit build join for the producer and consumer [DER].

## The REP/LOOP families allocate finite native selectors

Converter `0x1df900` dispatches on the descriptor high nibble. The three
families emitted by builder `0x1ba2c0` have exact finite allocation rules
[BIN]:

| family | Windows allocator | allocation and replacement |
|---:|---:|---|
| `0x94` | `0x1df0c0` | reuse a matching 0x1c-byte cache entry; otherwise decrement 16-bit context `+0x0a` and return `(source & 0xffff0000) | (new_counter + 0x20)`, selecting `0x2f..0x20` |
| `0xb4` | `0x1e37c0`, subcontext `+0x3c0` | reuse or allocate one of four indices `i=0..3`; return `(source & 0xfc000000) | (i + 4)` |
| `0xc4` | `0x1e2f50`, subcontext `+0x250` | reuse or allocate one of 23 indices `i=0..22`; return `(source & 0xfc000000) | (i + 8)` |

All three match an existing allocation with mask `0xf000ffff`. Initializer
`0x1de2d0` sets the type-9 counter to 16 at `0x1de2ed`, proving its sixteen
low selectors are `0x20..0x2f`; exhaustion returns descriptor sentinel
`0xf0000000` [BIN]. The type-9 miss path also appends
`{source, replacement, 0, 0, 0, 0}` to the 0x1c-byte cache at context
`+0x424`; the four-slot and 23-slot allocators keep their source descriptors,
used masks, lifetime flags and 16-bit owner values in their respective
subcontexts [BIN]. These are allocation identities with explicit reuse and
failure. Their low values are not compiler serials after conversion [DER].

Linux high-level dispatcher `m9x17578` independently sends type 9 to
`m9x7929`, type B to `m9x21980`, and type C to `m9x28923` [BIN]. The type-9
miss path at `0x4bd9e8..0x4bda15` has the same decrement and `+0x20` formula;
initializer `0x4c0c50` writes the same counter value 16 at `0x4c0c81` [BIN].
The B and C output paths at `0x4bd5f5..0x4bd606` and
`0x4bd165..0x4bd176` preserve `source & 0xfc000000` and add 4 or 8 to the
allocated index [BIN]. The different structure offsets reflect the 64-bit
layout, while the returned descriptor formulas match Windows [DER].

## Resolved selectors become native instruction bits

After transform `0x1cef80` has substituted each paired physical descriptor,
the type-3 resolver described in
`parhelia-m9-shader-transport-static-closure.md` feeds the resolved values to
direct serializer `0x1dc8b0`. Its final insertions are [BIN]:

| gate | resolved object field | native slot insertion |
|---:|---:|---|
| `0x02` | `+0x14` | `W1[20:15] = resolved & 0x3f` |
| `0x04` | `+0x18` | `W0[5:0] = resolved & 0x3f` |
| `0x10` | `+0x1c` | `W1[13:9] = resolved & 0x1f` |

`0x1dc940..0x1dc95f` stores W0 and W1 directly into the native 8-byte-slot
vector [BIN]. This closes the chain from a REP/LOOP source anchor, through a
typed symbolic descriptor and paired physical allocation, to the exact native
instruction bits [DER]. No step interprets the anchor or compiler serial as a
byte displacement [BIN][DER].

## Closed boundary and remaining uncertainty

The native placement chain for the typed REP/LOOP control descriptors is now
exact through source-anchor extraction, same-anchor multiplicity, typed fixup
construction, physical selector allocation, mapping publication, modifier
preservation and final W0/W1 insertion. The exact remaining gaps are:

1. the frontend semantic rules that choose every anchor and typed descriptor
   family for arbitrary D3D constructs. The structural scheduler for arbitrary
   graph topology is now exact in `parhelia-m9-cfg-linked-image-re.md`;
2. the final byte join that combines completed native images and referenced
   resources inside the heterogeneous external selector-`6c` record. One
   coherent 0x100-byte linked-state image is now byte-exact at host object
   `+0xd9`, but its placement in the 0x80-aligned GPU allocation is not joined;
3. the hardware-semantic names of the three selector namespaces. Their
   per-context reset, exhaustion, scheduling quarantine, retirement and
   ordinary reuse rules are now exact.

The mapping `entry+0x04` producer and its final native-slot unit are no longer
open. This result does not turn the outer `6c` resource into an instruction
array or establish a universal record-relative code offset [DER].

Existing allocation, cache, COW publication and lifetime results are unchanged
and are intentionally not repeated here.

## Reproduction

From the repository root:

```sh
python3 tools/m9-linked-pair-target-evidence.py
```

The verifier uses only Python's standard library and performs static reads.
