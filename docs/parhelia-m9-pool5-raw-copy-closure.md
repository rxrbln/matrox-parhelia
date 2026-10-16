# M9 selector-6c pool and raw-copy static closeout

This note closes the concrete callback left open by the earlier selector-6c
analysis. The operation called a serializer there is only a dirty marker. The
vendor builds a fixed 0x80-byte semantic image directly in host memory and a
later resource operation copies that memory byte for byte into mapped backing.
There is no record packing, pointer traversal, or internal relocation pass.

Addresses are virtual addresses. `[BIN]` marks direct binary evidence, `[CAP]`
saved capture evidence, and `[DER]` a conclusion constrained by those facts.
The pinned inputs are:

* Linux `xserver/1_13_0/m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* Windows `MSerX/MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`;
* controlled flat/Gouraud pair 37 in
  `captures/m9x-linux-fixed-state-20260914/pairs/case-37`.

`tools/m9-selector6c-constructor-evidence.py` parses both executable formats,
checks their complete SHA-256 hashes, pins all relevant function bodies,
resolves ELF relocations and PE vtables, and reconstructs the first 0x80 bytes
of both captured records. It executes no vendor code.

## Linux concrete class closure

The two concrete Linux device vtable data objects are `m9x3864` at
`0x1222b80` and `m9x6183` at `0x1223b20`; their primary vptr values are
`0x1222b90` and `0x1223b30`. Both CreateFragPtrsBuffer slots, respectively at
`0x1222bc8` and `0x1223b68`, have `R_X86_64_RELATIVE` relocations to
`m9x25836` at `0x5b2690` [BIN]. These are every concrete LinuxDevice vtable
found in the image, and both converge on the same implementation.

`m9x21784` calls that slot with requested count `0x80` and pool/type 5. The
pool-5 arm of `m9x25836` does the following [BIN]:

1. allocates exactly `0x260` bytes;
2. calls `m9x18895` at `0x37d130` to construct the object;
3. creates a type-`0x31` resource configuration with width `0x80`, height 1;
4. calls `m9x29951` at `0x37cee0` to initialize it.

`m9x18895` loads GOT entry `0x12641a8`. Its relative relocation targets the
vtable data object `m9x30179` at `0x121d580`, so the primary vptr installed in
the returned object is `0x121d590`. Its exact slots are [BIN]:

| object slot | target | recovered role |
|---:|---:|---|
| `+00` | `37c920` | destructor |
| `+08` | `37d0e0` | deleting destructor |
| `+10` | `37cb80` | backing callback |
| `+18` | `37cbc0` | backing callback |
| `+20` | `37cea0` | resource operation dispatcher |
| `+28` | `37c8b0` | reset/destructor body |
| `+30` | `37cae0` | return host image pointer |
| `+38` | `37caf0` | set dirty |
| `+40` | `37cc00` | constant/type getter |
| `+48` | `37ccc0` | query resource pair |
| `+50` | `37cc20` | metadata query |
| `+58` | `37cc90` | mark dirty through subobject |

The earlier association with `m9x9761` at `0x295a10` was wrong. That function
is the base LinuxDevice assertion stub named by the embedded
`LinuxDevice::CreateFragPtrsBuffer` assertion string. The two derived vtables
above provide the concrete override [BIN].

## Linux commit and lazy copy

The callback used by `m9x2048` at pool vtable `+0x38` is the complete
eight-byte `m9x21044` [BIN]:

```c
static void pool5_commit(struct linux_pool5 *p)
{
        p->dirty_54 = 1;
}
```

The following vtable `+0x48` call, `m9x21936`, returns the already maintained
descriptor/resource pair at object `+0x248/+0x250`. For query type 5 it selects
mask `0x400` and metadata slot 2. It neither reads nor transforms the host
image [BIN]. Thus `m9x2048` only marks the resource dirty, queries its pair,
publishes it at c34 `+0xb8/+0xc0`, and increments generation `+0xcc`.

The actual copy is deferred until resource operation 1. Vtable `+0x20`
dispatches operation 1 to `m9x11321` at `0x37cdf0`. After its stale test and
possible `m9x21341` backing allocation, the only data movement is [BIN]:

```c
memcpy(p->mapped_58, p->host_38, p->byte_size_44);
p->dirty_54 = 0;
```

`m9x21341` aligns an arena cursor, obtains the mapped address and resource,
constructs the pair at `+0x248/+0x250`, and advances the cursor. It never
examines the copied bytes. There is therefore no pool-owned internal fixup or
relocation table [BIN]. The command builder still relocates the outer resource
pair when binding selector 6c, as documented previously.

## Exact sizes and the Linux upper bytes

For type `0x31`, `m9x29951` encodes format ID `0x78`. The format-bit lookup
table at `0xeb00a0 + 0x78 * 4` contains 32. Its byte-size calculation is
`ceil(width * height * bits / 8)`, hence [BIN]:

```text
Linux x86-64: 0x80 * 1 * 32 / 8 = 0x200 bytes
```

No Linux staging builder writes beyond offset `+0x7f`, and the host allocation
at object `+0x38` is not zeroed. Consequently the copied range
`+0x80..+0x1ff` contains allocator residue. Case 37 makes that visible: the
Gouraud tail contains repeated `00010000 fffffffc 00000000` triplets,
descriptor-like values, and two copies of host-looking
`00002b600d4dfee8`; the flat tail contains `00002b600d4dfed8` [CAP]. These
bytes are not a variable pool record and must not be reproduced [BIN][CAP][DER].

A clean implementation may allocate the observed Linux-compatible 0x200-byte
backing, but it should zero `+0x80..+0x1ff`. The hardware semantic image is the
defined first 0x80 bytes [DER].

## Windows independent closure

The Windows concrete device constructor at `0x751f0` installs vptr
`0x1f3850`. That vtable's device slot `+0x18` is `0x75930`, the concrete
CreateFragPtrsBuffer implementation [BIN]. Its pool-5 path rounds the requested
`0x80` bytes to `0x20` dwords, builds width `0x20`, height 1, and calls
`0xa3910`.

`0xa3910` allocates a `0x1cc` backing object and a `0x3c8` wrapper. Constructor
`0xa32a0` installs pool-subobject vptr `0x1f3d08` at wrapper `+0x38`;
initializer `0xa4390` stores the backing pointer at wrapper `+0x70`. The
returned interface is wrapper `+0x38`. Its relevant slots are [BIN]:

| object slot | target | recovered role |
|---:|---:|---|
| `+14` | `a66c0` | return mapped host image |
| `+18` | `a66f0` | set backing dirty |
| `+20` | `a9100` | thunk to pair query `a6740` |

The ordinary pool arm of `0xa66f0` only executes
`wrapper->backing_70->dirty_28 = 1`. Main-wrapper resource operation 1 at
`0xa78f0` calls `0xdc5e0`, whose only image movement is [BIN]:

```c
memcpy(backing->mapped_2c, backing->host_14, backing->byte_size_1c);
backing->dirty_28 = 0;
```

The Windows byte calculation is `0x20 * 1 * 32 / 8 = 0x80`. This confirms the
semantic image size and also explains the x86-64 Linux tail: Windows treats the
request as bytes and converts it to dwords before resource creation; this
Linux binary passes `0x80` directly as the 32-bit element width [BIN][DER].

## Exact first-triangle images

The selected Linux backings and their hashes are [CAP]:

```text
Gouraud draw-a-resource.bin[0x400:0x600]
aa6610b823e68e70aa6eed21fd8cba99d90ce6e6c86d03b9e3e193fe8fc17018

