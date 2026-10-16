Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# Parhelia vertex fetch and GPU memory-space classes

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** `0x1040` does not carry the byte stride on the copy path a GL client uses — it is the placeholder `4`, with the per-vertex size in draw-header bits [22:17].
What the low two bits of every GPU address mean, how the vendor stack picks
them, what the vertex-fetch unit (`0x1000..0x1064`, `0x10b0..0x10b8`) is
programmed with, how the draw packets are framed, and — the reason this was
written — what our replay of the captured single-triangle stream must change
before it can produce pixels.

Evidence tags: **[BIN]** = read from a disassembled instruction (binary and
offset given, `.text`-relative for `mtx_parhelia.o`, VMA for the Windows PE
files), **[HW]** = seen in a capture from the running 0x0528, **[INF]** =
inferred, and said so.

Sources: `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` (obfuscated
`mtxNNNN` symbols, disassembly in `scratchpad/mtxk_r.asm`),
`xserver/7_0_0/mtx_drv.so`, `opengl6/lib64/libGL.so.1.3.0`
(`scratchpad/libGL64.asm`), and the Windows XP set in
`xddm32_210_00_105_se_u_nowhql/MtxPar/` (`MTXPARD.dll`, `MTXOGL.dll`,
`MTXPARM.sys`). Ground truth: `captures/live-ring-0528/` (`decoded.txt`,
`ringall.txt`, `vertices.hex`, `draw-replay.hex`).

---

## 0. The short version

1. **Bits [1:0] of a GPU address are a bus-cycle class, not a heap selector.**
   `0` = local (frame buffer), `2` = non-local memory reached with AGP cycles,
   `3` = non-local memory reached with PCI bus-master cycles. Nothing is
   registered for class 3: the kernel never programs a base, a limit or a
   translation table for it, and the address is the raw bus (== CPU physical)
   address. On the PCI-X 0x0528 (no AGP), class 2 is *downgraded to 3* by the
   kernel at every place a tag is produced, which is why every non-local
   pointer in the capture ends in `..3`.
2. **`0x1000` is stream 0's base address, tagged.** The kernel writes
   `0x1000 = resolve(vertex_buffer, offset)` where `resolve` returns
   `bus_base + offset | class`. In the captured kernel-internal draw the base
   is 0 with class 3, so the `0x97` packet's start/end are absolute bus
   addresses. In the ICD path the base is the vertex slot's tagged bus address
   and the `0x8f` ranges are vertex indices from it.
3. **The vertex fetch is not what is broken in the replay.** The replay
   reproduces the vendor's class and addressing exactly. What it does not
   reproduce is the **WARP program heap**: every `0x1100` / `0x10c8..0x10d8`
   value and three dwords inside the `0x41` block loads are
   `(bus_address / 3) | 3` pointers into a 1 MiB PCI-locked heap the vendor
   kernel allocated at context creation. The WARP fetches its instructions
   from that memory under class 3 at run time. After the vendor process is
   gone the heap is unallocated RAM, the transform/setup program is garbage,
   and no fragment is ever generated — with every sync and fence still
   retiring, exactly the observed symptom. §7 has the recipe.

---

## 1. Memory-space classes

### 1.1 Where the tag is made — three producers, one rule

| producer | input | output | evidence |
|---|---|---|---|
| `mtx1132` `(obj, offset)` — the address resolver every register emitter uses | `obj[0x18]` = 64-bit bus base, `obj[0x20]` = region type | `addr = base + offset`; type 1 → `addr \| 2`; type 2 → `addr \| 3`; otherwise `addr` (class 0) | [BIN] `mtx_parhelia.o` `.text+0x4c730..0x4c754` |
| `mtx1205` `(buf, type, size, kaddr, busaddr)` — attach memory to a command-buffer object | `type` | `buf[0x30] = type`; `buf[0x34]` (the END tag) `= type`, **except** type 2 becomes 3 unless `caps[0xe0] & 0x400` or `caps[0xe0] & 0x10` | [BIN] `.text+0x2e754..0x2e79f` |
| `mtx241` `(buf, region)` — attach a kernel region | `region[0x80]` through a 6-entry table at `.data+0xa60` = `{0, 0, 2, 3, 3, 3}` | same 2 → 3 downgrade on the same two cap bits | [BIN] `.text+0x2e7bc..0x2e81b`, table bytes at `.data+0xa60` |

