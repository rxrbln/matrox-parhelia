# M9 selector-6c constructor static closeout

> **Superseded:** `parhelia-m9-pool5-raw-copy-closure.md` resolves the concrete
> Linux and Windows vtables and corrects this note's opaque-serializer and
> variable-record interpretation. Keep this note only for the earlier staging
> and publication evidence.

This note supersedes the tentative producer join in
`parhelia-m9-linked-record-final-join-boundary.md`. Static analysis now closes
the publication path into the final program state. It also reduces the missing
constructor to one concrete virtual callback. It does **not** make a captured
record safe to replay.

Addresses are virtual addresses. `[BIN]` marks direct binary evidence, `[CAP]`
saved capture evidence, and `[DER]` a conclusion constrained by those facts.
The pinned inputs are:

* Linux `m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* Windows `MSerX/MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`;
* controlled flat/Gouraud case 37 in
  `captures/m9x-linux-fixed-state-20260914/pairs`.

`tools/m9-selector6c-constructor-evidence.py` checks the binary bodies, every
call edge below, the cross-platform publisher, and the captured bind values.
It only reads saved files.

## Corrected ownership and final publication

Linux top state embeds five different objects. `m9x3673` constructs them at
base offsets `+0xe8`, `+0x208`, `+0xbb0`, `+0x1798`, and `+0x17c0`, then stores
owner pointers at top `+0xc0`, `+0xc8`, `+0xd0`, `+0xd8`, and `+0xe0`.
Consumers use a view biased by eight bytes. In that view, the final program
state is called top `+0xc8`; the separate resource/link state is top `+0xd8`.
`m9x11566` makes the distinction unambiguous by resetting the former with
`m9x18369` and the latter with `m9x31420` [BIN].

`m9x21983` promotes compiler pairs into the separate `m9x31420` object. Its
destination `+0xb8/+0xc0` does not alias the final program state's same-named
fields. The old proposed `m9x21983 -> m9x2427` join was therefore wrong.

The real producer is `m9x2048` at `0x44b550`. In compact C-like form, with
descriptor bit packing omitted but byte-for-byte pinned by the verifier, it
does this [BIN]:

```c
bool publish_frag_ptrs(struct owner *o, struct c34 **dst)
{
        struct pair pair = { 0 };
        struct RsFragPtrsBuffer *pool = o->frag_ptrs; /* owner + 0xa4 */

        pool->vtable[0x38 / 8](pool);                 /* opaque commit */
        if (!pool->vtable[0x48 / 8](pool, 5, &pair, 0, 0))
                return false;

        (*dst)->resource = pair.resource;             /* c34 + 0xc0 */
        (*dst)->descriptor = pack_pair_descriptor(pair); /* c34 + 0xb8 */
        (*dst)->generation = ++o->device_state->generation_894; /* +0xcc */
        return true;
}
```

`m9x21784` at `0x44b4d0` supplies the staged helper object. It reuses
`owner+0xa4`; on first use it calls the device allocator with exact arguments
`size=0x80` and `type/pool=5`, caches the returned object, then calls the
object's vtable `+0x30` to obtain a helper node [BIN]. An assertion string at
Linux `0xe916a0` names the allocator exactly:

```text
virtual Pnx::RsFragPtrsBuffer*
LinuxDevice::CreateFragPtrsBuffer(MUINT32, Pnx::RsResourceType::Pool)
```

The xref `m9x9761` at `0x295a10` is the base-device assertion stub. The
concrete LinuxDevice override and its serializer do not have a recovered
symbol or direct call edge in this image [BIN].

Windows independently proves the same lifecycle [BIN]:

| Operation | Linux x86-64 | Windows x86 |
|---|---:|---:|
| cached pool | owner `+0xa4` | owner `+0x84` |
| create pool | device vtable `+0x38`, `(0x80, 5)` | device vtable `+0x18`, `(0x80, 5)` |
| obtain helper | pool vtable `+0x30` | pool vtable `+0x14` |
| commit/serialize | pool vtable `+0x38` | pool vtable `+0x18` |
| query mapped pair | pool vtable `+0x48`, `(5,&pair,0,0)` | pool vtable `+0x20`, `(5,&pair,0,0)` |
| publish object | c34 `+0xc0` | c04 `+0xb0` |
| publish descriptor | c34 `+0xb8` | c04 `+0xac` |
| publish generation | c34 `+0xcc`, source `+0x894` | c04 `+0xb8`, source `+0x4fc` |

The Windows publisher is `0x13c200`. Its five direct callers pair exactly with
five helper acquisitions from `0x13c1a0`. This cross-platform match rules out
an incidental Linux cache or error path [BIN][DER].

## Exact serializer inputs now visible

Every dynamic builder computes the destination as
`owner->c34_array + owner->index * 0xc34`, obtains a helper, fills one of the
following payload shapes, and passes the address of that destination pointer
to `m9x2048` [BIN]:

| Linux builder | acquire / publish | staged helper bytes | source or meaning |
|---|---|---|---|
| `m9x111` | `44b8b9` / `44b908` | `+0x60=u32 enable`, `+0x64=float bits`, `+0x68=u32`, `+0x6c=u32` | computed scalar, owner `+0x48`, owner `+0x50` |
| `m9x26427` | `44bf29` / `44bf90` | `+0x1c=u32` | packed computed control |
| `m9x25711` main | `44c046` / `44c0d2` | `+0x0c=0 or 0x08000000`, `+0x20..` copied bytes | `0x40` or `0x60` bytes from direct `pipeline+0xe3c`; `[pipeline+0xd98]` separately supplies flags |
| `m9x25711` alternate | `44c25b` / `44c28b` | `+0x1c=0x3f800000 + (((linked[0x1ed]>>1)&0x3f)<<8)` | encoded native-fragment control |
| `m9x5091` | `44c57e` / `44c5d1` | `+0x00=u64`, `+0x08=u32`, `+0x10=u64`, `+0x18=u32` | `[pipeline+0xd98]+0xdb8, +0xdc0, +0xdc8, +0xdd0` |

The matching Windows acquire/publish pairs are `13b117/13b13e`,
`13b81c/13b850`, `13ba19/13ba30`, `13babb/13bae8`, and
`13bd1e/13bd31`. Their payload shapes match the Linux builders after the
expected 32/64-bit layout changes [BIN].

`m9x4576` orders these builders per active c34 entry. The relevant order is
`m9x111`, other direct state builders, `m9x5091`, `m9x25711`, `m9x26427`, then
the fixed-chain `m9x24483` builder. Each dynamic builder appends one helper to
the cached `RsFragPtrsBuffer`, commits, and republishes a generation; later
builders extend the same per-owner serialization state [BIN][DER].

Where hardware support is absent, `m9x25711` instead copies `0x60` bytes into
c34 `+0x7f4`, and `m9x5091` copies its direct fields into c34
`+0x7d4..+0x7f0`; both keep the 6c pair equal to the empty sentinel. Those
fallback fields are semantic inputs, not a replayable final 6c image [BIN].

## Exact selector-6c emission contract

`m9x18369` initializes both resource pairs from the global empty sentinel at
Linux `0x125c9f0`: the selector-69 pair at c34 `+0xa0/+0xa8` and selector-6c
pair at c34 `+0xb8/+0xc0`. It zeros controls/generation at `+0xb0`, `+0xc8`,
`+0xcc`, and `+0xd0`. `m9x31210` first zeros exactly `0xc34` bytes and then
tail-calls this constructor [BIN].

When the final pair is nonempty, `m9x2427` emits one group with header
`0x456c6b68` in this order [BIN]:

```text
68 = 0
6b = c34[0xc8]
6c = relocation(c34[0xb8], c34[0xc0], descriptor=4,
                usage=0x1a, count=1, destination_index=0x6c)
