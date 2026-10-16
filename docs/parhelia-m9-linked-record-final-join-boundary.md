# M9 linked-state to selector-6c final join boundary

> **Static follow-up:** `parhelia-m9-selector6c-constructor-static-closeout.md`
> identifies `m9x2048` as the final `+0xb8/+0xc0` publisher and reduces the
> remaining gap to the concrete `RsFragPtrsBuffer` commit callback. Its result
> supersedes this note's statement that static analysis could not name the
> semantic serializer boundary.

This note records the last statically closed boundary between the compiler's
linked state and the heterogeneous, 0x80-aligned resource selected by command
index `6c`. It also defines the smallest live probe that can identify the
remaining serializer without inferring a byte layout from one capture.
Addresses are virtual addresses. `[BIN]` marks direct static binary evidence,
`[CAP]` saved live evidence, and `[DER]` a conclusion constrained by those
facts.

The pinned inputs are:

* Linux `m9x_drv.so`, SHA-256
  `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`;
* Windows `MSerX/MxMD.dll`, SHA-256
  `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43`;
* all 17 cases in `captures/m9x-linux-gl2-special-20260914`, pinned by that
  fixture's manifest.

`tools/m9-linked-record-final-join-evidence.py` verifies every binary and
capture claim below. It only reads the vendor images and saved captures.

## Two operating systems build the same coherent host image

Linux `m9x10392` at `0x3d29a0` zeroes and constructs the exact 0x100-byte
image at state `+0x2d6`, which is linked nested object `+0xd9`. It compares the
complete image with the shadow at state `+0x7e8`, then publishes a generation
and dirty bit only when the bytes differ [BIN]. The exact final-writer map is
in `parhelia-m9-cfg-linked-image-re.md`.

Windows `0x1397e0` independently performs the same transaction [BIN]:

```text
destination             context + 0x248
zeroed extent            0x100
middle serializer        0x170ce0
polymorphic serializer   [context+0x86c].vtable[1]
upper serializer         0x1743d0
shadow                   context + 0x688
dirty publication        context+0x1b8 |= 0x00100000
generation               context+0x244
```

The Windows middle and upper serializers reproduce the Linux ranges and fixed
values, including the paired dwords at image `+0xa8..+0xe4`. Thus the host
image is a real cross-platform hardware state representation, rather than a
Linux-only software cache [BIN][DER].

## The visible static consumers stop before the final allocation

Linux backend translator `m9x11630` at `0x3ef9c0` consumes the linked state and
promotes its descriptor/object pair at `+0x301/+0x309` into backend fields
`+0xf0/+0xf8`. An instruction-by-instruction audit of its complete 4,180-byte
body finds no read of state `+0x2d6` or linked `+0xd9`; the verifier pins the
whole body and the positive pair reads [BIN]. This excludes `m9x11630` as a
verbatim host-image copier [DER].

The next attractive join is also excluded. `m9x24062` passes child `+0x40`
unchanged as the destination of `m9x21983`. That routine copies the compiled
pairs at `+0x2f1/+0x2f9` and `+0x301/+0x309` to destination `+0x98/+0xa0`
and `+0xb8/+0xc0`. Device construction identifies the destination as a
standalone, `0xd0`-byte `m9x31420` resource/link object [BIN].

The object ultimately emitted at `6c` is the distinct `m9x18369` program
state. Its constructor initializes its own pair `+0xb8/+0xc0` from the global
empty-resource sentinel. Final top-state dispatch sends `+0xd8` through
`m9x21922`, while it independently sends `+0xc8` through `m9x26001` and
`m9x2427` [BIN]. Matching offsets in the two types do not alias the objects.
This corrects the earlier tentative chain that joined `m9x21983` directly to
the final `6c` emitter [DER].

At the last visible boundary, `m9x2427` emits header `0x456c6b68`, literal
`68=0`, numeric `6b=program_state+0xc8`, and calls the command builder's
relocation method with [BIN]:

```text
resource pair   program_state + 0xb8 / +0xc0
descriptor      4
usage           0x1a
count           1
destination     command index 6c
```

Windows emitter `0xf78c0` has the same descriptor, usage, count and index.
Neither emitter reads the 0x100-byte host image [BIN].