`buf[0x58]` is `busaddr − kaddr` (`mtx1205` @`0x2e76e`), so a GPU address is
formed as `kernel_pointer + buf[0x58]` — i.e. the raw bus address — and then
OR'd with `buf[0x34]`. **[BIN]**

The two cap bits come from the device parameter block the GPL glue passes at
start (`mtx3229` @`0x24a2` copies `param[0x3c]` into `core[0xe0]`; the ABI
notes call that word `bm_flags`). Which bit is which AGP feature is not
recoverable from the binary; on the test box the kernel's own message says
`"PCI transfers available for read write"` and the capture shows only class 3,
so both bits are clear there. **[BIN]** for the copy, **[INF]** for the
naming.

### 1.2 The classes

| class | who gets it | bus cycle | translation | evidence |
|---|---|---|---|---|
| `0` | frame-buffer offsets (render targets `0x1220/0x1228`, textures `0x11dc`, cursor, the `0x8f`-path vertex slot when the ICD's ring is in VRAM) | local memory | none, offset from FB base | [BIN] `mtx1132` default arm; [HW] `0x1220=02000000`, `0x1228=018e8000` in the capture |
| `1` | never produced by any resolver | — | — | [BIN] no arm returns 1 |
| `2` | type-2 memory on a device with the AGP cap bits | AGP transactions | host GART, not the GPU | [BIN] `mtx1205`/`mtx241`; [INF] cycle type |
| `3` | type-2 memory otherwise (this box), and everything the WARP heap / pushbuffer / vertex data live in | PCI bus-master | **none** — raw bus address | [BIN] `mtx1205`; [HW] pushbuffer `19dc0000`, WARP heap `19f0xxxx`, vertices `19dc13f8`, our own DMA32 ring |

**Class 3 is raw system-physical.** Three independent facts pin it:

* the kernel module never writes any register in `0x0000..0x0130`, nor any
  "PCI heap base" — the complete enumeration of its literal and computed MMIO
  offsets is in `docs/parhelia-kernel-abi.md` §10 and the only ring registers
  it programs are `0x400`, `0x404`, `0x408`, `0x384` **[BIN]**;
* the primary-ring arm `mtx2508` writes `0x404 = start & ~3` and `0x400 = size`
  and nothing else address-related (`.text+0x3197b..0x31a52`), and our own
  ring at an arbitrary DMA32 physical address executes with `0x408 = end|3`
  **[BIN][HW]**;
* the vendor's vertex fetch in the capture reads `19dc13f8..19dc1434` — the
  gap between two secondary calls in its pushbuffer, whose bus address is its
  physical address (no IOMMU on the box) — under `0x1000 = 3` **[HW]**.

So for a driver: **a vertex buffer at any system-physical address below 4 GiB
is fetchable under class 3 with no registration at all.** The only alignment
the vendor imposes on the *heap* it allocates for such buffers is page
granularity (`mtx938` rounds the request to 4 KiB, `.text+0x4c4a8`); the WARP
program heap additionally rounds its base up to 0x180 bytes (§4).

### 1.3 Where each tag rides

| carrier | start dword | end dword | evidence |
|---|---|---|---|
| primary ring | `0x404 = start & ~3` (the tag is masked **off**) | `0x408 = end \| buf[0x34]` — `ring[0x9c] = base \| tag` is precomputed in `mtx2425` @`0x31b2c` and added to the cursor in `mtx1937` @`0x323f8` | [BIN] `mtx2508` @`0x3198b`, `mtx1937` @`0x32406` |
| secondary call (`0x20292820` → `0x10a0`, `0x10a4`) | `start \| ring[0x88]`, and `ring[0x88] = 2` is a constant written just before every kernel-emitted call | `end \| buf[0x34]` (`r8d` argument) | [BIN] `mtx1937` @`0x323c1..0x323df`; `mtx1443` @`0x5dcd`, `mtx3249` @`0x5e4d` |
| ICD-submitted secondaries (`mtx3257`) | taken verbatim from the request; `end & 3 == 0` selects a synchronous wait on `0x31c/0x318` | verbatim | [BIN] `.text+0x32f69`, `0x33007..0x3301f` |
| vertex stream 0 | `0x1000 = base \| class` | — | [BIN] `mtx2147` @`0x4b5fe..0x4b622` (`0x21212c00` tag, value from `mtx1132`) |
| WARP program entries `0x1100`, `0x10c8..0x10d8`, and entries stored in the block loads | `(bus/3 + 4·entry) & ~3 \| heap_tag` | — | [BIN] `mtx1650` @`0x9fbec..0x9fc39` |
| fence write-back `0x1090` | the vendor emits `1c951020` / `1a0bf000` with **tag 0** as ring packets | — | [HW] `ringall.txt` lines 353–355; **[INF]** the write-back unit does not use the class bits the way the fetch units do (our MMIO-programmed fence only fired with `\|2`, `docs/parhelia-session-2026-09-05.md`) |

The secondary-call **start** bit 1 is therefore not a memory class: the
kernel sets it to 2 on its own calls regardless of where the buffer lives,
while the memory class goes on **end**. Its hardware meaning is not
established here **[INF]**; our replay passes the vendor's pair through
unchanged, which is correct.

### 1.4 Allocation types and how they map

`mtx938` (`.text+0x4c470`) is the kernel's allocator switch, keyed by the
request's type word (`request+0x08`): `1` → `mtx3583` (AGP), `2` → `mtx586`
(PCI-locked `__get_free_pages`, DMA32), `4` → `mtx3185` (`vmalloc`), each
with the size page-rounded. The result object carries `[0x00]` kernel
pointer, `[0x10]` size, `[0x18]` bus address, `[0x20]` the type **[BIN]**.
Passed to `mtx1132` this gives type 1 (AGP) → class 2, type 2 (PCI-locked) →
class 3, type 4 (`vmalloc`) → class 0 — the last is safe only because vmalloc
memory (the record stream, control page) is CPU-consumed and never handed to
the engine by address **[BIN][INF]**.

The `.data+0xa60` table used by `mtx241` is indexed by a *region* field
(`region[0x80]`) and reads `{0, 0, 2, 3, 3, 3}`. Its index space is a
different enumeration from the allocator's; which value is `PCI_LOCKED` there
could not be settled from the binary alone. It does not matter for the
conclusion: every path that yields a non-local tag on this box yields 3.

### 1.5 The `0x0000..0x0130` bank and the `0x030..0x03c` "pull aperture"

These are **not** a heap registration and the vertex path does not use them.

* The kernel module never writes them (`docs/parhelia-kernel-abi.md` §10
  enumeration) **[BIN]**. They are written by the X driver: `mtx1373` clears
  the top nibble of slots 1..19 and programs slot 0 = `{0x10000000, 0,
  0x10000020, 0x00010001}` at first start (`docs/parhelia-engine-init.md`)
  **[BIN]**; the per-transfer writes in the triangle mmiotrace — `0x0030 =
  0x10f00000, 0x0034 = 0x018e8000, 0x0038 = 0x32030020, 0x003c = 0x00010020`,
  bracketed by `0x0030 = 0` and preceded by `0x314 = 1` — come from the same
  X server (`docs/parhelia-minimal-triangle.md`) **[HW]**.
* Their content is a **surface descriptor**, not an address window:
  dword 1 (`0x018e8000`) is the VRAM offset the capture later binds as texture
  base `0x1228 = 018e8000`; dword 2 (`0x32030020`) has the same shape as the
  `0x122c = 30030020` pitch/format word, whose top byte the X driver builds
  in `mtx1203` from the surface's pixel format (`0x20000000` for the 16-bit
  formats, `0x30000000` for 32-bit, `0x2e000000`/`0x3b000000` for two
  special formats, `mtx_drv.so` `0x6f634..0x6f690`) **[BIN]**; dword 3 is
  `height<<16 | width` (`0x00010020` = 1×32). Dword 0's top nibble is the
  arm/disarm bit the init loop clears **[BIN]**, over a VRAM byte offset
  (`0xf00000`, the same offset the X server `ioremap`s for CPU access)
  **[HW]**.