```

The command builder owns the address relocation. The allocation returned by
`RsFragPtrsBuffer` and every resource referenced by its serialized bytes must
remain alive until the submission fence retires [BIN][DER]. There is no
evidence for a separate user-visible upload packet: pool commit `+0x38`
creates/finalizes the native image, pool query `+0x48` returns its descriptor
and resource, and the command stream contains the single selector-6c
relocation [BIN].

`m9x25508` does not compare `+0xb8/+0xc0` directly. It compares generation
`+0xcc`; a change publishes dirty mask `0x3c`. `m9x26001` consumes that mask
through `m9x2427` and clears it. The independent selector-69 state uses dirty
mask `0x380` and `m9x11728` [BIN].

## Selector 69 is fully known but does not replace selector 6c

`m9x24483` constructs the optional inline selector-69 image from two selected
fragment chains. Each selected ID indexes a 32-byte table entry at `owner+8`;
the entry's vector begin/end are `+8/+16`, and each 16-byte fragment descriptor
is `{u64 cpu_pointer, u32 byte_length}`. Fragments from chain 0 are copied in
order, followed by chain 1. The cumulative byte count must stay at most
`0xbff`; allocation size is `(sum + 31) & ~31` [BIN].

The exact published inputs are [BIN]:

```text
c34 + 0xbd0 = start0 = bytes_before_chain0 >> 3
c34 + 0xbd4 = start1 = bytes_before_chain1 >> 3
c34 + 0xbd8 = aligned byte size, maximum 0xc00
c34 + 0xbe0 = CPU pointer to concatenated image
```

`m9x11728` copies the image into instruction ports in the same ordered command
submission. Each 32-byte line emits `68`, then `60..63`, then `64..67`. For
line byte offset `off`, the exact selector is
`0x80000000 | (off & 0x3fe0)`. It then binds literal `69=4` and two 16-bit
start values [BIN]:

```c
static void m9_upload_inline_image(const u8 *image, u32 bytes)
{
        u32 off;

        /* m9x24483 already rounds bytes to 32 and limits it to 0xc00. */
        for (off = 0; off < bytes; off += 32) {
                emit1(0x68, 0x80000000 | (off & 0x3fe0));
                emit4(0x60, get_unaligned_le32(image + off +  0),
                            get_unaligned_le32(image + off +  4),
                            get_unaligned_le32(image + off +  8),
                            get_unaligned_le32(image + off + 12));
                emit4(0x64, get_unaligned_le32(image + off + 16),
                            get_unaligned_le32(image + off + 20),
                            get_unaligned_le32(image + off + 24),
                            get_unaligned_le32(image + off + 28));
        }
}

