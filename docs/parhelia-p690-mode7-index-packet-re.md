# P690 mode-7 (`0x9f`) indexed packet: implementation evidence

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a narrow, clean-room RE note for the first native indexed-draw
prototype.  It is based on the saved unstripped Linux HAL
`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`, the retained Windows
`MTXPARD.dll`, and the checked-in GL capture/test corpus.  It records the
wire contract; it does **not** claim that the current DRM ABI accepts or has
hardware-tested this packet.

## Established packet contract

The native 32-bit-index draw is exactly three dwords:

```
header = 0x03000001 | context_layout_flags | 0x1c000000
cmd[0] = header                 // 0x9f000001 or 0xdf000001
cmd[1] = index_start            // byte address of first u32 index
cmd[2] = index_end              // exclusive byte address, low bits = memory class
```

`0x1c000000` is draw **mode 7**, address-mode 32-bit indices.  Its header
does not contain a vertex width or index count: `(header & 0xffff) == 1`
means three dwords, and the element count is `((cmd[2] & ~3) - cmd[1]) / 4`.
The mode-6 sibling is `0x9b000001` and selects 16-bit indices; it is not a
substitute for mode 7 because its start low bits encode halfword edge cases.

`context_layout_flags` is not caller-controlled packet padding.  Vendor
`mtx3712:0x18b72..0x18b83` produces `0x80000000` on every GL/D3D path, plus
`0x40000000` exactly when `layout.byte1 & 0x10`.  The normal header is thus
`0x9f000001`; the layout-flag form is `0xdf000001`.  The initial ABI should
generate the former itself and admit the latter only with a separately
validated compatible vertex-layout state.

Evidence is independent in both vendor stacks:

* Linux `mtx2661+0xb7` (`.text:0xc007`) stores `0x1c000000` for element size
  four; `mtx1443` (`0x5db0..0x5e01`) ORs it with `0x03000001` and the layout
  flags, then writes precisely three dwords.  `mtx3249` (`0x5e30..0x5e93`)
  emits the same packet and immediately submits it.
* Windows `MTXPARD.dll:0x12e1c7..0x12e1f9` selects the same mode for size four.
  Its two packet emitters at `0x132920..0x132988` and `0x132990..0x1329ff`
  reserve 12 bytes and write header/start/end.
* `tools/verify-features.py` independently generates the recovered mode-7
  header and checks `0x9f000001`.  This is header coverage only, not a GPU
  indexed-draw test.

## Address and element rules

For an index-buffer address `B | class`, byte `offset`, and count `n`, the
Linux builder implements:

```
start = (B & ~3) + offset
end   = (B | class) + (offset & ~3) + 4*n
```

This is visible in `mtx2661:0xbf7c..0xc02b`.  Because a mode-7 offset is a
multiple of four, the useful first-prototype form is simply:

```
start = B + offset
end   = start + 4*n | class
```

`MTXPARD.dll:0x12df60..0x12df7d` first computes
`offset + index_size * StartIndex`; its `0x12e1cc..0x12e1f9` mode-7 branch
then produces the same start/end result.  Thus `GL_UNSIGNED_INT` / Q3's
32-bit element stream maps directly to mode 7.  There is no index-count,
index-buffer-base, base-vertex, or index-bias register.  A non-zero first
index advances `start`; a base vertex advances every vertex-stream base.

Each 32-bit payload element is a **vertex ordinal** for the already-bound
streams.  It is neither a byte offset nor an address.  The separate stream
bases, byte strides, and the vendor's `0x1064`/`0x1060` vertex-index
floor/limit calculation make that division explicit.  The operands only
describe the index-array byte interval `[start, end)`: `end` is exclusive, so
six u32 elements occupy `[start, start + 24)`.  A verifier must require every
element to satisfy `floor <= index < limit` and also check the descriptor's
last byte against its owning stream allocation.