* Reading: a 20-slot table of host-visible surface windows the 2D/upload
  path uses for CPU-side texture and pixmap traffic **[INF]**. Nothing in it
  names system memory, and the vendor's 3D draw runs without it being
  touched (the trace shows only the two upload programs, then the ring
  doorbells).

### 1.6 Windows side

The display driver `MTXPARD.dll` emits the identical secondary-call packet
`[0x20292820][0][start | arg][end | arg][0]` at VMA `0xbfe93..0xbfebc` with
both tags supplied by the caller, exactly like `mtx1937` **[BIN]**. The
miniport `MTXPARM.sys` imports no register-write service and no AGP service
entry point beyond the registry names `AGPFlags/AGPDepth/AGPSafe/...` and
`TempBitNonLocalSize`/`ForceNonLocalTempBit` (`strings`), i.e. the AGP/
non-local heap is a DirectDraw heap decision with no GPU-side registration
**[BIN][INF]**. A full pass over the Windows heap code was cut short by the
session limit; nothing seen contradicts the Linux reading, and the Linux
reading is confirmed on hardware.

### 1.7 Answer to question 1

* `0x1000`'s value `0/2/3` and the low two bits of ring, secondary and
  `0x1090` addresses are the same field: **class 0 local, 2 AGP-cycle,
  3 PCI-cycle**, with 2 → 3 forced on a device whose `bm_flags` lack the AGP
  bits (this box).