static void m9_emit_inline_starts(bool split, u32 start0, u32 start1)
{
        if (split)
                emit(0x6d, start1 & 0xffff);
        emit(0x69, 4);
        if (split)
                emit(0x6a, start0 & 0xffff);
        else
                emit(0x6a, (start0 & 0xffff) |
                           ((start1 & 0xffff) << 16));
}
```

The vendor emitter orders `6d` before the grouped `69/6a` write in split
mode. The pseudo-function shows the values, not packet grouping. Inline bytes
have no BO relocations because they are copied into the command stream. The
external selector-69 path instead relocates c34 `+0xa0/+0xa8` with descriptor
`2`, usage `0x19`, count `1`, destination `0x69`; c34 `+0xb0` supplies `6a`
and optional c34 `+0xd0` supplies `6d` [BIN].

This image carries fixed declaration/vertex-conversion fragments. It is not
the selector-6c native semantic record. Linux and Windows both call the 69 and
6c emitters independently [BIN].

## Controlled flat/Gouraud difference

The successful controlled captures bind exactly these values four times
[CAP]:

```text
Gouraud: 68=00000000 69=11e42000 6a=25010000 6b=00001030 6c=40c3a401
flat:    68=00000000 69=11e42000 6a=25010000 6b=00001030 6c=40c3aa01
```

Thus 69, both packed start/control halves in 6a, and 6b are identical. Only
the tagged 6c selected address changes: the same `0x40c3a000` arena is selected
at aligned offsets `0x400` and `0xa00` [CAP][DER]. Both selected records begin
with these seven floats [CAP]:

```text
128.0, -128.0, 0.5, 0.0, 128.0, 128.0, 0.5
```

The defined remainder is a fixed first-process A/B image in every controlled
pair. The Gouraud/A linked payload is zero. Examples in the case-B record include
`0x007ff880` at relative `+0x24/+0x2c`, `0x007ff8a8` at `+0x4c/+0x54`, and
`0x007ff8d0` at `+0x74/+0x7c`. Thirteen fresh processes spanning alpha,
blend, color-mask, depth, cull, and flat-shade transitions reproduce those
values exactly. They are shared linked-fragment references, not flat-shading
opcodes [CAP][DER]. Their bounded constructor and scope are documented in
`parhelia-m9-fixed-sixword-provenance.md`.

## Smallest exact unknown and kernel integration boundary

The only opaque edge remaining between known inputs and the final pair is the
concrete `Pnx::RsFragPtrsBuffer` commit/serializer callback:

```text
Linux pool vtable +0x38    called by m9x2048 at 0x44b57f
Windows pool vtable +0x18  called by 0x13c200 at 0x13c22a
```

Its exact unknown behavior is: consume the ordered staged helper nodes, choose
the variable native record size/layout, allocate or reuse mapped backing,
write native bytes, attach internal resource fixups, and make the resulting
type-5 pair visible to the following query callback [BIN][DER]. The query,
publication, generation, selector-6c relocation, and lifetime are closed.

A clean kernel diagnostic can implement these proven pieces immediately:

1. keep per-context c34 state and separate 69 and 6c resource pairs;
2. cache an independently allocated serializer object and stage the five
   recovered payload shapes in vendor order;
3. publish a new generation only after serialization succeeds;
4. emit 69/start state before the `68/6b/6c` group, using the exact relocation
   descriptors above;
5. retain the native record and referenced BOs through fence retirement.

It can now synthesize the bounded first Gouraud and flat/case-B 6c records.
Arbitrary linked programs still require a verified `0x60`-byte linker product
or recovery of the upstream arena cursor and aliased writer. A source
watchpoint is the shortest route to name that writer, but it is not required
for the bounded constructor and is not the only way to close the general edge.

## Reproduction

```sh
python3 tools/m9-selector6c-constructor-evidence.py
python3 -m py_compile tools/m9-selector6c-constructor-evidence.py
```

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
