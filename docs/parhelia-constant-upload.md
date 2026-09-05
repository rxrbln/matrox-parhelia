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

## The matrix path — key 0x20, fully decoded

The cross-check above turned out to be the important part: none of the four
single-triangle captures contains a key `0x43` record, but every one contains a
key **`0x20`** — matrices. That is how the fixed-function path gets its
transform to the hardware, and it is not a block load either.

`mtx558` table entry 0x20 (`.text+0x458cb`), per item:

```c
type = item[0];                            /* item is {u32 type; float m[16]} */
if ((type - 0x100) <= 0x1f) {              /* the WARP matrix slots */
    slot = type - 0x100;                   /* 32 slots */
    dst  = HW + 0x1ef8 + (slot + 2) * 64;  /* 64 bytes per slot */
    LocMemCpy(dst, &item[1], 0x40);        /* 16 floats, verbatim */
    HW[0x1bf4] |= 1 << slot;               /* per-slot dirty bit */
}
```

`HW[0x1bf4]` is one of the five dirty words `mtx1008` resets, which ties the
matrices into the same flush machinery as the constants.

So **both** kinds of WARP state — constants via key `0x43` and matrices via key
`0x20` — land in kernel-side shadows with per-slot dirty bits, and a later stage
moves them to hardware. Neither travels as a `0x41` block-load packet, and the
only `0x41` in the object is the self-test.

### What this means for the open driver

The block-load format recorded in `parhelia-block-state-map.md` and used by
`tools/parhelia-3dtri.c` has no counterpart in the vendor kernel. A userspace
driver that owns the ring has to reproduce whatever the flush stage emits, and
that emitter is the one piece of this chain still not identified — `mtx610`
packs dirty vec4s into a staging buffer with `LocMemCpy` and the buffer's
consumer has not been traced.

Two facts make that tractable:

* the matrix shadow is at a **fixed offset**, `HW + 0x1ef8 + (slot+2)*64`, so a
  live capture with `MTXTRACE_BUFS=1` around a matrix change will show the
  bytes moving;
* the six-group, stride-`0x40` structure of `mtx610` matches the block file's
  384 entries exactly, so whatever the flush emits is organised by those same
  six groups.

## `mtx3699`, decoded — matrices compact, constants drain through a ring

```c
HW[0xd60] = HW + 0x1f38;                  /* published pointer, slot 1's matrix */
n = HW[0x1ef4];                           /* active matrix slot count */
for (slot = 0; slot <= n; slot++) {
    src = HW[0x1c50 + slot*8];            /* per-slot source pointer */
    dst = HW + 0x1ef8 + slot*64;
    if (src != dst) {                     /* not already compacted */
        LocMemCpy(dst, src, 0x40);        /* pull the matrix into the slot array */
        HW[0x1c50 + slot*8] = dst;        /* and repoint at it */
    }
}
HW[0x1ef4] = 2;                           /* reset to two active slots */

c = HW[0x1e60];                           /* the constant object */
while (c[0xa4] != c[0xa8])                /* head != tail */
    mtx610(c);                            /* drain one group per call */
c[0xa0] = c[0xa8];                        /* commit */
```

So the constant object carries its own **head/tail ring of 64-byte groups**
(`+0xa0` committed, `+0xa4` head, `+0xa8` tail), and `mtx610` moves one group
per call — its `shl $0x6` on `[rdi+0xa4]` is that index times 64.

The matrix half is a **compaction**: each slot's pointer is followed and the 64
bytes pulled into a contiguous array at `HW + 0x1ef8 + slot*64`, so that
downstream code can treat matrices as one flat block. `HW[0xd60]` is then
published as a pointer into it.

Everything on this path is still software. The hardware emit is downstream of
`mtx610`'s staging buffer and is the single remaining unknown in the chain —
but the chain itself is now complete from the client record to the staging
buffer, in both the constant and matrix directions.

### Summary of the state model

| what | client record | kernel shadow | dirty | flush |
|---|---|---|---|---|
| constants | key `0x43` | `HW[0x1e60]`, 256 slots | `0x1b44`… | `mtx610`, 6 groups x 0x40 |
| matrices | key `0x20` | `HW+0x1ef8 + slot*64`, 32 slots | `HW[0x1bf4]` bit per slot | `mtx3699` compaction |

Neither is a ring packet. An open driver that owns the ring must supply whatever
the stage after `mtx610` emits, and that is the piece to find next.

## `mtx3136` is not the emitter either — a useful negative

`mtx3699` is called from `mtx3136` (`.text+0x1b670`), which is a straight-line
sequence of flush calls:

    mtx3699  mtx3924  mtx580  mtx950  mtx273  mtx1230  mtx3143  mtx912

Every one of the seven after `mtx3699` is tiny — 9, 57, 46, 40, 35, 12 and 12
bytes — and none contains anything with the shape of a register tag or a packet
literal. The whole chain is **software state management**: shadows, dirty bits,
pointer compaction.

So the hardware emit is downstream of `mtx3136` altogether, not inside it. That
narrows where to look next: the validation path `mtx3712` (`.text+0x188f0`),
which `parhelia-slot-translation.md` already documents as running in fixed order
immediately before every ring submit, and which is where the register packets
for depth, blend, raster and the rest are actually built.

**Where this leaves an open driver.** The constant and matrix data model is now
fully mapped from client record to staging buffer, and every stage of it is
software that a driver owning the ring simply would not have. What such a driver
needs is not this chain but the *shape of what it finally emits* — and that is
one function away, in a path already partly documented for other state.

## The raster path is dirty-marking too — and what that implies

`mtx3654` (`.text+0x8b510`, 7905 bytes), step 6 of the validation order and the
one `parhelia-slot-translation.md` describes as "raster -> block file AND
registers", calls only two functions in bulk:

* `mtx2702` x15 — a **bitmap range-set**: given a base and a first/last bit pair
  it ORs `0xffffffff << first` and `>> ~last` into the 32-bit words of a bitmap
  at `+0x04`, walking whole words in between. Pure dirty-marking.
* `mtx3301` x10 — four bytes: `return obj[0xc]`. An accessor.

So the raster path marks dirty ranges; it does not emit.

**Every stage examined is software.** Record consumers write shadows, `mtx1008`
and `mtx2702` mark dirty bits, `mtx3699` compacts matrices and drains a
head/tail ring, `mtx610` gathers dirty vec4s into a staging area at
`constobj+0x230`, and `mtx3136`'s remaining seven calls are all tiny and emit
nothing.

**A hypothesis this raises, offered as such.** If no host-side code emits the
block file, the WARP may read its constants **directly from memory** — the
staging area being a DMA source rather than something copied through the ring.
That would explain three otherwise awkward facts at once: no `0x41` packet
outside a RAM self-test, no register path to the block file, and a readback port
that is read-only. It also matches `parhelia-vertex-isa.md`'s note that the
uploaded key-0x45 image is a *descriptor* the kernel consumes rather than
microcode.

It is not established. What would settle it: find where `constobj+0x230`'s bus
address is handed to the hardware — most plausibly one of the write-only WARP
registers (`0x1000`, `0x1040`, `0x10b0`, `0x1100`, `0x1104`) whose values we
have never been able to read back.