* **Class 3 is raw bus-physical. No base, limit or table exists for it.** The
  only registration the engine ever gets is the primary ring's own
  `0x404/0x400`. A vertex buffer at any system-physical address is fetchable
  under class 3 as-is — and the vendor proves it every frame with vertices in
  its pushbuffer. Vertices do **not** have to be in VRAM; class 0 works too,
  with `0x1000 = vram_offset | 0` and offsets relative to the frame-buffer
  base.
* The thing that *does* need to be "registered" is not a heap but the
  **WARP program heap**: a 1 MiB class-3 buffer whose contents the WARP
  fetches at run time through `0x1100`/`0x10c8..0x10d8` (§4). Our replay
  provides the vertices and not the programs.

---

## 2. `0x1000..0x101c`, `0x1040..0x104c`, `0x1060/0x1064`

Decoded from `mtx1035`/`mtx3355`/`mtx1017` in
`docs/parhelia-draw-builders-kernel.md`; summarised here so this document
stands alone, with the memory-class reading added.

| register | tag | meaning | capture | evidence |
|---|---|---|---|---|
| `0x1000 + 4·s` | `0x00..0x07` | stream *s* base = GPU address of the bound buffer `+ (offset & ~3)`, low 2 bits = class | `2` = the copy path's placeholder (class-2 default of `mtx2729` @`0x2e617`), then `3` = `[[chan+0x58]+0x34]`, the pushbuffer's class word, base 0 | [BIN] `mtx1017+0x4bf`, `mtx2147` @`0x4b616` (`mtx1132`) |
| `0x1040 + 4·s` | `0x10..0x13` | **stride in bytes, two 16-bit streams per register** | `4`: a constant placeholder on the copy path; the real per-vertex size travels in the draw header bits [22:17] | [BIN] state `+0x28..+0x2e` as `u16[8]` in `mtx1035` |
| `0x1060` | `0x18` | number of vertices addressable in the bound buffer = min over streams of `(bufSize − offset) / stride` | `0x000280cc` = 163020 = `0x31c2f0 / 20` | [BIN] `ctx+0x258` |
| `0x1064` | `0x19` | first valid vertex index = max over streams of the stream's first index | `0` (reset value `0xffffffff` = nothing bound) | [BIN] `ctx+0x254` |

So `0x1060` is not a count for the draw and not a stride pair: it is a
**fetch limit**, and `0x1064` a **fetch floor**, the pair the hardware
clamps vertex indices against. In address mode (`0x97`) the start/end
operands already bound the fetch, which is why the vendor's copy path can
leave `0x1000 = base 0 | 3`, `0x1040 = 4` and still draw; what it cannot
leave is `0x1060 = 0`, which it re-emits as the buffer's capacity before the
draw **[HW]** (`decoded.txt` line 174).