flat draw-b-resource.bin[0xa00:0xc00]
10243697f9ab2490e1e962763fa931117be896d61222a522fcf739135b4fc241
```

The exact defined 0x80-byte images, shown as little-endian u32 values, are:

```text
Gouraud
00: 43000000 c3000000 3f000000 00000000 43000000 43000000 3f000000 00000000
20: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
40: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
60: 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000

flat
00: 43000000 c3000000 3f000000 00000000 43000000 43000000 3f000000 00000000
20: 00000000 007ff880 00000000 007ff880 00000000 00000000 00000000 00000000
40: 00000000 00000000 00000000 007ff8a8 00000000 007ff8a8 00000000 00000000
60: 00000000 00000000 00000000 00000000 00000000 007ff8d0 00000000 007ff8d0
```

The common prefix comes from `m9x5091`: direct fields at `+00/+08/+10/+18`.
`m9x25711` then copies exactly 0x60 bytes from the linked-fragment product at
direct `pipeline+0xe3c` to image `+0x20`. The separately loaded
`[pipeline+0xd98]` object supplies builder flags; it is not the copy base. For
this Gouraud capture those bytes
are zero. For flat they contain three values twice:

```text
linked source +04/+0c -> image +24/+2c = 007ff880
linked source +2c/+34 -> image +4c/+54 = 007ff8a8
linked source +54/+5c -> image +74/+7c = 007ff8d0
```

These are semantic words already produced by the fragment compiler/linker.
Neither pool commit nor lazy copy changes or relocates them [BIN]. The
cross-capture encoding and bounded first-process allocation schedule are
closed in `parhelia-m9-fixed-sixword-provenance.md`; the upstream cursor and
writer for arbitrary linked programs remain unresolved.

`tools/m9-pool5-image.h` provides an implementation-ready field constructor.
The Gouraud constructor emits the recovered image and zeros the whole
Linux-compatible backing. The flat constructor fails unless the caller marks
a complete 0x60-byte linked-native product, then copies that product exactly as
`m9x25711` does. The bounded case-B constructor reproduces the six words for
the saved initial arena schedule, including case-37 flat shading.

## Remaining live observation

Static analysis has closed all concrete device and pool vtables, allocation,
dirtying, copying, pair publication, and outer command relocation. No live
watchpoint is needed for the pool path.

No live observation is required for the bounded first-triangle constructor.
For a general allocator-independent constructor, a watchpoint on direct
`pipeline+0xe3c` remains the shortest way to identify the aliased writer and
associated cursor, but static alias recovery or controlled cursor-variation
captures can close the same edge. Until that broader edge is closed, callers
must provide the full linked-native 0x60 bytes from a separately verified
linker or fail closed.

## Reproduction

```sh
python3 tools/m9-selector6c-constructor-evidence.py
python3 -m py_compile tools/m9-selector6c-constructor-evidence.py
cc -std=c11 -Wall -Wextra -Werror -x c -fsyntax-only tools/m9-pool5-image.h
```

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
