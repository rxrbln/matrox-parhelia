# M9148 secondary-range start tag

This note closes the last publication ambiguity left by commit `bd5ccd6`.
That revision correctly recovered the index-`2c/2d` record and end tag `2`, but
provisionally showed an untagged start for the clean DMA32 case.  Preserved
Linux ioctl output, joined to the kernel and userspace implementations, proves
that the active vendor GL BmBuffers use start tag **1**.

Recheck the binary slices, object lifecycle, captured ioctl fields, and a
reference descriptor with:

```sh
python3 tools/m9-secondary-start-tag-evidence.py
```

## Implementable rule

For a dword-aligned, compact-PTE-mapped system BmBuffer with GPUVA `S`, extent
`N`, and logical half-open range `[a,b)`, publish:

```text
word 0 = 45452d2c
word 1 = (S + 8 + a) | 1
word 2 = (S + 8 + min(b, N - 8)) | 2
```

For the three saved first-triangle ranges and example base `S=0x12000000`,
the exact records are:

```text
45452d2c 12000009 12000272
45452d2c 12000271 120004c6
45452d2c 120004c5 120004f2
```

The clean-room driver does not need to reproduce the vendor object's private
field.  Bind its coherent backing at a system GPUVA and use the fixed
transport tag `1` on that GPUVA.  The backing's raw DMA/mmap-cookie address
must not be substituted.  This start tag is separate from the primary
requester's `0x400 = primary_dma | 2` contract and from the index-`2d` end
tag `2`.

## Kernel object-to-descriptor path

The kernel core vtable symbol `m9x992` starts at `.rodata+0x540`, and objects
point to its effective vtable at `+0x10`.  Effective slot `+0x80` relocates to
`m9x830`, the `ExecuteDma` implementation.  Effective slot `+0x98` relocates
to `m9x83`, which obtains and returns the next BmBuffer.  This corrects an
earlier tentative identification of `m9x640` as the BmBuffer-return method;
that method occupies effective slot `+0x78`.

`m9x83` returns these fields through the ioctl tail:

```text
tail+08 = kernel BmBuffer pointer
tail+10 = BmBuffer+34
tail+14 = BmBuffer+40 extent
```

`m9x830` consumes the active BmBuffer pointer from `tail+08`.  Its exact range
publisher derives `base = BmBuffer+44 + 8`, adds the logical start, ORs
`BmBuffer+34` into that start, clamps the logical end to `extent-8`, and ORs
`2` into the end.  The field is therefore best named the **BmBuffer secondary
start/access-class tag**.  Its proven hardware role is the low tag bits on an
index-`2c` start address.

The same routine gives value zero another concrete role.  When `BmBuffer+34`
is zero and the input tail pointer is non-null, it copies command dwords from
the CPU-side pointer at `BmBuffer+0xb0` into the mapped destination at
`BmBuffer+0x50` before publication.  Nonzero classes bypass that copy-in.

Linux userspace closes both sides of the ioctl join.  `m9x24186` places its
current user BmBuffer handle into ioctl tail `+08`, calls `ExecuteDma`, then
copies returned tail fields `+00,+08,+10,+18,+20` back into its user BmBuffer.
The ioctl is 0x840 bytes, its tail starts at `+0x818`, and the returned class
therefore appears at complete ioctl offset `0x828`.  Userspace `m9x22842`
independently emits header `0x45452d2c`, ORs its BmBuffer class into the start,
and ORs `2` into the end in the single-buffer `PostDOSBuffer` path.

## Preserved runtime proof

The archive `captures/m9x-linux-ff-tnl-20260914/raw-preserve.tgz`, SHA-256
`49859ba73319efd6d967e2a873aa6508b1de7eccfeb026508cd10bf35317d2c2`,
contains 15 complete 0x840-byte `ExecuteDma` input/output pairs.  Three empty
members from an interrupted later capture are ignored.  Every complete pair
has:

| field | all inputs | all outputs | joined meaning |
|---|---:|---:|---|
| ioctl `+0x828` | `0` | `1` | returned next BmBuffer `+0x34` |
| ioctl `+0x82c` | unspecified | `0x8000` | returned next BmBuffer extent |

The input zero is expected: the userspace wrapper supplies the current kernel
BmBuffer pointer at tail `+08`; it does not supply the private class at
tail `+10`.  Kernel `m9x83` fills that output after executing the current
buffer.  The buffers ping-pong through subsequent calls, so these records pin
the steady active GL pool as class `1`.  The same vtable method supplies the
initial buffer acquisition, with the same output layout.  This is direct
runtime evidence for the canonical M9148 GL command-buffer class, rather than
an inference from an allocator mode number.

## Lifecycle and alternate classes

The private lifecycle explains why static inspection alone admitted several
answers:

- Constructors `m9x559`, `m9x597`, `m9x306`, and `m9x999` initialize both
  `+0x30` and `+0x34` to `1`.
- `m9x999` can replace both fields with a caller-supplied class while attaching
  an existing mapping.
- `m9x810` can copy a computed resource class from `+0x30` into `+0x34` after
  successful backing allocation.
- `m9x445` wraps an existing resource and maps its internal mode through the
  table `{0,1,2,2,2}` into both fields.
- Release path `m9x514` restores both fields to `1` after dropping the backing.

There is a separate static pool-construction path which tries internal mode 2
and can consequently construct class-2 objects.  It does not establish that
the captured GL BmBuffer pool uses tag 2.  The returned-object field in all 15
complete captures establishes tag 1 for that pool and takes precedence over
an association inferred from the alternate constructor.

Windows `MxMD.dll` at `0x24c73` corroborates the record structure.  It emits
the same header, combines a resource-relative offset and a separate low start
tag, and uses a zero-or-two end tag.  Its private object layout differs, so it
does not independently assign a numeric value to Linux `BmBuffer+0x34`.

The address-field audit in `parhelia-m9-secondary-address-domain-audit.md`
closes the other half of this rule: use the class-1 compact-PTE GPUVA with
start tag `1`.  The human-facing meanings and hardware
requirements of vendor-only classes 0 and 2 remain unproven beyond the copy-in
behavior and alternate construction paths above.  A clean-room driver need
not expose those classes unless it later implements those backing types.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