For a direct-fetch driver (vertices left in place, `0x8f`/`0x1b` packets):
`0x1000 = bus | class`, `0x1040 = stride`, `0x1064 = 0`,
`0x1060 = bytes / stride`, and the packet's `first/last` are indices.

---

## 3. Attribute descriptors `0x10b4` / `0x10b8`

`0x10b4` is the write index (always reset to 0), `0x10b8` the data port,
one dword per attribute, packed by `mtx2714` (`.text+0xb6f0`, bit layout
in `docs/parhelia-draw-builders-kernel.md`):

| bits | field | `0x20c00000` | `0xa1c50020` | `0x83c00000` (kernel preamble) |
|---|---|---|---|---|
| [2:0] | stream | 0 | 0 | 0 |
| [11:4] | offset within the vertex, **dwords** | 0 | 2 | 0 |
| [16:12] | A — destination slot | 0 = position | 5 | 0 |
| [19:17] | B — format code | 0 | 2 | 0 |
| [25:22] | C — component mask `[INF]` | `0011` = xy | `0111` = xyz | `1111` = xyzw |
| [26] | ? | 0 | 0 | 0 |
| [29:27] | D — 4 on every ICD attribute | 4 | 4 | 0 |
| [31] | last attribute | 0 | 1 | 1 |

So the captured `[x,y,z,w,0]` vertex was declared by the ICD as **`xy`
position at dword 0 plus a three-component attribute (slot 5, format 2) at
dword 2** — the "z,w" are that second attribute's first two floats, and the
trailing 0 its third. It is not a 4-float position; the WARP program the
capture binds is written for that layout, and a driver that declares
`xyzw` must also bind a program that consumes `xyzw`.

Descriptors for the two requested layouts, on stream 0 at their natural
offsets, using the fields that are proven **[BIN]** and leaving A/B (whose
GL meaning needs the ICD side, not reached this session) as parameters:

| layout | descriptor 0 | descriptor 1 |
|---|---|---|
| `[x,y,r,g,b]` (float2 + float3, stride 20) | `0x20c00000` (xy, slot 0) | `0xa1c00020 \| A<<12 \| B<<17` — offset 2 dw, xyz, last; the captured colour-less stream used A=5, B=2 for a float3 there |
| `[x,y,z,w,u,v]` (float4 + float2, stride 24) | `0x23c00000` (xyzw, slot 0) | `0xa0c00040 \| A<<12 \| B<<17` — offset 4 dw, xy, last |

`0x83c00000` is the kernel's own one-attribute `xyzw` declaration, used by
its prime/self-test draws and by `mtx1814`'s internal draws **[BIN]**.

### 3.1 Packed colour: kind 5, format 6 **[HW, P690]**

The vendor D3D driver (faaemu `faa-default`) declares its diffuse colour as
one dword: `0x00456040` = offset 4, kind 5, bits `[15:12]` = 6, mask
`[25:22]` = 1.  `parhelia-drm-test --packed-colour-probe` (2026-10-13, P690)
draws an x,y + one-dword stream with `0x80456020`: a flat `0xc0804010` reads
back as exactly `0xc0804010` from an ARGB8888 target, and per-vertex
`0xff0000ff` / `0xff00ff00` / `0xffff0000` interpolate to blue, green and red
corners.  Format 6 is therefore D3DCOLOR, `0xAARRGGBB` with blue in byte 0,
unpacked to v5 as four channels including alpha.  GL RGBA8 bytes need R and B
swapped.  Mesa's transform path uses it on both families.  The full format
table (bits [15:12]: 8/16-bit, normalized and integer) is in
`parhelia-vertex-fetch-formats.md`.

## 4. The WARP program heap — the pointer class the replay missed

This is outside the five questions asked, but it is the finding that changes
the replay, so it is documented at the same standard.

### 4.1 Two program-residency modes

`engine[0xa3114]` selects how a WARP program is bound (`mtx2316`
`.text+0x9fc7b`) **[BIN]**:

| `engine[0xa3114]` | bind through | entry value written to `0x1100` / stage regs |
|---|---|---|
| 0 | `mtx1391` — stream 12-byte instructions through `0x918`/`0x91c` into the 0x1800-byte on-chip store | `store_byte_offset / 3 + 4·entry` (instruction index × 4), class bits 0 |
| 1 | `mtx1650` — program already copied into a **memory heap** | `((heap_bus + slot·0x600) / 3 + 4·entry) & ~3 \| (heap_tag & 3)` |

