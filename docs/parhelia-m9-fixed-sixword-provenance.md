# M9 fixed-function six-word provenance

This note closes the bounded constructor for the six nonzero words in the
selector-6c image selected by fixed-state capture case B. It also corrects the
source address in the earlier raw-copy note. Addresses are virtual addresses.
`[BIN]` denotes pinned binary evidence, `[CAP]` saved capture evidence, and
`[DER]` a conclusion constrained by those facts.

## Direct producer-to-pool path

Linux `m9x25711` at `0x44bfa0` keeps the pipeline pointer in `r12`. Its
instruction at `0x44c0ad` computes `r12+0xe3c`, then copies `0x40` or `0x60`
bytes from that direct embedded state to pool image `+0x20` [BIN]. The object
loaded from `pipeline+0xd98` supplies branch and control inputs elsewhere in
the builder; it is not the base of this copy.

The Windows peer computes direct `pipeline+0xb44` at `0x13bad1` and copies it
to image `+0x20` [BIN]. Linux `m9x30457` at `0x44c940` and Windows `0x13a8c0`
also initialize the corresponding embedded ranges as 4-by-4 identities. The
matching copy and reset paths establish that `e3c` and `b44` are the same
cross-platform pipeline field [BIN][DER]. The pool then copies this image to
its backing byte for byte; it applies no inner fixups.

## Controlled-capture population

`tools/m9-selector6c-constructor-evidence.py` checks every one of the 33 saved
fixed-state pair directories. In every A process the copied `0x60` bytes are
zero. Cases 1-15 and 29-33 also have zero bytes in B. Thirteen separate B
processes have exactly six nonzero u32 values and no other nonzero copied byte
[CAP]:

| cases | B operation |
|---|---|
| 18-19 | alpha test |
| 20-23 | blending |
| 24-25 | color mask |
| 26-28 | depth state |
| 34 | culling |
| 37 | flat shade |

All thirteen images contain the identical layout [CAP]:

```text
image +24/+2c = 0x007ff880
image +4c/+54 = 0x007ff8a8
image +74/+7c = 0x007ff8d0
```

The six words therefore do not encode flat shading alone. Flat shading is one
trigger that selects a newly linked case-B image; unrelated state transitions
produce the same words in fresh processes. Their invariant pairing and stride
identify three linked-fragment references from a shared initial allocation
schedule [CAP][DER].

## Exact bounded constructor

The values have this exact arithmetic form [CAP]:

```c
limit = 0x00800000;
first = limit - 0x780;
for (i = 0; i != 3; ++i) {
        ref = first + i * 0x28;
        image[0x24 + i * 0x28] = ref;
        image[0x2c + i * 0x28] = ref;
}
```

An independent vendor-libGL initializer at `0x1414cd` writes `0x00800000` to
two limit fields and lays out adjacent `0x1000`/`0x801000` regions [BIN]. This
supports the 8 MiB bound, while the capture invariant supplies the exact
initial backoff and `0x28` stride [BIN][CAP][DER]. Static data flow has not yet
joined that initializer to the aliased writer of `pipeline+e3c`, so these
names describe the observed arithmetic rather than a recovered vendor type.

`m9_pool5_build_first_triangle_gouraud()` emits the common seven-float prefix
and a zero linked payload. `m9_pool5_build_first_triangle_case_b()` emits that
prefix plus the six values above. `m9_pool5_build_first_triangle_flat()` is a
convenience alias for the case-B constructor because case 37 uses precisely
that first-draw state. `m9_pool5_build_first_triangle_linked()` remains the
general fail-closed entry point for an independently linked `0x60`-byte
product.

The bounded constructor is exact when the linked arena begins in the saved
vendor first-process state. It must not be reused after arbitrary allocation
or for a different linked program without reconstructing the current cursor
and program references [DER].

## Need for a live watchpoint

A live source watchpoint is not required for the bounded Gouraud or case-B
first triangle: 13 fresh-process captures independently determine every
defined byte and the verifier reproduces them exactly. A watchpoint remains
the shortest way to name the upstream aliased writer and cursor for a general
allocator-independent linker. That broader provenance can also be closed by
static alias/data-flow recovery or controlled captures after deliberately
varying the arena cursor; live observation is therefore useful, not logically
unavoidable.

## Reproduction

```sh
python3 tools/m9-selector6c-constructor-evidence.py
python3 -m py_compile tools/m9-selector6c-constructor-evidence.py
cc -std=c11 -Wall -Wextra -Werror -x c -fsyntax-only tools/m9-pool5-image.h
```

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
