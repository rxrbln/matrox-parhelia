# How constants actually reach the WARP — and why our block loads fail

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

Read out of `mtx_parhelia.o` after hardware showed that `0x41` block-load
packets stop our ring executing. The answer is structural, not a framing detail.

## `0x4106000f` is the only block-load tag in the whole kernel

Every `0x41……` immediate in the object was extracted and filtered for
packet-tag shape (small payload length in the low 16 bits, small count in bits
22:17). Exactly one qualifies — `0x4106000f` — and all **four** of its uses are
inside `mtx676`, the WARP RAM self-test:

    a064b, a07f8, a09a7, a0b5d      the four walking-pattern packets

Everything else with that high byte is a float: `0x41200000` = 10.0f,
`0x41f80000` = 31.0f, and so on.

**So the kernel never emits a general block-load packet.** Building the constant
upload out of `0x41` packets — which this project has done from the start, from
a format recorded in `parhelia-block-state-map.md` — was copying a memory test.

## What the real path is

```
record key 0x43            client: constant-range upload
   -> mtx558 table entry 67  (.text+0x45462)
        first = payload[0], clamped to 0xff
        count = payload[1], clamped so first + count <= 0x100
        dst   = HW[0x1e60]              256-slot software shadow
   -> mtx1008 (.text+0x180a0)  marks dirty: 0x1b44, 0x1bd4, 0x1bec, 0x1bf0, 0x1bf4
   -> mtx3699 (.text+0x1b5c0)  -> mtx610 (.text+0x8f630)
        walks a SIX-group dirty bitmap, stride 0x40
        LocMemCpy's each dirty vec4, 16 bytes at a time, into a staging buffer
```

Six groups of `0x40` is 384 — exactly the block file's size, and exactly the six
bases `0, 0x40, 0x80, 0xc0, 0x100, 0x140` that `mtx676`'s packet lists. That
also re-reads the self-test packet: it is not four vec4s of data, it is two
patterns plus a **list of the six group bases**, i.e. a fill of the whole file.

So constants live in a kernel-side shadow, are packed into a staging buffer by
dirty group, and reach the hardware from there. A userspace tool driving the
ring directly has no equivalent of that stage.

## What this predicts, and what hardware already showed

| packet | measured |
|---|---|
| count-1 header-less `[0x41000003][vec4]` | **executes**, DWGSYNC retires past it |
| four such packets back to back | **executes** |
| count-4, our framing | stops the engine |
| count-4, `mtx676`'s verbatim `0x4106000f` | stops the engine |

Consistent with the above: a minimal `0x41` is tolerated, and the multi-vec4
forms are not something the engine is asked to parse from a ring in normal
operation, because the kernel never sends one.

**The open question is now narrow and concrete:** where does a count-1 block
load land? The header-less form has no destination field, so the file must have
a write pointer set elsewhere — most likely one of the WARP registers our bind
writes (`0x1000`, `0x1040`, `0x10b0`, `0x1100`, `0x1104`), all of which are
write-only and read back as 0.

That is answerable by RE rather than by scanning: find what `mtx610`'s staging
buffer is handed to, and which register selects its destination.

## Cross-check worth doing

`tools/parhelia-recdec.py` decodes the captured record streams exactly. None of
the four single-triangle captures contains a key `0x43` record — the fixed
function path uploads its constants some other way, or they are already resident
from device init. Establishing which would say whether an open driver needs this
path at all for a first triangle.
