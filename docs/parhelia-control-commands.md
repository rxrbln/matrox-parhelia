# The control-page command dispatch, and a retraction

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## The dispatcher

`mtx3683` (`.text+0x4de50`), reached from the `nr=33` doorbell:

```c
cmd = controlpage[0x00];                  /* dev[0xf0] is the control page */
if (cmd == 1) goto RENDER;                /* .text+0x4de90 */
cmd -= 3;
if (cmd > 0x1c) { control[0x234] = -1; return; }
jmp table[.rodata+0x1fe8 + cmd*8];        /* 29 entries, cmd 0x03..0x1f */
control[0x234] = status;                  /* 0 = ok, and the ONLY error channel */
```

Commands `0x07`, `0x08`, `0x13`, `0x17`–`0x1b` all jump to the reject arm, so
they are unimplemented in this build.

| cmd | handler | | cmd | handler |
|---|---|---|---|---|
| 0x01 | RENDER (inline) | | 0x0e | `.text+0x4df27` |
| 0x03 | `.text+0x4dec9` | | 0x0f | `.text+0x4df32` |
| 0x04 | `.text+0x4ded2` | | 0x10 | `.text+0x4df39` |
| **0x05** | **`mtx3112`** | | 0x11 | `.text+0x4df4a` |
| 0x06 | `mtx3019` | | 0x12 | `.text+0x4df5a` |
| 0x09 | `.text+0x4def2` | | 0x14 | `.text+0x4df6a` |
| 0x0a | `.text+0x4def9` | | 0x15 | `.text+0x4df7a` |
| 0x0b | `.text+0x4df07` | | 0x16 | `.text+0x4df8a` |
| 0x0c | `.text+0x4df12` | | 0x1c | `.text+0x4df9a` |
| 0x0d | `.text+0x4df19` | | 0x1d | `.text+0x4dfaa` |
|  |  | | 0x1e | `.text+0x4dfba` |

## RETRACTION: command 0x05 does not carry a pointer table

`docs/parhelia-minimal-triangle.md` and the commit that added the control-page
pointer capture (`b313598`) both state that command `0x05` "carries a table of
raw process pointers — client stack, heap, mmap — as ordinary payload", and
conclude that "byte-exact replay of the vendor stream is not achievable in
general". **That conclusion was wrong, and it was reached by looking at the data
instead of at the code that reads it.**

`mtx3112` (`.text+0x4f9a0`, 277 bytes):

```c
r13 = dev[0xf0];                  /* the control page          */
r12 = r13 + 0x10;                 /* the request, INLINE       */
... two capability probes via virtual calls ...
if (r12[0x08] != 4) ...           /* control+0x18 = memory type */
obj = new char[0x28];             /* a 0x28-byte request object */
mtx2024(obj);                     /* construct                  */
mtx938(obj, dev[0xd8], r12);      /* the three-heap allocator   */
```

`mtx938` is the allocator dispatcher already documented in
`parhelia-kernel-abi.md`: request type 1 → AGP, 2 → `__get_free_pages`
(DMA32), 4 → `vmalloc`. So **command `0x05` is a buffer allocation, and its
request lives inline in the control page at `+0x10`, spanning the 0x28 bytes the
handler allocates for its copy.** `control+0x18` is the memory-type field, and
the captured value there is `4` — vmalloc.

The 64-bit values at `+0x38`, `+0x40`, `+0x58` and so on that looked like a
pointer table are **client leftovers in a part of the control page the kernel
never reads for this command**. The page is 4 KB, the request is 0x28 bytes, and
the ICD does not clear the rest.

### What this means for the replay

* **No pointer rebuilding is needed for command `0x05`.** Replaying those bytes
  verbatim is correct, because nothing dereferences them.
* `control+0x10` is a pointer **only** for command `0x0c` — which the replay
  already gets right, since it keys on whether the tracer captured a readable
  struct at that address rather than on an address-range heuristic.
* The `CPTR` capture and rebuild added in `b313598` is therefore **not required
  for this path**. It is kept because it is cheap, off by default, and a
  genuinely pointer-carrying command may still turn up — but it is no longer the
  thing standing between us and a working replay.
* So the reason doorbell 4 hung is **still open**, and it is a narrower question
  than before: an inline vmalloc allocation request, replayed byte for byte,
  that does not return. The next thing to check is whether the allocation itself
  fails — `parhelia-kernel-abi.md` records that AGP is down on this box and the
  DMA32 path is capped at order 9 — and whether the handler's two virtual
  capability probes behave differently for a second process.

### The lesson worth keeping

The data in a control page does not tell you what the kernel reads out of it.
Four 64-bit values that decode as plausible userspace addresses are not a
pointer table until a disassembly shows something dereferencing them. Reading
`mtx3112` first would have saved the detour entirely.
