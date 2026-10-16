# M9 indices 68--6d resource and native-state reconciliation

This note corrects the former claims that live index `69` points to a generic
native-program heap or drawable backing.  The exact `RsFragPtrsBuffer`
constructor and binding path prove that it selects the external fixed-fragment
database.  The old address-directed BAR snapshot is an opaque alias, and
translation from the emitted address to the logical database remains
unresolved.  Independent disassembly of the Linux and Windows emitters fixes
what each field does without assigning shader-stage names that the joins do
not support.

Evidence labels are `[BIN]` for exact static instructions, `[CAP]` for captured
bytes or commands, and `[DER]` for a conclusion constrained by those facts.
`tools/m9-program-resource-reconciliation-evidence.py` hash-pins and checks the
facts below.

The reference images are:

* Linux X-server 1.13 `m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* Windows `MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`;
* live fixture `captures/m9x-linux-gl2-special-20260914`, whose individual
  files are pinned in its manifest.

## The emitters are two separate resource/control groups

Linux top-state emitter `m9x26001` at `0x4ba3a0` calls `m9x2427` at
`0x4ba8fb`, then calls `m9x11728` at `0x4ba919`.  Both receive the same large
top-state `+0xc8` object, but consume disjoint field groups [BIN].

| final index | Linux producer | Windows producer | exact established role |
| ---: | --- | --- | --- |
| `68` | `m9x2427`: literal zero in the external `6c` bind; both emitters also use it as an indexed-store selector | `0xf78c0` and inline path in `0xf74d0` | multiplexed selector/control; not a persistent program address |
| `69` | `m9x11728`: relocation from descriptor/pointer `+0xa0/+0xa8`, descriptor 2, usage `0x19` | `0xf74d0`: `+0xa0/+0xa4`, same descriptor and usage | external relocation-backed fixed-fragment database; address translation unresolved |
| `6a` | `m9x11728`: numeric dword `+0xb0` | `0xf74d0`: numeric dword `+0xa8` | control paired with the `69` group; external bitfield is not decoded |
| `6b` | `m9x2427`: numeric dword `+0xc8` | `0xf78c0`: numeric dword `+0xb4` | control paired with the `6c` group; normalizer forces the observed `0x1030` bits |
| `6c` | `m9x2427`: relocation from descriptor/pointer `+0xb8/+0xc0`, descriptor 4, usage `0x1a` | `0xf78c0`: relocation from `+0xac/+0xb0`, same descriptor and usage | external relocation-backed system-resource record; low bit 0 is retained as a tag |
| `6d` | `m9x11728`: optional numeric dword `+0xd0` | `0xf74d0`: optional numeric dword `+0xbc` | companion control in the same group as `69/6a`; absent from the captured external tuple |

Linux `m9x2427` emits one indexed packet with header `0x456c6b68`: `68=0`,
`6b=state+0xc8`, then relocation-backed `6c`.  Its resource-change path
normalizes the two bytes making up `6b`; Windows `0xf78c0` expresses the same
operation more directly as `(old & 0xffff91b0) | 0x1030` [BIN].  The live
value is exactly `0x00001030` in all 17 isolated GL2 cases [CAP].

Linux `m9x11728` separately relocates `69`, writes `6a`, and optionally writes
`6d`.  Windows `0xf74d0` is structurally identical.  The relocation callback
arguments identify two private vendor resource classes, `0x19` and `0x1a`.
They do not identify a shader stage or prove that either resource contains
native instructions [BIN][DER].

## The inline form is a different representation

Both operating-system binaries also implement an alternative inline form.
They upload a buffer in 32-byte lines through selector `68` and ports
`60..67`.  Each line contains four native two-dword instructions.  When the
upload finishes, the emitter writes literal `4` to `69`.  It either packs two
16-bit instruction starts into `6a`, or places the first start in `6a` and a
companion start in `6d` [BIN].

That inline representation proves that the controls can hold instruction-slot
starts in this fixed-chain route.  It does not prove that external
`6a=0x25010000` has the same field layout.  In particular, the external route
puts an address at `69`, whereas the inline route puts the non-address literal
`4` there.  A new driver must keep the two modes distinct [DER].

The aligned inline byte length is consumed by the upload loop only.  No final
count or end is written beside `69/6a/6d`.  The two inline starts belong to a
fixed vertex/declaration-conversion chain and its optional companion.  Static
evidence still does not assign them universally to VS and FS [BIN][DER].

## Live binding and address-snapshot boundary

The special-op fixture records the allocation descriptor used for `69` and an
address-directed BAR snapshot at the same numeric value.  The descriptor result
is [CAP]:

| result field | value |
| --- | ---: |
| object bytes `+0x08`, `+0x20` | `3`, `4` |
| GPU address `+0x208` | `0x11e42000` |
| mapping/result address `+0x210` | `0x11e42000` |
| extent `+0x218` | `0x1b080` |
| alignment `+0x21c` | `0x1000` |

All 17 isolated draws bind `69=0x11e42000`.  The 0x1b080-byte BAR snapshots
vary across cases, with more than eight distinct complete hashes, and include
values matching the preceding 128x128 render, including the exact `dfdx`,
`dfdy`, comparison, texture, and control-flow colors [CAP].  The static
constructor and bind path establish that `69` selects the fixed-fragment
database [BIN].
The mismatch makes the old address-directed bytes an opaque alias: they are
not a linear database image or drawable-ownership evidence and do not prove an
MMU root or other translation structure.  Translation from the emitted address
to the logical database remains unresolved [DER].

The separately captured system allocation is [CAP]:

| result field | value |
| --- | ---: |
| object bytes `+0x08`, `+0x20` | `1`, `1` |
| GPU base `+0x208` | `0x40c3a000` |
| mmap cookie/result `+0x210` | `0x1a7cc000` |
| usable extent `+0x218` | `0x20080` |
| alignment `+0x21c` | `0x80` |
| captured mapping size | `0x21000` |

Every final `6c` is `system_base + case_offset | 1`.  Clearing only bit 0
gives a unique 0x80-aligned offset inside the usable extent.  The saved
`selected-6c-window.bin` equals the corresponding byte range of the full
system mapping for every case.  The sequence begins at offsets `0x200`,
`0x680`, `0xa80`, and continues monotonically to `0x4900` [CAP].

The observed allocation-object fields are recorded numerically because the
private ioctl field names and the allocator's wrap/reuse policy are not
statically closed.  The mmap extends 0xf80 bytes beyond the returned usable
extent.  That page-rounded tail must not be treated as allocatable payload
without another address join [DER].

## Where native instructions are, and where they are not

Final indices `8c/8d/8e` select end-exclusive ranges in the same system
allocation.  Their saved range bytes are ordinary constants and vertex or
preprocess data in these draws.  For example, the baseline raster range is
four IEEE floats and the preprocess range contains viewport and vertex data.
Those ranges therefore cannot be relabeled as native code [CAP].

Some `6c`-selected records contain exact two-dword, instruction-shaped spans:

| test | offset from untagged `6c` record | pinned slots |
| --- | ---: | ---: |
| `dfdy` | `0x100` | 8 |
| `loop-uniform` | `0x100` | 25 |
| `vertex-tex-lod` | `0x108` | 5 |

The slot width matches the independently recovered native ISA width.  The
words vary with special shader behavior and include control-flow and texture
forms, making these strong native-code candidates [CAP][DER].  The record
layout is not uniform, however.  Other cases have metadata, pointers, poison
patterns, or code at a different location in the same selected window. No
single public header field supplies an instruction start, instruction count,
VS entry, FS entry, or termination rule across all cases.

The exact current boundary is therefore:

* native instructions can reside inside or be referenced by the record
  selected through `6c` [CAP][DER];
* `69` binds the external fixed-fragment database, but its emitted-address
  translation is unresolved [BIN/DER];
* `8d..8e` ranges are exact constant/vertex-resource ranges here [CAP];
* the inline `68/60..67` path remains a separately proved on-chip upload mode
  [BIN];
* public VS/FS entry, count, and stop fields are absent from this transport;
  those details remain private to the opaque linked pair.

## Static compiled-resource producer boundary

Linux compiler/cache routine `m9x24062` calls `m9x21983` at `0x4530db`.
`m9x21983` copies two descriptor/pointer pairs from a compiled pipeline
resource at `+0x138` [BIN]:

| source bytes | destination pair |
| --- | --- |
| `+0x2f1..+0x2f4`, pointer `+0x2f9` | descriptor `+0x98`, pointer `+0xa0` |
| `+0x301..+0x304`, pointer `+0x309` | descriptor `+0xb8`, pointer `+0xc0` |

The second destination has exactly the descriptor/pointer shape consumed by
`m9x2427` for `6c`.  The copier's immediate destination is a separate
`m9x31420` resource object, while final emission uses the larger top-state
`+0xc8` object. Device construction and final dispatch prove that the former
is the top-state `+0xd8` resource/link path and the latter is the independent
`m9x18369` program path. There is no alias or promotion edge between these
matching field offsets: `m9x21983` is excluded as the direct writer of the
final emitter object [BIN][DER].

Initializer `m9x18369` constructs the large final object and independently
initializes descriptor/pointer pairs `+0xa0/+0xa8` and `+0xb8/+0xc0`, plus
controls `+0xb0/+0xc8/+0xd0` and inline state `+0xbcc..+0xbe0` [BIN].  This
confirms field ownership, but its default resource sentinels do not assign the
semantic role of `69` or `6c` [DER].

The exact cross-platform host-image and selected-allocation audit is in
`parhelia-m9-linked-record-final-join-boundary.md`. None of the host image's
eight invariant paired dwords occurs even once in any complete saved system
allocation, proving that final `6c` storage is semantically constructed rather
than copied verbatim [CAP][DER].

## Corresponding Direct3D command path

The Windows central state emitter at `0xf6f20` calls `0xf74d0` at `0xf7307`
for dirty groups `0x80/0x100/0x200`, and calls `0xf78c0` at `0xf7496` for
dirty group `0x3c00` [BIN].  This is a call-reachable Direct3D command path,
not a dormant signature.

Fixed vertex/declaration-conversion wrapper `0x13c2c0` feeds builders
`0x13b7e0`, `0x13b9b0`, `0x13bb80`, and `0x13bda0`, then fixed-fragment linker
`0x13bf80`.  The latter builds the inline native image and its two starts for
`0xf74d0` [BIN].  `0xf78c0` consumes the associated auxiliary records and
resource/control pair [BIN].

The programmable fragment compiler finalizes a compiler/cache record and
marks nested state dirty, but the exact setter/promotion edge from a Direct3D
VS/PS object to external `69/6a/6b/6c/6d` is not recovered.  The Windows
binary therefore corroborates the two hardware packet groups and the inline
fixed-chain route.  It does not resolve how the external `69` address reaches
the logical fixed-fragment database [DER].

## Driver contract and remaining construction work

Kernel validation and Mesa emission must use separate resource kinds for the
fixed-fragment database at index `69` and the linked record at index `6c`.
Generic names such as `PROGRAM` or `DRAWABLE` for `69` lose the established
identity and can lead a validator to apply the wrong memory class, alignment,
or tag.  For the captured external route:

```text
69 = relocated external fixed-fragment-database binding
6a = captured numeric control 0x25010000
68 = 0
6b = captured numeric control 0x00001030
6c = relocated system-record address | 1
6d = not emitted
```

These values are evidence fixtures, not universal constants.  Userspace must
never submit the captured raw addresses.  The kernel should add the proven
low tag only for the typed `6c` relocation, enforce 0x80 alignment, and check
the selected record lies within the BO.  It must not add that tag to `69`.

The separate GLSL A/B/A fixture now proves that either a VS-only or FS-only
change selects a new complete pair through `6c`; it exposes no independent
stage entry. Arbitrary programmable construction remains gated on the private
linked-pair builder, semantic control-flow placement, allocator wrap/reuse, and
general stop semantics. Until those joins are exact, expose only complete
known-good linked pairs and keep raw programmable submission disabled.