`mtx1650` `.text+0x9fbec..0x9fc39`: `slot = desc[0x08]`;
`addr = engine[0xa0648] + slot·0x600`; `addr = addr · 0xaaaaaaaaaaaaaaab >> 65`
(= `addr / 3`); `+ 4·desc[0x2c + 4·entryIdx]`; `& ~3`; `| engine[0xa0650] & 3`.
**[BIN]**

So a WARP entry is an **instruction address in units of one quarter
instruction** (12-byte instructions, address/12 × 4 = address/3), with the
memory class in the two low bits that the ×4 leaves free. For a
memory-resident program the unit's PC is a bus address divided by 3.

### 4.2 The heap

`mtx2565` (`.text+0x9fe94..0xa0023`) allocates it at engine start **[BIN]**:

* size `0x100` pages = **1 MiB**; allocator `mtx3583` (AGP) when
  `caps[0xe0] & 0x10`, with `engine[0xa0650] = (caps & 0x400) ? 2 : 3`; on
  failure, or when `caps & 0x100004 == 0x100004`, `mtx586` (PCI-locked
  pages) with `engine[0xa0650] = 3`; if no allocator applies,
  `engine[0xa3114] = 0` and programs are streamed instead;
* `engine[0xa0648]` = bus base rounded **up to a multiple of 0x180** (32
  instructions), `engine[0xa0640]` = the matching kernel pointer
  (`0x9ff86..0x9ffc4`);
* `0x2aa` = 682 slots of `0x600` bytes (128 instructions) with a 16-byte
  slot table at `engine+0xa0658` (`mtx2961` bounds the index at `0x2aa`,
  `.text+0x9f1fe`);
* `mtx1397` (`.text+0x9f5b0`) uploads a program: `slot = mtx2961(nslots)`,
  `memcpy(engine[0xa0640] + slot·0x600, desc[0x20], desc[0x18])`, marks
  `desc[0x01] = 1`, `desc[0x08] = slot`. Verbatim copy, no transformation.

### 4.3 What the capture's entries actually are

Decoding the captured values with §4.1 (`bus = (value & ~3) · 3`):

| where in the stream | value | bus address | class |
|---|---|---|---|
| `0x1100` (4 binds) | `08a57183` `08a57383` `08a57583` `08a57783` | `19f05480` `19f05a80` `19f06080` `19f06680` | 3 |
| `0x10c8/0x10d0`, `0x10cc/0x10d4` | `08a579bf`, `08a579cf` | `19f06d34`, `19f06d64` | 3 |
| `0x10d8` | `08a56b83` | `19f04280` | 3 |
| inside the first `0x41` block load (lines 57, 59, 61 of `draw-replay.hex`) | `08a56183` `08a55583` `08a56b83` | `19f02480` `19f00080` `19f04280` | 3 |

Every one lands in a 28 KiB window at bus `0x19f00080..0x19f06d64`, which
sits just above the pushbuffer (`0x19dc0000`) — the two consecutive DMA32
allocations of the vendor kernel. The pages the earlier session read from
`/dev/mem` at `0x08a56000`/`0x08a57000` (`captures/live-ring-0528/prog-*.txt`)
are **the wrong physical address** (value taken as a byte address instead of
×3), which is why they look like noise. **[HW]** for the values, **[BIN]** for
the decode.

Thirteen dwords of the replay image are therefore pointers into memory the
vendor kernel freed when the GL client exited. Whether the RAM still holds the
programs at replay time is luck; on a box that has run anything since, it
does not.

### 4.4 Consequence

The replay's assumption in `tools/parhelia-3dtri.c` (`tri_replay`) — "the WARP
programs the entries name were uploaded by the vendor kernel at context
creation and nothing clears the instruction store, so they are still
resident" — is false on this hardware: the store was never used
(`engine[0xa3114] = 1` on any box where the 1 MiB allocation succeeds, and a
full GL session shows zero `0x91c` writes, `docs/parhelia-kernel-abi.md`
§"What kernel-side MMIO a full GL session actually performs"). The programs
are fetched from bus memory, per bind, under class 3.

