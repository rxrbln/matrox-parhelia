# M9 Linux external fixed-fragment database binding

This note closes the Linux vendor ownership and command-publication path for
the external fixed-fragment database used by selector `69`. It does not close
the GPU virtual-address translation behind the vendor relocation callbacks.
Addresses are virtual addresses in the pinned Linux `m9x_drv.so`; `[BIN]`
means static binary evidence.

## Logical database construction and backing

`m9x30489` at `0x446400` builds the logical fixed-fragment database as 57
vectors of 16-byte `{source pointer, byte length}` descriptors. The first
four payloads at `0xea4200`, `0xea42c0`, `0xea4500`, and `0xea4640` are
byte-identical to the Windows normal-path P00 fragments, with sizes `0xa8`,
`0x240`, `0x128`, and `0xb8`. Calls at `0x446435`, `0x446457`, `0x446479`,
and `0x44649b` append them through `0x446130` [BIN].

`m9x25676` at `0x4496d0` records the owning context at logical-object
`+0x7a8` and calls the constructor. Its caller at `0x461f17` passes the
owner's `+0x38` object. This is the Linux analog of the independently
extracted Windows normal logical database whose defined concatenation is
`0x1a040` bytes. The Linux allocator does not embed that number as an
immediate: it reads the constructor's computed total from logical-object
`+0x898` [BIN].

`m9x17919` at `0x461900` allocates a backing through device vtable `+0x38`:

```text
size     = *(uint32_t *)(owner->logical_db + 0x898)
pool     = 3, with fallback pool 4
backing  = allocation result stored at owner +0x88
```

It calls `m9x16102` at `0x460550`. That function obtains a host staging
pointer through backing vtable `+0x30`, walks exactly `0x39` logical chains
at a `0x20`-byte stride, and copies every 16-byte descriptor's payload with
`memcpy` in chain order. It commits through backing vtable `+0x38` [BIN].
Thus the uploaded resource is the flattened logical database, separate from
drawable storage.

After upload, `m9x17919` calls `m9x11136` at `0x3b7210`. The latter publishes
the backing at final-state `+0x3c`, updates generation `+0x38`, and sets dirty
bit `0x40000`. `m9x25615` at `0x3aee10` propagates the generation/resource
pair and the same dirty bit when copying state [BIN].

## Getter and selector publication

`m9x17624` at `0x44a9e0` selects a `0xc34`-byte entry, obtains a resource from
builder `+0x64`, and calls its vtable `+0x48` with operation type `5`. The
returned pair is written at `c34+0xa0/a8`; the builder retains the queried
resource at `+0x9c`. Its direct builder caller is `0x44c6bf` [BIN]. The
bounded pass did not locate the assignment joining final-state `+0x3c` to
builder `+0x64`; those are two proven segments of the ownership chain rather
than a proven direct field-to-field copy.

`m9x11728` at `0x4b9340` emits selector `69` and submits one relocation for
the pair at `c34+0xa0`:

```text
selector header  0x45454569
descriptor       2
pair             c34 + 0xa0
count            1
usage            0x19
callback         command-builder vtable +0xb0
```

The program-state dispatcher calls this emitter at `0x4ba919` on the dirty
`0x380` route [BIN]. This is the external fixed-fragment database binding.

The neighboring selector-`6c` route is separate. `m9x2427` at `0x4b99d0`
submits the pair at `c34+0xb8`, descriptor `4`, count `1`, with relocation
usage `0x1a`; the dispatcher calls it at `0x4ba8fb` on the dirty `0x3c`
route [BIN]. Usage `0x1a` therefore belongs to selector `6c`, not to selector
`69` or the external database allocation.

## Remaining translation boundary

This call graph ends at the resource type-5 getter and the command-builder
relocation callback. It proves logical database ownership, upload order,
final-state publication, the getter-to-selector path, and selector-`69`
usage `0x19`. The final-state `+0x3c` to builder `+0x64` assignment remains
unlocated. The chain also does not reveal how the returned GPU virtual
address maps to a physical BAR window.

The saved allocation descriptor's `0x11e42000` address and `0x1b080` extent
remain exact capture evidence. Address-directed BAR snapshots at that value
alias prior drawable contents across controlled runs, so those bytes are an
opaque alias. They do not establish a linear database mapping, an MMU table,
or a root pointer. Closing the final translation requires a resource-aware
dump or a trace inside the allocator/getter/relocation implementation.

## Reproduction

```sh
python3 tools/m9-index69-db-binding-evidence.py
python3 -m py_compile tools/m9-index69-db-binding-evidence.py
```

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