The memory class lives only in the final operand's low two bits.  A
`CREATE_VERTEX` allocation is local VRAM, class 0, hence both mode-7 operands
are four-byte aligned.  The saved vendor code also supports class 2 (AGP) and
class 3 (PCI/system DMA), but that is hardware packet evidence, not permission
to expose a raw physical-address ABI.

## State that mode 7 depends on

Mode 7 dereferences the currently bound vertex streams; it is not an indexed
version of the current `0x97` byte-range packet.  Before the draw, the stream
state must describe the *source vertices*:

* `0x1000..0x101c`: up to eight stream bases, each `address | class`.  They
  are ordinary indexed-register writes: the vendor's direct-fetch emitter
  produces the opcode-0 reglist

  ```
  0x0000000e, 0x03020100, base0, base1, base2, base3,
              0x07060504, base4, base5, base6, base7,
              0x13121110, stride01, stride23, stride45, stride67
  ```

  where index 0 maps to `0x1000`, index 7 to `0x101c`, index `0x10` to the
  packed stride registers beginning at `0x1040`, and every `strideNN` holds
  two 16-bit byte strides.  `mtx1035:0xbb06..0xbb35` calculates each base as
  `mtx2320(buffer) + (byte_offset & ~3)` and writes it to its selected stream
  slot.  A mode-7 index packet only names elements from this previously bound
  state; it contains no stream-base field.
* `0x1040..0x104c`: eight packed 16-bit **byte** strides.
* `0x1064`: legal vertex-index floor; `0x1060`: legal vertex-index limit.
  Linux `mtx1035:0xb940..0xbb55` calculates the former as the maximum stream
  floor and the latter as the minimum `(size - offset) / stride`.
* `0x10b4 = 0`, then `0x10b8` descriptors.  Each descriptor identifies a
  stream in bits `[2:0]`, an intra-stream dword offset in `[11:4]`, and ends
  the list with bit 31.  Linux `mtx2714:0xb6f0..0xb937` and Windows
  `MTXPARD.dll:0x12df80..0x12e111` agree on this stream setup.
* `0x10b0` supplies the primitive word.  For the initial Q3 triangle-list
  test this is the proven triangle-list state, not a new packet field.

Use one interleaved stream first.  The Linux direct-fetch builder keeps
descriptor offsets per stream, while the Windows driver appears to accumulate
them across streams.  The meanings agree for one stream, so a multi-stream
prototype would add an unrelated unresolved variable.

The `captures/gl-matrix/idx32-*.txt` files contain only de-indexed `0x97`
draws.  That demonstrates the vendor GL client-array fallback, not absence of
mode 7: ordinary client addresses lacked a GPU memory class.  It must not be
used as a positive hardware test for `0x9f`.

## Bounds and relocation requirements

A correct initial interface accepts only a P690-local, class-0 mode-7 draw:

1. The packet length must be exactly 3 and its header must be `0x9f000001`
   plus only independently validated layout flags.  Require initial
   `cmd[1] == cmd[2] == 0` before relocation; do not accept caller-provided
   addresses.
2. One index relocation must name a live, creator-owned, persistently pinned
   `CREATE_VERTEX` object, with checked `offset + length`, nonzero length,
   4-byte offset and length, and `length / 4` divisible by three for the
   triangle-list-only bring-up.  It must rewrite both operands from the
   kernel-known VRAM offset.  The existing relocation ABI has no independent
   byte-length field for an index range, so it needs a versioned new structure
   or a purpose-specific encoding; overloading the current `dword/delta`
   range form without documenting it is unsafe.
3. Every referenced stream base must likewise be relocated from a creator-
   owned `CREATE_VERTEX` object; require one stream initially.  Its base,
   stride, descriptors, `0x1060`, and `0x1064` must be validated as one draw
   contract.  A range-valid index buffer alone cannot prevent an index value
   from fetching beyond an unvalidated vertex allocation.
4. Validate every u32 index against the declared stream bounds before the
   doorbell, or supply an equivalent kernel-trusted maximum-index proof tied
   to immutable allocation contents.  `0x1060`/`0x1064` are fetch bounds, not
   a documented security fence for untrusted command submission.