---

## 5. Draw opcodes

From `docs/parhelia-draw-builders-kernel.md` (kernel builders `mtx1017`,
`mtx3063`, `mtx1767/1796`, `mtx1443/3249`, `mtx2393`, `mtx1814`), with the
Windows display driver agreeing on the packet shape at the one site read
(`MTXPARD.dll` `0xbfe93`, the call packet). Header rule for every packet:
`low16 = totalDwords − 2`.

| bits | meaning | evidence |
|---|---|---|
| 31 | set on every GL-path draw (`0x97 = 0x17 \| 0x80`), never on kernel-internal draws (`mtx2393` writes bare `0x17000001`); from `ctx+0x42c` = `0x80000000` or `0xc0000000` | [BIN] `mtx3712+0x27e` |
| 30 | `layout.byte1 & 0x10` → `0xd7`; on the ICD side it is exactly the "decomposed quads / polygon-offset mode" flag `ctx[0xa4f2]` (`docs/parhelia-vertex-submission.md` §2.3) | [BIN] both sides |
| 28 | operands are (start, end) **addresses** (`0x17/0x1b/0x1f/0x97/0xd7`) vs inline payload (`0x03/0x07/0x0b/0x8f`) | [BIN] opcode table, [INF] bit naming |
| 27:26 | element kind: `00` vertices, `01` 16-bit indices, `10` 32-bit indices; `0x8f` = `0x03 \| 0x8c` is the ICD's index-*range* list | [BIN] |
| 24 | base draw bit (`0x03`) | [BIN] |
| 22:17 | **dwords per vertex − 1** for vertex packets (`(Σ stride>>2) − 1`); 0 for index packets. `0x97080001` → 4 → 5 dwords = the 20-byte vertex | [BIN] `mtx1017+0x67c` |
| 15:0 | inline vertices: `nVerts·dwPerVertex − 1`; inline indices: `nIndices − 1`; address mode: `1` | [BIN] |

Operands of the address form: `start` = bus address of the first vertex
byte, `end` = **exclusive** bus address after the last; both relative to
stream 0's base in `0x1000` (0 in the capture), class from `0x1000`. The
header is written *after* the vertex bytes in the vendor's pushbuffer, so
the vertices lie *before* the packet that names them — memory the engine
never fetches as commands **[BIN][HW]**. For the index address forms
(`0x1b/0x1f`) the low 2 bits of start/end additionally carry the half-dword
alignment of a 16-bit index list (`mtx2661`).

---

## 6. Primitive word `0x10b0`

`mtx3319` (`.text+0xb610`): `word = (word & ~0x7f) | mtx734[p]`;
`bits[14:12] = mtx3178[p]`; bit 17 always set; bit 18 = `(ctx+0x430 == 1)`.
Tables at `.data+0x40` / `.data+0x80` **[BIN]**:

| p | `mtx734` | `mtx3178` | primitive | `0x10b0` |
|---|---|---|---|---|
| 0, 1 | `0x20` | 0 | points | `0x00020020` |
| 2 | `0x3d` | 1 | lines | `0x0002103d` |
| 3, 4 | `0x3d` | 2 | line loop / strip | `0x0002203d` |
| 5 | `0x5c` | 3 | **triangles** | `0x0002305c` |
| 6 | `0x5c` | 4 | triangle strip | `0x0002405c` |
| 7, 8 | `0x5c` | 5 | fan / polygon | `0x0002505c` |
| 9 | `0x5c` | 6 | quads | `0x0002605c` |
| 10 | `0x5c` | 7 | quad strip | `0x0002705c` |
| 11 / 12 | `0x3d` / `0x5c` | 1 / 3 | decomposed-line / decomposed-triangle variants | as lines / triangles |

Low byte: `0x20` point, `0x3d` line, `0x5c` triangle rasteriser mode; the
init values `0x40` (X driver) and `0x60` (kernel reset), and the arm's
`0x10`, are the same field with no primitive selected **[BIN][HW]**.
Bits [14:12] = topology. Bit 18 (`0x0006305c`) = `ctx+0x430 == 1`, the
legacy per-vertex-register path (`0x1160..0x119c` planes); `mtx3712+0xa6d`
sets it to 2 when the packet tables are installed, so the vendor writes
`0x0002305c` first (packet path) and `0x0006305c` on the reset block that
precedes the copy-path draw. Which of the two the draw needs was not
separated on hardware; the replay carries both as captured **[INF]**.