## Captures disprove a literal copy

The Linux image constructor always places each of these eight invariant
dwords twice in sequence at host image `+0xa8..+0xe7` [BIN]:

```text
38800006 3ffff000 47800000 477fff00
3b800000 40800000 37800006 3ffff088
```

All 17 special-shader captures preserve the complete 0x21000-byte system
mapping immediately before and after submission. The two snapshots are equal,
so construction had finished before the ioctl. None of the eight invariant
dwords appears even once in any complete mapping, or in any selected `6c`
window [CAP]. This is stronger than failure to find the full 64-byte signature:
the final allocation cannot contain a verbatim copy of the host image [DER].

The selected offsets are distinct, monotonically allocated and 0x80 aligned;
the 17 windows have 16 distinct hashes and differing extents from `0x400` to
`0x580` [CAP]. Some carry instruction-shaped spans at `+0x100` or `+0x108`,
while others place different data there. The object is therefore constructed
from semantic state, native streams and referenced resources according to a
private variable layout. Treating `linked+0xd9` as the final record base, or
copying its 0x100 bytes into a captured record template, is invalid [DER].

## Ownership and lifetime now established

The compiler-linked object owns the coherent host image and generation shadow.
The `m9x31420` state owns promoted resource descriptors. The `m9x18369` state
owns the final descriptor/object pair consumed by `m9x2427`. The command
builder owns relocation of that pair into index `6c`; the selected allocation
remains byte-stable across the captured submission [BIN][CAP].

The saved monotonically advancing offsets do not establish reuse or retirement
of the 0x20080-byte allocation. They only establish that no reuse occurs in the
17-case fixture. A clean driver must retain the selected allocation and every
referenced resource until the submission fence retires, which is the proven
general command-resource lifetime rule documented in
`parhelia-m9-context-lifecycle-re.md` [DER].

## Smallest decisive live probe

Static analysis cannot name the semantic serializer between these object
views. The remaining question is the identity and call arguments of the code
that first writes a nonzero dword into the final mapped record. The saved
17-case process starts at offset `0x200`; one fresh baseline process with the
same allocation order should therefore be enough [CAP].

`tools/m9x-linked-record-final-join-gdb.py` implements the probe. Source it
before a fresh process reaches its first draw; it permits pending symbol
breakpoints when `m9x_drv.so` has not been mapped yet:

```gdb
set $m9_join_offset = 0x200
set $m9_join_map_length = 0x21000
set $m9_join_gpu_base = 0x40c3a000
set $m9_join_limit = 128
source tools/m9x-linked-record-final-join-gdb.py
continue
```

For an attached process whose system mapping already exists, obtain its CPU
mapping address from the existing `M9XTRACE_MAPS=1` log and set
`$m9_join_map_base` before sourcing. The watchpoint is then installed
immediately.

The script waits for the 0x21000-byte `mmap`, installs one four-byte hardware
write watchpoint at mapping `+0x200`, and automatically ignores initial zeroing
writes. It stops at the first nonzero write and prints the exact PC and a
24-frame backtrace. At every `m9x10392` return it saves the 0x100-byte host
image and logs the linked descriptor/object pair. At every `m9x2427` return it
saves the final five-dword `68/6b/6c` packet and, when the GPU base is supplied,
the selected record bytes.

The immutable capture set needed to close the join is only:

1. the first-nonzero-write PC, eight surrounding instructions and backtrace;
2. the saved host image and its SHA-256;
3. the logged final program-state `+0xb8/+0xc0` pair and five-dword packet;
4. the selected record dump at the matching offset.

If the watchpoint reaches its 128-write guard without a nonzero value, repeat
with offsets `0x204`, `0x208`, and `0x20c`; no MMIO capture or long application
run is required. If a fresh run selects a different first offset, the emitted
packet reports it exactly; repeat once with that offset. Once the writer is
named, its static callers and the two small baseline/delta record dumps are
sufficient to recover the constructor fields and relocation slots.

## Reproduction

From the repository root:

```sh
python3 tools/m9-linked-record-final-join-evidence.py
python3 -m py_compile tools/m9-linked-record-final-join-evidence.py \
    tools/m9x-linked-record-final-join-gdb.py
```

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