## Current-ring constraints

The present render-node code deliberately cannot carry this packet:

* The kernel has a disabled structural foundation for `0x9f`: it accepts only
  `0x9f000001` with both operands zero, binds exactly one creator-owned
  `CREATE_VERTEX` stream relocation, bounds its stride/floor/limit and
  descriptors, and reads every u32 index with `drm_gem_vram_vmap()` while the
  TTM BO reservation is held. It still returns `-EOPNOTSUPP` before the
  engine doorbell, and `PARHELIA_PARAM_NATIVE_INDEXED` remains zero.
* `parhelia_select_vram_vertex_stream()` changes the last stream-0 value
  from class 3 to class 0.  That works only for the old `0x97` range path;
  it neither emits a nonzero stream base nor establishes an index relocation.
* `PARHELIA_RELOC_VRAM` is suitable for surface/sampler bases, but must not
  implicitly bless stream or index operands.  A dedicated type needs to bind
  ownership, range, and exact operand role.

Keep the current security boundary: userspace supplies independently framed
secondary packets; the kernel validates and copies them to kernel-owned
DMA32 blocks, creates the primary CALL chain, writeback, EOB, and fence.
Mode 7 must not introduce raw bus addresses, user primary-ring control,
nested CALLs, or reuse of a vertex/index allocation before its fence retires.
The current synchronous submit is safe for a first test.  Async queueing
requires at least independently owned in-flight command, vertex, index, and
writeback slots; merely dropping the wait would race the existing reusable
buffers.

### Writable-map TOCTOU gate

`CREATE_VERTEX` intentionally exports a writable DRM mmap offset so Mesa can
fill local VRAM.  A reservation prevents TTM relocation and lets the validator
map the BO, but it cannot prevent another CPU from storing a different ordinal
through that already established mapping after the validation loop and before
the WARP engine fetches it. Such a race could turn a validated index range
into an out-of-bounds vertex fetch. Do not enable native indexed submissions
on the basis of the structural scan alone. The next kernel change must copy
the checked u32 list into a driver-owned pinned local-VRAM scratch BO (or add
a real sealed, read-only `CREATE_VERTEX` index allocation which revokes
writable mappings) and submit that immutable copy. Only then may the kernel
rewrite the mode-7 operands and advertise `PARHELIA_PARAM_NATIVE_INDEXED`.

## First hardware result (2026-09-10)

The isolated screen test reached the P690 after the immutable, page-aligned
local-VRAM index snapshot had been added. It did not complete: its first
native packet timed out and the WARP engine wedged. The kernel recorded:

```
submit timeout at CALL 28/28:
BM=03000000 prim=35c40238/35c4023f sec=00000000/00000000
fence=00000058 want=00000060; wedged
DMA status450=fbefdf0f want=00000008 writeback=00000007
DWGCTL=00a60ec4 908=638fa047 924=00201002
```

This proves the packet passed the DRM ownership, relocation and index-copy
gates and reached hardware. It does not prove the recovered state sequence is
sufficient. The prototype wrote stream-0 base and stride, while the vendor
direct-fetch emitter writes the full eight-base/four-packed-stride reglist
shown above before the indexed packet. That state difference is the next RE
gate. The engine was reset and the capability is deliberately disabled
(`PARHELIA_PARAM_NATIVE_INDEXED = 0`) in the staged module, so Mesa cannot
issue this path to Q3. A timeout can leave the device wedged until reset.

## Bring-up gate

The first real proof should be a standalone P690 screen test: one local-VRAM
interleaved quad, six 32-bit indices, one fully relocated mode-7 packet, and
pixel/readback comparison with the existing two-triangle `0x97` result.
Exercise offset zero and a nonzero four-byte index offset, then reject every
unaligned, overflowing, zero-length, out-of-bounds, duplicate, and
unrelocated case in a kernel self-test.  Only after that result is stable
should Mesa replace Q3's de-indexed software-draw batches.