---

## 7. What our replay must change to get pixels

The replay (`tools/parhelia-3dtri.c`, `tri_replay`) already gets the memory
class right: vertices in a DMA32 buffer, `0x1000 = 3`, absolute bus
addresses in `0x97080001 start end`, secondary call `start|2 / end|3`. That
part matches the vendor bit for bit and needs no change. What it must add,
in order:

1. **Relocate the WARP program heap, not just the vertices.** Allocate a
   class-3 buffer (any DMA32 pages, base rounded up to a multiple of `0x180`)
   and reproduce the vendor's heap window `0x19f00000..0x19f07000` in it:
   read those 28 KiB from `/dev/mem` **on a box where the vendor context is
   still alive** (or from the vendor kernel at capture time — the existing
   `prog-0x08a5*.txt` files are the wrong pages and must be discarded),
   copy them at the same offsets, and rewrite every entry in the image as
   `((new_bus + off) / 3) & ~3 | 3`. The 13 dwords: image indices 51, 56,
   58, 60, 66, 242, 256, 283, 291, 292, 294, 295, 296 of `draw-replay.hex`
   (values `08a5xxxx`), i.e. the four `0x1100` binds, `0x10c8/0x10cc/0x10d0/
   0x10d4/0x10d8`, and three dwords inside the first `0x41` block load.
   Keep the 3-alignment: `(value & ~3) · 3` must stay on a 12-byte instruction
   boundary, which the vendor guarantees by the `0x180` rounding.
   *Until this is done every other item is untestable — the sync/fence
   evidence that the stream "executes" says nothing about the WARP, which
   executes whatever those pages hold.*
2. **Verify the heap before the doorbell.** Compare the relocated bytes with
   the vendor's `0x19f0xxxx` content captured under a live context; a
   mismatch in the first slot (`0x19f00080`, the `08a55583` program) is
   fatal on its own.
3. Keep `0x1000 = 3`, `0x1040 = 4`, and re-emit `0x1064 = 0`,
   `0x1060 = capacity` exactly as the image does (line 174 of `decoded.txt`);
   do **not** zero `0x1060` — it is the fetch limit (§2).
4. Keep the attribute pair `0x10b8 = 0x20c00000, 0xa1c50020` **and** the
   `[x,y,(z,w,0)]` vertex layout together; changing the vertex to a real
   `xyzw` without changing the descriptors and the bound program (§3) draws
   nothing.
5. `0x10b0`: leave both captured writes (`0x0002305c` then `0x0006305c`).
6. Render-target/depth binds (`0x1220 = 02000000 | 0`, `0x1228 = 01000000`,
   class 0 VRAM) and the texture base `018e8000` are VRAM offsets and need no
   relocation, but the visible framebuffer is at VRAM 0 (memory note
   `parhelia-visible-framebuffer`): read pixels back at `+0x02000000`, not on
   the screen.
7. Fence: the vendor's `0x1090/0x1094` ring packets carry a tag-0 address;
   our validated instrument is the MMIO-written `0x1090 = addr|2`. Use ours
   for completion, and additionally poll the **render target** — that is the
   only signal that the WARP ran.
8. Only after 1–2 succeed: try `0x1000 = 0` with vertices in VRAM
   (`--vram` path), which needs no heap at all for the vertices, and the
   direct-fetch form (`0x1000 = bus|3`, `0x1040 = 20`, `0x8f` ranges) to
   drop the per-draw copy.

### What was not reached

* The GL meaning of descriptor fields A/B (slot / format codes for colour and
  texcoord) needs `MTXOGL.dll`/`libGL.so` — the sub-analysis was terminated by
  the session limit before reporting; §3 gives the formulas with A/B left
  symbolic.
* The Windows miniport's heap code was read only far enough to confirm it
  never programs a GPU-side aperture (§1.6).
* The hardware meaning of the secondary-call start bit 1 (§1.3).
