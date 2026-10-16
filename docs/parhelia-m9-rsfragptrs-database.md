# M9 Windows RsFragPtrsBuffer database extraction

This note records the exact normal fixed-fragment database produced by the
saved Windows `MSerX/MxMD.dll`.  It also separates that logical image from the
opaque bytes returned by the old Linux address-directed capture of index 69.
Addresses below are virtual addresses.  `[BIN]` is direct binary evidence and
`[CAP]` is saved capture evidence.

## Normal constructor path

`MxMD.dll` function `0x1454a0` initializes 57 fragment vectors, slots
`0x00..0x38`.  Each vector holds `{source pointer, byte length}` descriptors
appended by `0xadf00`.  Function `0x120910` walks exactly `0x390` bytes of
16-byte vector objects and copies each descriptor payload in slot order with
no separator or per-chain padding [BIN].

The constructor has two input predicates at owner state `+0x0c` and `+0x40`.
The path selected by the saved first-triangle bind is:

```text
owner state +0x0c != 0
owner state +0x40 == 0
252 descriptor payloads
57 chains
defined concatenated size = 0x1a040
page-rounded allocation   = 0x1b000
```

The other three constructor paths total `0x1c1d8`, `0x1c050`, and `0x19e40`.
The normal-path selection is independently fixed by selector `6a` in the live
capture.  Chain P00 begins at byte `0`, and chain S28 begins at byte
`0x12808`.  Their 8-byte slot starts are `0x0000` and `0x2501`, exactly the
captured external bind `6a=0x25010000` [BIN/CAP].

The phrase “0x1b000-byte database” therefore needs one qualification.  The
vendor defines and copies `0x1a040` bytes.  `0x1b000` is its page-rounded
allocation.  The clean-room artifact zeros the unused `0xfc0` tail; no shader
entry points into that tail.

The complete ordered descriptor list, chain offsets, append call sites, and
chain lengths are in `tools/m9-rsfragptrs-normal.json`.  The deterministic
extractor pins the complete source image, constructor, flattener, every append
call target, every 8-byte-aligned payload, both selected starts, and the final
hashes:

```text
defined 0x1a040 bytes
sha256 a032cd35c90399f9b1404734b68baca2cf4886150512552dc09cd435059df35b

padded 0x1b000 bytes
sha256 44db851ec46e487efb9f09e05eb3935ff96fdecc5dbb6bd90db8e1ea9b829539
```

## Vendor inline form for the first triangle

The same vendor driver has an inline path that copies only the selected
chains to instruction upload selectors `68/60..67`, rounds the result to a
32-byte line, writes literal `69=4`, and binds the rebased starts in `6a`.
For the captured first triangle the selected image is exactly:

```text
P00 size                 0x4c8
S28 size                 0x020
defined inline size      0x4e8
padded inline size       0x500
rebased starts           0x0000 / 0x0099
packed 6a                0x00990000
sha256 18d26d54045d13a2522df404278ed5c6fa2449d9dd32816c222fceaef2359493
```

This is the fully recovered vendor route for a first primitive while the
external allocation's GPU address translation remains unresolved.  It avoids
inventing a linear mapping for index 69.

`tools/m9-resource69-inline-evidence.py` also generates the compact,
submission-ready form of this route.  The header contains exactly 483
count-free dwords: forty `68/60..67` upload lines followed by the `69/6a`
bind.  It includes no captured draw stream or duplicated private framing, and
uses `u32` in a Linux kernel build and `uint32_t` in Mesa.  Its packed little
endian dword SHA-256 is
`7588059e96bc4b8ed40229df3c22bb8099d7f61a6ba3bda8d5a16a6c06888969`.

## Index 69 capture boundary

The external selector-69 publication path statically owns this immutable
fixed-fragment database [BIN].  The saved Linux allocation descriptor reports
address `0x11e42000` and extent `0x1b080`, but the bytes obtained by the old
address-directed snapshot alias prior drawable contents across controlled
runs [CAP].  Those bytes are not a linear copy of the database.  They also do
not prove a page table, lower root, or any other hardware mapping structure.

The repeated 0x80-byte pattern in that snapshot must remain an opaque physical
alias until a resource-aware dump or allocator translation trace joins its
addresses to the logical allocation.  A new driver must not copy the logical
database into the current 0x1b080 diagnostic buffer and assume index 69 can
fetch it.  The exact inline path above is the implementation-safe first draw
route.

## Reproduction

```sh
python3 tools/m9-rsfragptrs-extract.py \
  --defined-output /tmp/m9-rsfragptrs-defined.bin \
  --output /tmp/m9-rsfragptrs-padded.bin \
  --first-triangle-inline-output /tmp/m9-p00-s28-inline.bin
python3 -m py_compile tools/m9-rsfragptrs-extract.py
python3 tools/m9-resource69-inline-evidence.py --self-test

# Generate an identical header at either consumer's chosen path:
python3 tools/m9-resource69-inline-evidence.py \
  --emit-count-free-header /path/to/linux/drivers/gpu/drm/m9x/m9_resource69_inline.h
python3 tools/m9-resource69-inline-evidence.py \
  --emit-count-free-header /path/to/mesa/src/gallium/drivers/m9x/m9_resource69_inline.h
```

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
