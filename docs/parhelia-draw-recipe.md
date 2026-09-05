# Parhelia draw recipe — the complete ordered sequence for one triangle

Copyright (C) 2026 René Rebe <rene@exactco.de>

What a driver must emit, in order, from `open("/dev/matrox/mtx0")` to a rendered
triangle, recovered from the vendor user-space stack and cross-checked against
live captures on a real Parhelia 0x0528.

This is a checklist, not a narrative. Every step says what to write, where, and
what it depends on. Where the vendor does something whose purpose is not
established, the step is still listed — marked **[PURPOSE UNKNOWN]** — because an
open driver may have to replicate it anyway.

**Tags.** `[CONFIRMED]` = read out of a live capture or out of the vendor code.
`[INFERRED]` = the best reading of the evidence, self-consistent but not proven.
`[UNRESOLVED]` = genuinely open; do not build on it without checking.

**Sources.**
`captures/mtxtrace-5draws.txt` (untextured, five `glFinish`-separated triangles —
the cleanest run and the primary source for §1 and §2),
`captures/mtxtrace-textured-submits.txt` (one texture upload + one textured
triangle — primary source for §3.3),
`captures/glsweep-0528-raw.txt` (per-GL-feature deltas — primary source for §3.1/§3.2),
`captures/mmio-draw-0528.txt` (kernel-side MMIO),
`matroxdriver-x86_64-cvs-20090511/{opengl6/lib64/libGL.so.1.3.0, kernel/mtx_parhelia.o, kernel/src/, xserver/7_4_0/mtx_drv.so}`.

Prerequisite reading: `docs/parhelia-kernel-abi.md` (ioctl ABI, DMA ring, fence
registers), `docs/parhelia-command-construction.md` (submission model),
`docs/parhelia-gl-feature-map.md` (per-feature registers),
`docs/parhelia-fragment-isa.md` (the fragment program blob).

---

## 0. The record grammar — settled from both ends

Everything in §1–§3 is expressed in one format, so it is stated first. It was
derived independently from the captures and from the kernel's own interpreter,
and the two agree.

The 136 KB region ("command record stream") is rebuilt **from byte offset 0 on
every submission**. It is a flat sequence of records:

```
record := u32 opcode ; u32 count ; count * <element of a per-opcode size>
```

Some opcodes are variable-length: their handler returns an extra byte count that
the interpreter adds on top of `8 + count*stride`.

Capture side: this framing decodes five independent streams end-to-end and lands
exactly on the byte length the driver publishes in `region0 + 0x230` —
`0x11f0`, `0x1188`, `0x1078`, `0x190`, `0x30`. Code side: `mtx558` @`.text+0x44ef0`
dispatches on `record[0]` (bounded at `0x51`) through the jump table at
`.rodata+0x1b08`, and every arm advances the pointer by `8 + count*stride`.
[CONFIRMED, both]

| opcode | stride | name | payload |
|---|---|---|---|
| `0x00` | 8 | NOP / skip | |
| `0x05` | 8 | PAIRS-05 | `(id, value)`, `mtx1734` @0x1abf0 |
| `0x06` | 8 | SAMPLER | `(id, value)`, `mtx3081` |
| `0x07` | 24 | PIPELINE-KEY | `key, 0, 0, ffffffff x3`; copied verbatim to `ctx+0x1e04`, **the kernel does not interpret it** |
| `0x08` | 32 | VERTEX-FORMAT | `0, 0, u64 key, stride, 0, mask, 0` |
| `0x0a` | 72 | TEXTURE-DEF | §3.3 |
| `0x0c` | 8 | STATE | `(state id, value)` — see the correction below |
| `0x0d` | 32 | ? | 8 dwords, constant in every capture |
| `0x0e` | 8 | TEXTURE-UNIT | `(u16 unit, u16 sub), value` |
| `0x12` | 16 | SCISSOR-RECT | `x, y, w, h` — screen pixels, **Y from the bottom** |
| `0x15` | 36 | CLEAR | see §3.5 |
| `0x1b` | 40 | DRAW | §2.2 |
| `0x20` | 68 | MATRIX | `sub_op, float m[16]`; **transposed in place by the kernel** |
| `0x21` | 8 + body | LIGHT | `(index, kind)` + a kind-dependent body |
| `0x22` | 72 | MATERIAL | `face, diffuse4, ambient4, specular4, emission4, shininess` |
| `0x23` | 16 | LIGHTMODEL-AMBIENT | 4 floats |
| `0x24` | 8 | **[PURPOSE UNKNOWN]** | always `3, 5` |
| `0x25` | 20 | CLIP-PLANES | 6 elements `plane_id, float4` -> `ctx+0x1da0` |
| `0x26` | 16 | VIEWPORT | `x, y, w, h`, same space as `0x12` |
| `0x27` | 8 | DEPTH-RANGE | 2 floats; **only the last element of the batch is applied** |
| `0x35` | 16 | **glBlendColorEXT** (constant blend colour) | 4 dwords, all zero in every capture -- consistent, the default blend colour is (0,0,0,0) |
| `0x36` | 8 | LINE/POINT-SIZE | 2 floats; last element only |
| `0x43` | 8 + n*16 | VERTEX-PROGRAM CONSTANTS | `start (<=0x100), count`, then float4s |
| `0x45` | 0x670 | **VERTEX**-program upload | `memcpy(ctx+0x1474, payload, 0x664)` |
| `0x46` | 0x678 | **FRAGMENT**-program upload | `u32 progid, u32 flags, u8 body[0x670]` |
| `0x51` | 8 | ? | one `(index, 0)` |

The full 82-entry table is in `mtx558`'s jump table; the rows above are the ones
that appear in a real draw.

### The single most important correction: `0x0c` pairs are NOT register writes

Both `docs/parhelia-gl-feature-map.md` and
`docs/parhelia-command-construction.md` decode a `0x0c` pair `(index, value)` as
"write `0x1000 + index*4`". **The kernel does not do that.** `mtx2617`
@`.text+0x19950` treats the first dword as a **driver state id**:

```
if (id in 1..0x100)   { ctx[0x10 + id*4] = value;            /* cached, skipped if unchanged */
                        if (id <= 0xab) <jump table .rodata+0xba0, 172 arms>(...); }
if (id in 0x101..0x200) { set/clear bit (id - 0x101) in the bitmap at ctx+0x414; }  /* glEnable/glDisable */
```

Each of the 172 ids has its own arm updating its own hardware shadow; the actual
register packets are built later by the dirty-state emitters
(`mtx3712` @0x188f0, `mtx606` @0x4200, `mtx2372` @0xcc30). **There is no
identity mapping from state id to register index.** [CONFIRMED]

The `0x1000 + index*4` path is real, but it lives one level lower: it is the
**DMA packet header encoding** (`mtx2434` @0x398e0), where a header byte *is* a
register index. Those are the bytes in `0x21212425`, not the dwords in a `0x0c`
record.

**Practical consequence.** The per-feature tables in
`docs/parhelia-gl-feature-map.md` are correct as *state ids* — `glDepthFunc`
really does emit id `0x17` with value `f - GL_NEVER + 1`, and that is what a
driver reproducing the vendor stack must emit. But the register names attached
to them (`0x105c` for depth func, and so on) rest on an assumption the kernel
contradicts, so a driver that skips the command stream and writes registers
directly **cannot** use them without first dumping the 172 arms of
`.rodata+0xba0`. This document therefore writes state as `id`, with the
previously published register name in parentheses and marked as unverified.
Two other consequences of the framing:

1. **`(0x0c, N)` is a record header, not `reg 0x1030 = N`.** This is the whole of
   the "`0x1030` (index 0x0c) is multiplexed or mis-sliced" ambiguity in
   `docs/parhelia-gl-feature-map.md` §6: `scissor`, `cullface`, `fog` and
   `lighting` show "`0x1030 = 1`" because each emits a one-entry STATE record.
   [CONFIRMED]
2. **`(0x05, N)` is likewise a header, not `reg 0x1014 = N`.** Feature-map
   contradiction #5 ("`glEnable(GL_BLEND)` writes `0x1014 = 4`") is a mis-framed
   `(0x05, 4)` header. The real writes are in §3.2. [CONFIRMED]

### The clobbered first record — resolved

`glsweep` reads the stream after `glFinish` and always sees `(0xff, 1)` in the
first two dwords. That is the ICD re-seeding the buffer with the empty-list
sentinel after the flush returns (`libGL` @0x1c9be0); it is visible in
`mtxtrace` as a lone 4-byte change to offset `0x000` in every submission that
*follows* a render. `mtxtrace` snapshots around the doorbell instead and
therefore captures the **real** first opcode. The "unrecoverable first token"
caveat in `docs/parhelia-command-construction.md` does not apply to mtxtrace
captures. [CONFIRMED]

---

## 1. Initialisation — device open to "ready to draw"

Two layers have to be up. §1.1 is the hardware arm, which on the vendor stack is
done once by the X driver plus the kernel core. §1.2 is the per-context sequence
the GL client itself performs — **seventeen submissions before the application's
first GL call** — which is the part that was missing.

### 1.1 Hardware arm (once per device)

Ordered exactly as `captures/mmio-draw-0528.txt` shows it. All of this is
already documented in `docs/parhelia-kernel-abi.md` §9 and §11; it is repeated
here only as the ordered prerequisite list.

```
 1  R 0x450                                  read the FIFO/status word first
 2  W 0x10b0 = 0x00000010                    bus-master arm
 3  W 0x12b8 = 0x00070601                    bus-master configuration
 4  R 0x300 ; W 0x300 = (v & 0xe0e0ff3f) | agpbits | 0x0100 [| quirks]
 5  W 0x384 = 0                              DMAPAD (see the 128-byte rule below)
 6  W 0x308 = 0x83                           bus-master enable (0x83 on 0x0528, else 0x01)
```

Then, before any command can be fetched:

```
 7  W 0x314 = 1            (x4)              kick  [PURPOSE UNKNOWN]
 8  R 0x8f0  until bit 17 (0x00020000) set   primary DMA idle
 9  R 0x450                                  FIFO drain check
10  W 0x404 = ringBase                       PRIMADDRESS, & ~3
11  W 0x400 = statusWriteBackAddr            engine write-back pointer, see §4.6
12  W 0x314 = 1            (x8)
13  W 0x920 = 1                              WARP enable
14  W 0x0000..0x0130                         aperture / surface descriptor bank,
                                             20 slots x 4 dwords {base, size, 1, 0x10001}
15  W 0x920 = 1                              WARP enable, again
16  W 0xbc0 = 0x4087                         memory configuration
17  W 0x408 = ringBase | 0x23                first PRIMEND — the ring is now live
18  W 0x8fc = 0x00010000                     IRQ enable mask
```

Notes a driver author needs:

* **WARP microcode.** `0x91c` is *not* written in that trace, because mmiotrace
  only sees the kernel's `ioremap` mapping. The loader is `mtx1391` @0x9f6b0:
  full idle wait (`0x8f0` bit 17 -> `0x450` -> `0x908 == 0x63a78001` ->
  `0x924 & 0xfff == 0x100` -> `0x8f0 & 0x03000000 == 0`), then
  `0x918 = byteOffset / 12`, then three dwords per instruction to `0x91c`, then
  `0x920 |= 1`. Exactly one image exists: 4812 bytes, 401 instructions,
  <!-- Reader trap: the [HW] section below reports "loaded 7 WARP programs, 2664
  bytes (222 instructions)".  That is NOT a second microcode version -- 4812 is
  the mtx2785 self-test image, 2664 is the seven real boot programs extracted by
  symbol.  Both figures are right and they describe different objects.  The
  "two microcode revisions" reading was retracted; see parhelia-vertex-isa.md. -->
  md5 `b29644ecfcfcbbc11ef97240872892ca`, extractable with
  `tools/parhelia-extract-ucode.c`. [CONFIRMED]
* **The DMAPAD rule applies to every register write.** If the previous register
  offset written was within 128 bytes *below* the new offset, write `0x384 = 0`
  first. A register *read* cancels the pending pad.
* **`0x408` advances by 0x20 per submission in the trace** (`0x14600023`,
  `…43`, `…63`, …), while `mtx1937`'s disassembly increments the ring write
  pointer by `0x14`. One of the two readings is incomplete. [UNRESOLVED]
* **`0x400` is not a limit.** It receives the physical address of the shared
  status page, giving the engine a write-back target — see §4.6, which corrects
  `docs/parhelia-kernel-abi.md` §6.4.
* The GL client **never maps BAR1**: `captures/mtxtrace-userspace-writes.txt`
  shows seven mappings, none of them the register region. All register traffic a
  GL client causes goes through the command stream and the kernel. [CONFIRMED]

### 1.2 The seventeen pre-draw submissions

Every one of these is `ioctl(fd, 0x4621)` — zero-sized, no argument. The payload
is the 4096-byte control block (region 0). Ordering, opcodes and payload values
are read out of `captures/mtxtrace-5draws.txt`; the textured run produces the
identical sequence. The ICD-side function names come from the RTTI type strings
embedded in `libGL.so.1.3.0` (`11ICDLinkImpl`, `12GLBufferImpl`,
`14GLResourceImpl`, `21GLObjectBufferManager`, `19GLPrimitiveHardware`).
[CONFIRMED]

#### The two submission wrappers

`ICDLinkImpl` has one generic control-op submit and one dedicated RENDER.
[CONFIRMED — `libGL.so.1.3.0` @0x358820 and @0x3589b0, vtable @0x696d50]

```
/* @0x358820 -- every control op goes through here */
int Submit(ICDLinkImpl *this, u32 op, void *payload220)
{
    if (this->IsLost() && op != 3)                 return -3;
    pthread_mutex_lock(&g_lock);
    if (op > 0x1f || !((1u << op) & 0xf07ffe78))   goto fail;
    CB[0x00] = op;
    if (payload) memcpy(CB + 0x10, payload, 0x220);
    ioctl(fd, 0x4621, NULL);
    if (payload) memcpy(payload, CB + 0x10, 0x220);
    pthread_mutex_unlock(&g_lock);
    return CB[0x234];
}

/* @0x3589b0 -- RENDER */
int Render(ICDLinkImpl *this, GLContext *ctx, void *base, u32 cmdLen)
{
    if (this->IsLost()) return -3;
    lock;
    ctx[0xbc84] = ctx[0xbc70];        /* publish this submission's fence value */
    CB[0x10]  = 0;                    /* the command list always starts at 0   */
    CB[0x230] = cmdLen;
    CB[0x00]  = 1;
    CB[0x08]  = ctx[0xbc58];          /* the rendering-context handle          */
    if (drawable needs validation) { Validate(); goto retry; }   /* emits op 0x1d */
    ioctl(fd, 0x4621, NULL);
    unlock;
    return CB[0x234];
}
```

Two consequences a driver author will trip over otherwise:

* **A control op writes only `CB[0x00]` and `CB[0x10..0x22f]`.** `CB[0x08]` and
  `CB[0x230]` are never touched by anything but RENDER, so a control op observed
  carrying them in a trace is showing leftovers.
* **The op-0x1d in the middle of the init sequence is nested inside `Render`** —
  it is drawable revalidation issued from the retry path, which is exactly why it
  appears with `CB[0x08]` and `CB[0x230]` already filled in.
* The payload stack buffer in `GLBufferImpl::Alloc` is **not** zeroed, which is
  why unused control-block slots hold garbage (`CB[0x14] = 0x7fff`).

#### The sequence

| # | op | ICD function | key inputs | outputs | what it does |
|---|---|---|---|---|---|
| — | — | — | — | — | `open("/dev/matrox/mtx0")` |
| — | — | — | — | — | `ioctl 0x80304600` GET_MEMORY_INFO |
| — | — | `ICDLinkImpl::Open` @0x358c30 | — | cookie at out+8 | `ioctl 0xc038461f` (nr=31), then `mmap64(cookie, 0x238)` -> **region 0**, the control block |
| 0 | `0x0c` | `GLResourceImpl::Create` @0x357d20 | `CB[0x10]` = user pointer to a 0x1c-byte descriptor | `CB[0x18]` = handle **1** | create the **front** window surface |
| 1 | `0x0c` | ditto | | handle **2** | the **back** surface |
| 2 | `0x0c` | ditto | | handle **3** | the **Z/aux** surface |
| — | — | — | — | — | `mmap64(0x1000)` -> region 1 |
| 3 | `0x05` | `GLBufferImpl::Alloc` @0x3579c0 | `CB[0x10]` = `0x2106c` size, `CB[0x18]` = 4 (SYS) | `CB[0x10]` = `0x22000` rounded size, `CB[0x20]` = buffer handle, `CB[0x28]` = **mmap cookie** | allocate the 136 KB command-record buffer |
| — | — | — | — | — | `mmap64(cookie, 0x22000)` -> **region 2** |
| 4 | `0x0a` | GLX context create @0x35ac60 | `CB[0x18]` = a qword from the new context object, `CB[0x20]` = 2, `CB[0x28]` = 3 (the back and Z surface handles) | `CB[0x30]` = **context handle**, `CB[0x48]` = **mmap cookie of a 4-byte page** | create the rendering context and its shared fence word |
| — | — | — | — | — | `mmap64(CB[0x48], 4)` -> **region 3**, the fence/serial page |
| 5 | `0x14` | `GLObjectBuffer::Create` @0x1c8ee0 | `CB[0x10]` = host pointer, `CB[0x18]` = `0xa0` size | `CB[0x10]` = device address written back | register a 160-byte host block as a device-readable object buffer |
| 6 | `0x14` | ditto | | | the second one |
| 7 | `0x1e` | GLX bind @0x35b44a | `CB[0x10]` = context handle 1, `CB[0x18]` = `0x800002` drawable id | — | **attach the drawable to the context** |
| 8 | `0x1d` | drawable validate @0x35a120 | `CB[0x10]` = `0x800002`, plus the rect | — | validate/refresh the X drawable (nested in `Render`) |
| 9 | **`0x01`** | `Render` @0x3589b0 | `CB[0x08]` = 1, `CB[0x10]` = 0, `CB[0x230]` = `0x11f0` | serial page -> 1 | **execute the default-state stream** (§1.3) |
| 10 | `0x05` | `Ring::Init` @0x1b1ec0 | `CB[0x10]` = `0x320010`, `CB[0x18]` = 1 | fails | 3 MB vertex ring, VRAM class |
| 11 | `0x05` | ditto | `CB[0x18]` = 2 | fails | retry, PCI-locked |
| 12 | `0x05` | ditto | `CB[0x18]` = 3 | fails | retry, AGP |
| 13 | `0x05` | ditto | `CB[0x10]` = `0x321000`, `CB[0x18]` = 4 | cookie | retry, SYS — **succeeds** |
| — | — | — | — | — | `mmap64(0x320000)` -> **region 4**, vertex ring |
| 14 | `0x05` | ditto | `CB[0x10]` = `0xa9000`, `CB[0x18]` = 4 | cookie | 672 KB command ring |
| — | — | — | — | — | `mmap64(0xa8000)` -> **region 5** |
| 15 | `0x05` | ditto | `CB[0x10]` = `0x401000`, `CB[0x18]` = 4 | cookie | 4 MB texture heap |
| — | — | — | — | — | `mmap64(0x400000)` -> **region 6** |
| 16 | `0x04` | `Ring::Reset` @0x1b2260 | none | — | **wait for engine idle**, then mark every 8 KB ring page's fence tag free |

Then, and only then, the application's first GL call runs.

Five things in that table are worth calling out.

**Buffer create is opcode `0x05`; `CB[0x28]` is the mmap cookie.** The cookie is
not a physical or GPU address — it is a handle the kernel hands back
(`mtx_mmap` matches `vm_pgoff << PAGE_SHIFT` against `region->base` with `==`).
The ICD asks for `size + 0x10` and the **kernel writes the page-rounded size back
into `CB[0x10]`**, which the ICD then reuses on a retry: that is why the vertex
ring appears first as `0x320010` and afterwards as `0x321000`. `CB[0x20]` is the
buffer handle, the key for the free op `0x06`; in practice it is a kernel virtual
address (`0xffffc200_…`). [CONFIRMED — the cookies match the following `mmap64`
in both captures]

**The memory-type enum in `CB[0x18]` is the kernel's `mtx_memory_type` plus one**
(`kernel/src/mtx_mem_type.h`: IO/LOCAL 0, PCI_LOCKED 1, AGP 2, SYS 3), so the
ICD's preference list 1, 2, 3, 4 means VRAM -> PCI-locked -> AGP -> system, and
`4` = system memory. [INFERRED, but consistent at every call site]

**Submissions 10–12 are the vendor's own fallback walk, not a bug.** The 3 MB
request fails as class 1, 2 and 3 in turn — `MTX_MAX_ORDER` is 9, so 801 pages is
over the contiguous limit, and the box has no AGP — and the kernel logs
`Failed to allocate locked pages (nb: 801, type: 2)` /
`Failed to create device buffer (size: 3280896, type: 1)`.
`3280896 == 0x321000`, exactly the size submission 13 succeeds with. An open
driver should allocate the vertex ring from vmalloc'd (scatter) memory and not
chase the contiguous path. [CONFIRMED — log text, sizes, class numbers and the
retry count all agree]

**The fence page comes from the context-create op `0x0a`, and its mmap cookie
doubles as its bus address.** It is `MTX_MEMTYPE_PCI_LOCKED`, for which
`region->base = __pa(kaddr)`, and the value that later reaches register `0x1090`
in every draw packet is *exactly* that cookie (`0x188a4000` in the 5-draw run,
`0x18c4a000` in the textured run). [CONFIRMED — the cookie, the mmap offset and
the `0x1090` payload are the same dword]

**Op `0x14` is not what it looks like.** The largest command-stream write of the
run (3372 bytes) lands in the snapshot attributed to submission 5, and op `0x14`
was the obvious suspect. It is not: op `0x14` is `GLObjectBuffer::Create`, the
driver-internal implementation of `ATI_vertex_array_object` — the enum checks in
`glNewObjectBufferATI` (`GL_STATIC_ATI 0x8760` / `GL_DYNAMIC_ATI 0x8761`) settle
it — and its payload is a 160-byte *host* block, allocated twice for the two
`GLPrimitiveHardware` objects the context binds. The 3372 bytes are ordinary
command-list construction by plain CPU stores into region 2, which is only ever
flushed by op `0x01`. Snapshot-based attribution to a submit is an artefact.
[CONFIRMED]

#### The full control-op vocabulary

For completeness, since a driver must implement the kernel side of whichever ops
it wants to support. Ops `1`, `3`–`6`, `0x0a`–`0x16`, `0x1c`–`0x1f` are the ones
this ICD can emit; op `0x09` passes the client-side mask but has no call site.
[CONFIRMED]

| op | meaning |
|---|---|
| `0x01` | RENDER — execute the command list |
| `0x03` | Flush/Finish; publishes the fence value. **The only op allowed while the link is lost** (§5) |
| `0x04` | wait for engine idle |
| `0x05` / `0x06` | allocate / free a device buffer |
| `0x0a` / `0x0b` | create / destroy a rendering context |
| `0x0c` / `0x0d` | create / destroy a surface resource |
| `0x0e` / `0x0f` | lock / unlock a resource (returns an mmap cookie + offset) |
| `0x10` | 2D blit or fill between resources |
| `0x11` / `0x12` | create / release a vertex-stream binding object |
| `0x13` | upload a compiled **vertex** program |
| `0x14` / `0x15` / `0x16` | create / free / update an object buffer |
| `0x1c` | page-flip present |
| `0x1d` | validate the X drawable |
| `0x1e` | attach (`CB[0x18]` = drawable) or detach (`CB[0x18]` = 0) a drawable |
| `0x1f` | read back a rect from a resource into host memory |

Note the split: the **fragment** program travels in-band as a `(0x46,1)` record
in the command stream (§3.3), while the **vertex** program has its own control op
`0x13`. The capture never exercises `0x13` — the fixed-function path uses the
resident WARP microcode.

### 1.3 The default-state stream (`0x11f0` bytes, executed by submission 9)

Decoded end to end; the framing lands exactly on `0x11f0`. [CONFIRMED]

```
0x0000  (0x51,  1)   TARGET-SELECT  index 2
0x0010  (0x0c,108)   core render state          -- 108 pairs, table below
0x0378  (0x0e,112)   texture environment        -- 4 units x 28 sub-indices
0x0700  (0x05, 24)   state group 05             -- 24 pairs
0x07c8  (0x06, 36)   sampler state              -- 4 units x 9 sub-indices
0x08f0  (0x51,  1)   TARGET-SELECT  index 1
0x0900  (0x0c,108)   ... the identical four blocks again, for target 1
0x0c68  (0x0e,112)
0x0ff0  (0x05, 24)
0x10b8  (0x06, 36)
0x11e0  (0x0c,  1)   state id 0x3e = 1
```

So: **the whole default state is emitted twice, once per render target**, and
the `0x51` record selects which. `CB[0x08]` at the RENDER is 1, matching the
second block. [INFERRED — the two-copies-with-a-selector structure is plain in
the capture; that the selector means "render target" is the reading, not a proof]

State id `0x3e` closes the block with 1, and is the id `SwapBuffers` toggles
(§3.5). [CONFIRMED]

#### Default render state, record `0x0c`

Emit all 108 entries in this order. `id` is what the record carries; "published
reg" is the register name earlier documents attach to it, kept here for
cross-reference only — see the correction in §0, it is **not verified** and the
kernel does not perform that mapping.

| id | published reg | value | meaning |
|---|---|---|---|
| 0x07 | 0x101c | 0x0 | depth test off |
| 0x08 | 0x1020 | 0x3 | polygon mode FILL |
| 0x09 | 0x1024 | 0x2 | blend src RGB = ONE |
| 0x0a | 0x1028 | 0x1ffff |  |
| 0x0c | 0x1030 | 0xc |  |
| 0x0e | 0x1038 | 0x1 | depth write enable |
| 0x0f | 0x103c | 0x0 | alpha test off |
| 0x10 | 0x1040 | 0x0 |  |
| 0x13 | 0x104c | 0x2 |  |
| 0x14 | 0x1050 | 0x1 | fragment program select |
| 0x16 | 0x1058 | 0x3 | cull face disabled |
| 0x17 | 0x105c | 0x4 | depth func LEQUAL |
| 0x18 | 0x1060 | 0x0 | alpha reference |
| 0x19 | 0x1064 | 0x8 | alpha func ALWAYS |
| 0x1a | 0x1068 | 0x1 |  |
| 0x1b | 0x106c | 0x0 | blend off |
| 0x1c | 0x1070 | 0x0 | fog off |
| 0x1d | 0x1074 | 0x0 |  |
| 0x1e | 0x1078 | 0x0 |  |
| 0x22 | 0x1088 | 0x0 |  |
| 0x23 | 0x108c | 0x0 |  |
| 0x24 | 0x1090 | 0x0 | fence write-back address |
| 0x25 | 0x1094 | 1f | fence value |
| 0x26 | 0x1098 | 0x1 |  |
| 0x28 | 0x10a0 | 0x0 |  |
| 0x29 | 0x10a4 | 0x0 |  |
| 0x2f | 0x10bc | 0x0 |  |
| 0x30 | 0x10c0 | 0x0 |  |
| 0x34 | 0x10d0 | 0x0 | stencil off |
| 0x35 | 0x10d4 | 0x1 |  |
| 0x36 | 0x10d8 | 0x1 |  |
| 0x37 | 0x10dc | 0x1 | stencil op |
| 0x38 | 0x10e0 | 0x8 |  |
| 0x39 | 0x10e4 | 0x0 | stencil reference |
| 0x3a | 0x10e8 | 0xffffffff | stencil mask |
| 0x3b | 0x10ec | 0xffffffff |  |
| 0x3c | 0x10f0 | 0x0 |  |
| 0x40..0x5f | 0x1100..0x117c | 0x0 | |
| 0x80..0x87 | 0x1200..0x121c | 0x0 | |
| 0x88 | 0x1220 | 0x1 |  |
| 0x89 | 0x1224 | 0x1 | lighting off |
| 0x8a..0x8d | 0x1228..0x1234 | 0x0 | |
| 0x8e | 0x1238 | 0x1 |  |
| 0x8f | 0x123c | 0x0 |  |
| 0x91..0x94 | 0x1244..0x1250 | 0x0 | |
| 0x97 | 0x125c | 0x0 |  |
| 0x98 | 0x1260 | 0x0 |  |
| 0x99 | 0x1264 | 0x0 |  |
| 0x9a | 0x1268 | 1f |  |
| 0x9b | 0x126c | 1f |  |
| 0x9c | 0x1270 | 0x0 |  |
| 0x9d | 0x1274 | 0x0 |  |
| 0x9e | 0x1278 | 1f |  |
| 0x9f | 0x127c | 0x0 |  |
| 0xa0 | 0x1280 | 0x0 |  |
| 0xa1 | 0x1284 | 0x0 |  |
| 0xa2 | 0x1288 | 0xffffffff |  |
| 0xa3 | 0x128c | 0x0 |  |
| 0xa4 | 0x1290 | 0x0 |  |
| 0xa6 | 0x1298 | 4096f | GL_POINT_SIZE_MAX (NOT guard band -- see note) |
| 0xa7 | 0x129c | 0x0 |  |
| 0xa8 | 0x12a0 | 0xf |  |
| 0xaa | 0x12a8 | 1f |  |
| 0xab | 0x12ac | 0x1 |  |

Note `id 0x40..0x5f = 0` here, whereas the *first draw* stream sets the same 32
ids to `0xffffffff` (§2). Both values are real; that block of state is written
twice with different content. [CONFIRMED, purpose of the difference UNKNOWN]


> **Note on slot 0xa6 / register 0x1298.** This was previously glossed here as
> "guard band / viewport scale" in two places.  That reading is **retracted**.
> Slot 0xa6 belongs to the point-size group (0x9a size / 0x9b min / 0xa6 max,
> emitted at 0x1142aa / 0x1143a1 / 0x1143ea) and the captured 2048.0f is exactly
> `GL_ALIASED_POINT_SIZE_RANGE` max, which both the GL and D3D capability blobs
> declare as 2048.  The guard band is a different quantity entirely: +/-8160.0f,
> block index 0x174, byte-identical in both vendor binaries.  The 4096f default
> coinciding with `GL_MAX_VIEWPORT_DIMS` 4096x4096 is what made the wrong
> reading look plausible.  See docs/parhelia-gl-limits.md and
> docs/parhelia-raster-clip.md.


#### Texture environment, group `0x0e`

112 entries = **4 units x 28 sub-indices**. The index dword is
`(sub << 16) | unit`; sub runs `0x00..0x1c` skipping `0x0c`. Emission order is
sub-major: all four units of sub 0, then all four of sub 1, …

The kernel keeps **eleven** such units, `0x82` bytes of state each, at
`ctx + 0xdd7 + unit*0x82`, and the same record type also carries the
texture-object bind (`[+0x0c] = handle`, dirty bit `|= 0x08`). Only four units
are programmed at init. [CONFIRMED — handler at `mtx558+0x459dc`]

Stage 0 defaults differ from stages 1–3:

| sub | stage 0 | stages 1–3 |
|---|---|---|
| 0x00 | 0 | 0 |
| 0x01 | 4 | 1 |
| 0x02 | 2 | 2 |
| 0x03 | 0 | 1 |
| 0x04 | 2 | 1 |
| 0x05 | 2 | 2 |
| 0x06 | 0 | 1 |
| 0x07–0x0a | 0 | 0 |
| 0x0b | 0 | **stage index** (1, 2, 3) |
| 0x0d, 0x0e | 1 | 1 |
| 0x0f | 0 | 0 |
| 0x10, 0x11 | 1 | 1 |
| 0x12–0x14 | 0 | 0 |
| 0x15 | 1 | 1 |
| 0x16–0x18 | 0 | 0 |
| 0x19–0x1c | 1 | 1 |

#### Sampler state, group `0x06`

36 entries = **4 units x 9 sub-indices** (sub `0x01..0x09`), identical for all
four units:

```
sub 0x01 = 0
sub 0x02 = 2      sub 0x03 = 2      sub 0x04 = 2      sub 0x05 = 2
sub 0x06 =  1000.0f       sub 0x07 = -1000.0f       sub 0x08 = 1000.0f
sub 0x09 = 1.0f
```

The `±1000.0f` triple reads as LOD min / LOD max / LOD bias clamps and `1.0f`
as max anisotropy. [INFERRED]

#### State group `0x05` — 24 pairs

Indices `0x04, 0x08, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c, 0x30, 0x34,
0x38, 0x48, 0x4c, 0x50, 0x68, 0x54, 0x58, 0x5c, 0x60, 0x64, 0x6c, 0x70` (note
`0x68` out of order, exactly as emitted), values

```
0,0,0,3,0,0,0,2,2,1,1,0,0,0,0,0,0,0,0,0,0,0,1.0f,0x7fffffff
```

Index `0x14` in this group is the **fragment program selector** (§3.3); it is 0
here and becomes the uploaded program's id at the first draw. The register
addresses behind the other indices are [UNRESOLVED] — see §0.

---

## 2. The minimal per-draw sequence — one untextured Gouraud triangle

Two cases: the **first** draw after any state change, which re-emits everything
the ICD considers dirty, and the **steady state**, which is startlingly small.

### 2.1 First draw — the full stream (`0x1188` bytes, capture submission 17)

Emission order, exactly as captured. Every record is mandatory in the sense that
the vendor emits it; the ones a minimal driver can plausibly drop are noted.

```
0x0000  (0x0d,  1)  32-byte element  2, 0, 3, 0, 0, 0, 8, 3   [PURPOSE UNKNOWN]
0x0028  (0x35,  1)  16-byte element  0, 0, 0, 0                [PURPOSE UNKNOWN]
                    -- byte-identical in every capture; emit verbatim
0x0040  (0x0c, 36)  state:
            id 0x17 = 2                      depth func LESS
            id 0x26 = 1.0f
            id 0x9b = 0
            id 0xa6 = 2048.0f                GL_POINT_SIZE_MAX (see note)
            id 0x40..0x5f = 0xffffffff        the 32-entry attribute block
0x0168  (0x12,  1)  SCISSOR-RECT   0, 0, 256, 256
0x0180  (0x46,  1)  FRAGPROG-UPLOAD  progid 2, 0x664-byte blob
0x0800  (0x05,  2)  record-05 ids 0x13 = 1 and 0x14 = 2   (select fragment program 2)
0x0818  (0x26,  1)  VIEWPORT       0, 0, 256, 256
0x0830  (0x36,  1)  LINE/POINT-SIZE  1.0, 1.0
0x0840  (0x0c,  1)  state id 0x8c = 1
0x0850  (0x24,  1)  3, 5                     [PURPOSE UNKNOWN]
0x0860  (0x23,  1)  light-model ambient 0.2, 0.2, 0.2, 1.0
0x0878  (0x0c,  2)  state ids 0x8e = 0 and 0x89 = 0
0x0890  (0x21, 24)  12 light blocks of 0x78 bytes, GL defaults
0x0e38  (0x22,  2)  materials, front and back, GL defaults
0x0ed0  (0x27,  1)  depth range 0.5, 0.5
0x0ee0  (0x25,  6)  six clip planes, all zero
0x0f60  (0x20,  6)  six 4x4 identity matrices, ids 1, 3, 0x10, 0x11, 0x12, 0x13
0x1100  (0x0c,  1)  state id 0x16 = 1        cull face off
0x1110  (0x07,  1)  PIPELINE-KEY = 0x00000010
0x1130  (0x08,  1)  VERTEX-FORMAT  stride = 20
0x1158  (0x1b,  1)  DRAW
```

The light, material, clip-plane and matrix records are the fixed-function T&L
state; the WARP unit runs transform *and* lighting, so a driver that wants the
vendor's pipeline must supply them. Matrix ids `0x10..0x13` are the four texture
matrices. [INFERRED from the ids and the identity contents]

### 2.2 The DRAW record — `(0x1b, 1)`, 40-byte element

```
+0x00  u32   primitive     the raw GL enum: POINTS 0, LINES 1, LINE_LOOP 2,
                           LINE_STRIP 3, TRIANGLES 4, TRIANGLE_STRIP 5,
                           TRIANGLE_FAN 6, QUADS 7, QUAD_STRIP 8, POLYGON 9
+0x04  u32   flags         [PURPOSE UNKNOWN] observed 0, 1, 2, 4, 6, 0x18
+0x08  u64   pointer to a u64 holding the client descriptor base
+0x10  u64   descriptor start offset in the command ring   = slot
+0x18  u64   descriptor end offset,  **bit 0 = hybrid mode** = slot + 0x1c + 1
+0x20  u64   vertex data offset in the vertex ring          = slot + 0x40
```

`slot = draw_index * 0x2000` in **both** rings. Verified across five consecutive
draws: `0x0000, 0x2000, 0x4000, 0x6000, 0x8000, 0xa000`. [CONFIRMED]

The ICD passes the GL primitive enum straight through; it does **not** decompose
strips, fans or quads. Only wide lines are CPU-emulated. [CONFIRMED]

**The kernel does not write a primitive register directly.** `mtx2455` @0x471d0
translates the GL enum into three shadows and lets the dirty-state emitter build
the packets. The translation tables, dumped from `.rodata`:

| GL prim | 0 PTS | 1 LINES | 2 L_LOOP | 3 L_STRIP | 4 TRI | 5 TSTRIP | 6 TFAN | 7 QUADS | 8 QSTRIP | 9 POLY |
|---|---|---|---|---|---|---|---|---|---|---|
| `ctx+0x1be4` low nibble | 0 | 2 | 3 | 4 | 5 | 6 | 7 | 9 | 0xa | 1 |
| `ctx+0x297c` | 0 | 2 | 3 | 4 | 5 | 6 | 7 | 9 | 0xa | 1 |
| class (`.rodata+0x1f20`) | 6 | 7 | 7 | 7 | 8 | 8 | 8 | 8 | 8 | 6 |
| splittable (`.rodata+0x1f54`) | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 1 | 1 | 0 |

with a coarse class `{0,1}` point, `{2,3,4}` line, `{5..8}` triangle, `{9,10}`
quad. `splittable = 1` marks primitives a batch may be broken at freely; strips,
fans and polygons are 0. [CONFIRMED]

This **contradicts** `docs/parhelia-command-construction.md`, which reads the
`0x10b0` primitive value as emitted by the ICD as a standalone register packet.
Both may be true on different paths — the ICD does contain the `0x10b0` encoder —
but on the path the captures exercise, the enum reaches the kernel inside the
DRAW record and the kernel decides. [UNRESOLVED which path a real workload takes]

### 2.3 The vertex data (region 4, vertex ring)

Three vertices, stride 20, at `slot + 0x40`:

```
slot+0x40   x  y  r  g  b      IEEE-754 floats, raw object-space glVertex values
slot+0x54   x  y  r  g  b
slot+0x68   x  y  r  g  b
```

Positions are **not** transformed on the CPU. The stride is a prefix sum over a
fixed attribute order and is announced per submission by the VERTEX-FORMAT
record — see `docs/parhelia-command-construction.md` for the full table.
[CONFIRMED twice, independently]

### 2.4 The command-ring packet (region 5), 7 dwords at `slot`

```
slot+0x00   0x8f000001    DRAW packet: opcode 0x8f, count = payload dwords - 1
slot+0x04   first         first vertex index                     = 0
slot+0x08   last          one-past-end vertex index              = 3
slot+0x0c   0x00000002    register-write element: 2 data dwords follow
slot+0x10   0x21212425    DMA header: index 0x25 -> reg 0x1094,
                                      index 0x24 -> reg 0x1090,
                                      index 0x21 -> filler/terminator (twice)
slot+0x14   fence value   -> reg 0x1094   = the submission serial
slot+0x18   fence address -> reg 0x1090   = the fence page's bus address
```

Captured verbatim for five consecutive draws with only the fence value changing
(2, 3, 4, 5, 6, 7) and the address constant. [CONFIRMED]

DMA header bytes are consumed LSB first and each names register
`0x1000 + index*4`. Index `0x21` (`0x1084`) **terminates** the header — the next
dword is a new header — while index `0x20` (`0x1080`, DMAPAD) is the mid-packet
filler that consumes a data dword and discards it. Getting these the wrong way
round produces a stream the engine mis-parses.
[CONFIRMED — `mtx2434` @0x398e0, the vendor's own software replay of this stream.
Note `mtx2434` is **dead code in the kernel build** — no relocation anywhere in
the object references it — and is live only in the X driver, selected by
`pStatus[0x8fc] == 1`. It is still the authoritative spec for the format.]

### 2.4a How the kernel turns that into ring traffic

This is the piece a from-scratch driver has to reimplement, and it is not
symmetric: **the 7-dword descriptor is split in two.** `mtx2196` @`.text+0x4acb0`
reads the DRAW record's `end` field and branches on its **bit 0**:

* **bit 0 clear — pure DMA jump.** `mtx1937(cmdbuf, start, end & ~1, 2, 2)`
  appends the 5-dword secondary-buffer call to the primary ring:
  `{ 0x20292820, 0, start|2, end|2, 0 }`, i.e.
  `DMAPAD=0; SECADDRESS=start|2; SECEND=end|2; DMAPAD=0`.
* **bit 0 set — jump plus inline copy**, which is what every captured draw does
  (`end = slot + 0x1d`). The kernel jumps to `[start, (end & ~1) - 0x10)` — that
  is `[slot, slot + 0x0c)`, exactly the three draw dwords — and then
  `LocMemCpy`s the last 16 bytes of the descriptor
  (`{0x00000002, 0x21212425, value, address}`) **straight into the primary ring**.

So `slot + 0x1d` is not "end plus a stray bit": **bit 0 selects the hybrid
submission mode**, and the client must set it for the packet shape documented
above to work. [CONFIRMED]

Per RENDER call the ring receives, in this order:

```
(a)  cache-flush packet, 6 dwords:
         0x00000004, 0xc0afafc0, mode|7, fence|1, fence, saved(0x1300)
     0xc0afafc0 decodes as {0x1300, 0x12bc, 0x12bc, 0x1300}
(b)  one jump (+ optional inline copy) per DRAW record
(c)  the kernel's own 3-dword fence element:
         0x21212425, fence, fenceWriteBackAddress
     fence = ++*(u32 *)(ctx[0x10] + 0x50), 32-bit wrapping, skipping 0
(d)  memWriteDword(0x408, ringBusBias + ringWritePointer)     the doorbell
```

[CONFIRMED — `mtx558` epilogue @0x451dd, `mtx1937` @0x32360, `mtx3468` @0x3bd5b]

Note that the fence element appears **twice**: once as the tail of the client's
own descriptor (inlined by the kernel in the hybrid path) and once appended by
the kernel from its own counter. Whether these are the same counter, or the
client's is redundant, is [UNRESOLVED]. The observed serial page increments by
exactly 1 per draw and matches the value in the client's descriptor, so at
minimum they stay in lockstep.

One more [UNRESOLVED] item worth a single capture: the client's stream prefixes
each register element with an explicit dword count (`{2, 0x21212425, value,
address}`) and the kernel's flush packet has the matching `0x00000004` prefix,
but `mtx1937`'s jump packet and the kernel's own 3-dword fence do not — and
`mtx2434` reads no count field at all. Dump the ring bytes around a `0x20292820`
to settle whether the count word is on the wire or is a client-side convention
the `0x21` terminator makes redundant.

### 2.5 Steady state — what a repeat draw actually costs

Draws 3, 4, 5 in the 5-draw capture touch **twelve bytes** of the command-record
stream. The whole stream is a single 48-byte DRAW record, and only the three u64
offsets change:

```
region 2   0x18 .. 0x28    12 bytes   the descriptor/vertex offsets
region 4   slot+0x40       60 bytes   3 x 20-byte vertex
region 5   slot+0x00       24 bytes   the 7-dword packet
region 0   nothing         CB[0x00]=1 and CB[0x230]=0x30 already stand
ioctl(fd, 0x4621)          one zero-sized doorbell
region 3   +0x00                      the engine writes back the serial
```

So the per-draw driver loop is:

```
1  write the vertices at vertexRing + slot + 0x40
2  write the 7-dword packet at cmdRing + slot
3  patch the DRAW record's three u64 offsets in the record stream
4  (if the stream shape changed) set region0[0x230] = stream length
5  (if the target changed) set region0[0x08]
6  region0[0x00] = 1   (RENDER)
7  ioctl(fd, _IO('F',33))
8  advance slot by 0x2000
```

Everything else — the state records, the fragment program, the matrices — is
emitted only when the corresponding GL state is dirty.

---

## 3. Adding one feature at a time

**Read the register names in this section through §0.** The records carry
driver *state ids*; the `0x1000 + id*4` register name is kept for
cross-reference and is unverified. Where a driver reproduces the vendor stack it
emits the id; where it drives registers itself it must first resolve the id
through the kernel's 172-arm table.

Each subsection lists **only the delta** against §2. All of it comes from
single-feature captures, so the attribution is direct — with the one caveat that
the ICD flushes state lazily, so a capture's stream also carries the *cleanup* of
the previous operation. Those cleanup writes are marked.

### 3.1 Depth test

`glEnable(GL_DEPTH_TEST)` + `glVertex3f`, total stream `0x88` bytes:

```
(0x0c, 1)   id 0x07 (0x101c) = 1          depth test enable
(0x07, 1)   PIPELINE-KEY = 0x00000011     was 0x10
(0x08, 1)   VERTEX-FORMAT stride = 24     was 20 — z appears
(0x1b, 1)   DRAW
```

Related, each a single PAIRS-CORE pair:

| GL call | record | register | value |
|---|---|---|---|
| `glDepthFunc(f)` | `(0x0c,1)` id `0x17` | `0x105c` | `f - GL_NEVER + 1`, i.e. NEVER 1 … ALWAYS 8 |
| `glDepthMask(FALSE)` | `(0x0c,1)` id `0x0e` | `0x1038` | 0 / 1 |
| `glDisable(GL_DEPTH_TEST)` | `(0x0c,1)` id `0x07` | `0x101c` | 0 |

The **vertex stride grows to 24** and the position attribute becomes `x,y,z`;
the colour follows it. The DRAW record and the ring packet are unchanged.
[CONFIRMED]

### 3.2 Blending

`glEnable(GL_BLEND)` + `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`,
total stream `0xd0` bytes:

```
(0x0c, 6)   id 0x1b (0x106c) = 1   blend enable
            id 0x05 (0x1014) = 4   [PURPOSE UNKNOWN — blend equation? always 4 here]
            id 0x0a (0x1028) = 5   source alpha factor   = GL_SRC_ALPHA
            id 0x09 (0x1024) = 5   source RGB factor
            id 0x0c (0x1030) = 6   dest alpha factor     = GL_ONE_MINUS_SRC_ALPHA
            id 0x0b (0x102c) = 6   dest RGB factor
(0x0c, 3)   ids 0x07 = 0, 0x17 = 2, 0x0e = 1        [previous op's cleanup]
(0x07, 1)   PIPELINE-KEY = 0x00000051      = 0x11 | 0x40
(0x08, 1)   VERTEX-FORMAT stride = 28      alpha appears (glColor4f)
(0x1b, 1)   DRAW
```

Factor enum: `ZERO=1 ONE=2 SRC_COLOR=3 … SRC_ALPHA=5 ONE_MINUS_SRC_ALPHA=6`,
confirmed by a second capture (`glBlendFunc(ONE, ONE)` sets all four to 2).
Register roles: `0x1024`/`0x1028` = source RGB/alpha, `0x102c`/`0x1030` =
destination RGB/alpha. [CONFIRMED]

**Pipeline-key bit `0x40` = blending** [INFERRED] — `0x51` vs `0x11` differ by
exactly that bit, but the pair is not clean because the same stream carries the
depth teardown.

Alpha test, for completeness (no key and no vertex-format change):

```
(0x0c, 4)   id 0x0f (0x103c) = 1     alpha test enable
            id 0x19 (0x1064) = 5     alpha func GREATER (same 1..8 enum as depth)
            id 0x18 (0x1060) = 0x80  reference, 8-bit: round(0.5 * 255)
            id 0x1b (0x106c) = 0     [previous op's cleanup]
```

### 3.3 A texture

Total stream `0x1078` bytes (mtxtrace, 16x16 RGBA) or `0x808` (glsweep, 64x64).
Both decode identically in structure:

```
(0x0a, 1)   TEXTURE-DEF        80-byte record, layout below
(0x0e, 1)   texture unit 0, sub 0x00 = 4
(0x46, 1)   FRAGPROG-UPLOAD    progid, then a 0x664-byte blob; record padded to 0x680
(0x05, 2)   ids 0x13 = 1 and 0x14 = progid            SELECT FRAGMENT PROGRAM
(0x0c, 1)   id 0x89 = 0
... the usual light / material / matrix / clip-plane revalidation ...
(0x0e, 1)   texture unit 0, sub 0x18 = 2
(0x07, 1)   PIPELINE-KEY = 0x00020011      bit 17 set
(0x08, 1)   VERTEX-FORMAT stride = 28 or 32
(0x1b, 1)   DRAW
```

**TEXTURE-DEF payload (72 bytes)**

```
+0x00  u64   kernel pointer to the texture object
+0x08  u32   0
+0x0c  u32   0
+0x10  u32   4                 [PURPOSE UNKNOWN, constant in every capture]
+0x14..+0x20 0
+0x24  u32   width             plain dword, not log2, not a pitch   [CONFIRMED]
+0x28  u32   height            [CONFIRMED]
+0x2c  u32   level count / depth   = 1
+0x30..+0x44  filter / border state — differs between the two captures
              (16x16 NEAREST run: 0, 0, 0, 0x24, 0, 2;
               64x64 LINEAR run:  0, 2, 0, 1.0f, 1.0f, 1.0f)     [UNRESOLVED]
```

**There is no format field.** Two runs identical except `GL_RGB` vs `GL_RGBA`
produce byte-identical descriptors, and the pixel data occupies 32 bits per texel
either way. [CONFIRMED]

**Where the pixels go.** Region 6, the 4 MB heap, and they appear at **first
use, not at upload**: `glTexImage2D` changes nothing; the first textured draw
writes `width * height * 4` bytes at offset 0. [CONFIRMED — 1024 bytes for
16x16, 16384 for 64x64]

**Pipeline-key bit 17 (`0x20000`) = texturing.** Clean pair: the key returns to
`0x11` after `glDisable(GL_TEXTURE_2D)` with nothing else changing.
[CONFIRMED]

**Vertex stride.** The two captures give 28 and 32 for the same nominal case,
because the ICD picks a texcoord entry from its stride table by the *declared*
> **Superseded (2026-09-05): the pipeline-key bit numbering below is wrong.**
> `docs/parhelia-gl-limits.md` §1.2 assigns `[6:4]` = diffuse colour and
> `[10:8]` = secondary/specular/fog, and puts the texcoord sets at `[18:16]` and
> `[21:19]`, with the shift set `{16,19,22,25}` read from `.rodata` literals at
> `0x36ada0` and `0x369830`.  That accounts for all six captured keys with no
> free parameters.  Use it, not the field positions in this section.

texcoord dimensionality: `fmt[6:4]=3` -> 8 bytes -> stride 28
(`x,y,r,g,b,u,v`), `fmt[6:4]=1` -> 12 bytes -> stride 32. Both were seen. The
28-byte case was decoded vertex-by-vertex and the texcoords came out
`(0.5,1.0) (0,0) (1,0)` for a triangle — an unambiguous confirmation of the
layout. [CONFIRMED]

**Sampler parameters** are a `(0x06, N)` record indexed
`(sub << 16) | stage`. Only one is pinned by capture:
`glTexParameteri(GL_TEXTURE_MIN_FILTER, GL_LINEAR)` emits
`(0x06,1) { (sub 0x10, stage 0) = 2 }`. `glTexParameteri(…, GL_REPEAT)` emits
**nothing** — repeat is the default. `glTexEnvi(…, GL_MODULATE)` likewise emits
nothing. `GL_REPLACE` emits a whole new fragment program. [CONFIRMED]

**The fragment program is the texture environment.** There is no fixed-function
combiner: each distinct texenv configuration is compiled to a program, uploaded
by a `(0x46,1)` record, and selected by group-05 index `0x14`. The program id in
the upload header and the value written by the select record always match — that
match is what pins the whole structure. Even the *untextured* triangle uploads a
program (progid 2). A cache hit (`tex_env_modulate`) emits no blob at all: the
region-2 delta is 12 bytes. See `docs/parhelia-fragment-isa.md` for the blob
layout and instruction encoding. [CONFIRMED]

### 3.4 A second texture unit

**Not captured.** What is established, and what a driver would have to do:

* The hardware has **four texture stages**. Both per-stage record types index
  `(sub << 16) | stage` with `stage` in `0..3`, and the default-state block
  programs all four (§1.3). [CONFIRMED]
* Stage `n`'s texenv sub-index `0x0b` carries the stage number itself in the
  defaults (0, 1, 2, 3), which is the only per-stage-varying default besides
  stage 0's differing combiner setup. [CONFIRMED]
* A second unit needs a **second texcoord in the vertex**: `fmt[10:8]` selects
  texcoord-1's size from the same table as texcoord 0, appended after texcoord 0.
  Stride 28 -> 36 for two 2-float sets. [INFERRED from the ICD stride table]
* It needs a **new fragment program**: the combiner for two units is a different
  program, so a `(0x46,1)` upload with a new id and a matching group-05 index
  `0x14` select. [INFERRED, but it follows directly from §3.3]
* `TEXTURE-DEF` records carry no stage number, so the binding of a texture
  object to a stage must be one of the unidentified fields, or implicit in
  emission order. [UNRESOLVED]
* The pipeline key for two units is unknown; bit 17 is "texturing", not "one
  texture". [UNRESOLVED]

### 3.5 Other single-feature deltas, for reference

All are `(0x0c, N)` pair records unless noted. [CONFIRMED]

| GL | state id | published reg | value |
|---|---|---|---|
| `glEnable(GL_STENCIL_TEST)` | 0x34 | 0x10d0 | 1 |
| `glStencilFunc` ref | 0x39 | 0x10e4 | ref |
| `glStencilFunc` mask | 0x3a | 0x10e8 | mask |
| `glStencilOp(…, REPLACE)` | 0x37 | 0x10dc | 3 |
| `glEnable(GL_CULL_FACE)` + `glCullFace(GL_BACK)` | 0x16 | 0x1058 | 2 (1 = off) |
| `glPolygonMode(…, GL_LINE)` | 0x08 | 0x1020 | 2 (3 = FILL) |
| `glEnable(GL_FOG)` | 0x1c | 0x1070 | 1 |
| `glFogi(GL_FOG_MODE, GL_LINEAR)` | 0x8c | 0x1230 | 3 |
| `glEnable(GL_LIGHTING)` | 0x89 | 0x1224 | 1, plus a `(0x21,2)` light block, key 0x05, stride 24 with the normal streamed |
| `glScissor(x,y,w,h)` | — | `(0x12,1)` rect | screen pixels, **Y from the bottom** |
| `glViewport(x,y,w,h)` | — | `(0x26,1)` rect | same space |
| `SwapBuffers` | 0x3e | 0x10f8 | toggles 1 / 0 |

`glClear` is a `(0x15, 1)` record, 36-byte element:
`[mask][ARGB8888][depth float][stencil][0x80808080][x1][y1][x2][y2]`, mask
1 = colour, 2 = depth, 3 = both. **The last four dwords are a corner pair, not
`w, h`**, and the kernel adds the render target's origin to all four before
blitting (`mtx3513` @0x4a0b0) — a correction to the earlier "x, y, w, h" reading.
`payload[0] & 0x10` selects a second pass. The `0x80808080` dword is constant in
every clear. [PURPOSE UNKNOWN]

Wide lines are **CPU-emulated**: `glLineWidth(4.0)` emits pipeline key `0x852`,
stride 32, *window-space* pre-transformed vertices, and four parallel 1-pixel
lines. Do not look for a width register.

---

## 4. Synchronisation

There are exactly **seven** distinct wait primitives in the vendor core, found by
enumerating every backward branch enclosing a register read. **None of them has
a timeout.** They are listed here in the order a driver meets them.

### 4.1 `WaitBusMastering` — the composite "engine drained" wait

The one every teardown, every buffer free and every ring wrap goes through.
Inlined at `mtx2508` @0x31880, `mtx2095` @0x321e0, `mtx479` @0x44d50,
`mtx2667` @0x4c5c0, `mtx3677` @0x4f8c0, `mtx935` @0x4fcf0, `mtx1391` @0x9f781;
un-inlined in `mtx_drv.so` as `CDPSHwManager::WaitBusMastering` @0x6c750.
[CONFIRMED]

```
1  while (pStatus[0x78] != 0) ;                       /* pending-submit counter */
2  do { v = read(0x8f0); } while (!(v & (1 << 17)));  /* primary DMA finished   */
3  do { v = read(0x450); } while (v != pStatus[0x9c]);
   pStatus[0x90] = 0;                                 /* reset the FIFO counter */
```

Two things about step 3 that matter and are not obvious:

* the test is a **full 32-bit equality**, not a masked bit test;
* `pStatus[0x9c]` is a **snapshot of register `0x450` taken once at device init**
  (`mtx3229` @0x2460 reads `0x450` and stores it at `+0x9c`). The idle criterion
  is literally "`0x450` has returned to the value it had at init". A driver must
  take the same snapshot; there is no documented mask to use instead.
  [CONFIRMED]

**Step 1 is a bug — do not copy it.** The field is not `volatile`, the compiler
hoisted the load, and the emitted code is `cmp DWORD PTR [rdx+0x78],0 / jne
<itself>`: if the counter is ever non-zero the CPU spins forever *without
re-reading memory*. All seven sites are affected (0x318a0, 0x321e4, 0x44d76,
0x4c5e6, 0x4f8e3, 0x4fd41, 0x9f785). [CONFIRMED]

### 4.2 The indexed-write FIFO — `0x450` bit 22

Registers `0x1000–0x13FF` go through a 16-entry write FIFO. The exact rule, read
off `mtx2434` @0x399cf:

```
n = ++pStatus[0x90];
if (n <= 16) { pStatus[0x90] = n; }                    /* no wait */
else         { pStatus[0x90] = 0;
               do { v = read(0x450); } while (!(v & 0x400000)); }
write(reg, data);
```

So the drain fires on the **17th** write since the last drain, and the counter is
not re-checked afterwards. Roughly 25 inline sites. [CONFIRMED]

**`0x450` therefore has two unrelated uses** — full-equality idle (§4.1 step 3)
and bit-22 FIFO-empty here. Do not conflate them.

### 4.3 The engine fence — `0x12BC` write, `0x41C` read

Tag allocation: `tag = pStatus[0x44] += 8`, **skipping 0**; **bit 0 of the value
written to `0x12BC` means "block until complete"**. The tag is emitted into the
command stream as register index `0xaf`; `mtx2434`'s handling is

```
<FIFO drain, §4.2>
write(0x12BC, val & ~1);
if (val & 1) do { v = read(0x41C); } while (v != (val & ~1));
```

The wait the *driver* performs (`mtx508` @0x38888, `mtx984` @0x4e890,
`mtx3616` @0x4ec70, and `ParhlXAASync` @0x534d7 in `mtx_drv.so`) is wrap-safe and
has two sources:

```
for (;;) {
    completed = pStatus[0x8fc] ? (read(0x41C) & ~7)      /* poll the register    */
                               : (pStatus[0x10] & ~7);   /* host write-back slot */
    latest = pStatus[0x44];
    if ((completed >= myTag) != (completed > latest)) break;   /* mtx177 @0x4dc80 */
}
```

`pStatus[0x8fc]` is the submission-mode word: 0 = ring, 1 = emulated DMA,
2 = direct PRIMEND. `pStatus[0x10]` is a **hardware DMA write-back slot**, usable
only once `0x400` points at a DMA-coherent page (§4.6). Prefer the `0x41C` poll
until that is set up. [CONFIRMED, the write-back reading INFERRED]

### 4.4 The per-submission serial — how completion reaches user space

This is the mechanism a GL client actually waits on, and it needs **no ioctl and
no interrupt**. Every draw packet carries:

```
reg 0x1090 = bus address of a 4-byte completion word
reg 0x1094 = the value to write there
```

`0x1090` is the mmap cookie of the PCI_LOCKED fence page created by
control-block op `0x0a` (§1.2) — for that memory class the cookie *is* the
physical/bus address. `0x1094` is a monotonic submission serial. Across five
draws the packets carried 3, 4, 5, 6, 7 and the page read back exactly those
values. `glFinish` is "spin until `*fencePage >= myValue`".
[CONFIRMED, both halves observed in the same capture]

**A driver must therefore allocate one DMA-visible page per context, hand its bus
address to `0x1090` in every packet, and poll it.** There is no completion ioctl
in this ABI.

### 4.5 The pre-submit bus gate — `0x318` / `0x31C`

Taken only when the ring buffer's address-space selector is 0
(`mtx3144` @0x31db2, `BmPrimInt::Start` @0x8943e):

```
mask = 0x57; expect = 0x50;
if (CommonData[0x2440] & 0x7e000) { mask = 1; expect = 1; }
do { reg = (CommonData[0x2440] & 0x70000) ? 0x31C : 0x318;
     v = read(reg); } while ((v & mask) != expect);
write(0x408, PRIMEND);
```

`CommonData[0x2440]` is a cached bus-master capability word; the masks line up
approximately with the `AGPINFO_*` flags in `kernel/src/mtx_agp_flags.h`.
[CONFIRMED code, flag mapping INFERRED]

### 4.6 `0x400` is a write-back pointer, not a limit — correction

`docs/parhelia-kernel-abi.md` §6.4 records `0x400` as an unexplained
"limit or wrap pointer", and `pStatus[0x40]` as "ring limit value". Both are
wrong. `mtx3229` @0x24ad stores the **low 32 bits of the shared-locked page's own
physical address** into `pStatus[0x40]` (with `pStatus[0x4c] = phys + 0x20`), and
`BmPrimInt::Reset` (`mtx2508`) writes that to `0x400`. So `0x400` gives the
engine a **status write-back address**, which is what makes `pStatus[0x10]` and
`pStatus[0x20]` readable by the CPU. [INFERRED, high confidence]

Still unexplained: the value is masked `& ~0x1f` when a capability query returns
bit 2 (`CommonData[0x2198][0x608]->vtbl[0x20]() & 4`, at 0x319e8). [PURPOSE
UNKNOWN]

### 4.7 The WARP idle wait, before a microcode load

`mtx1391` @0x9f6b0, in this exact order. A driver reloading the microcode after a
fault must reproduce all six steps. [CONFIRMED]

```
1  mtx1937(...)                                   commit whatever is pending (writes 0x408)
2  WaitBusMastering                               all three steps of §4.1
3  do v = read(0x908); while ((v & 0xfff7ffbf) != 0x63a78001);
4  do { v908 = read(0x908); v924 = read(0x924); }
     while ((v924 & 0xfff) != 0x100 || (v908 & 0xfff7ffbf) != 0x63a78001);
5  do v = read(0x8f0); while (v & 0x03000000);    WARP busy bits 24-25 clear
6  write(0x918, byteOffset / 12); then 3 dwords per instruction to 0x91C
```

The one bounded error path in the whole engine: at 0x9f6f2,
`if (curOffset + size > 0x17ff) { flag = 1; return FALSE; }` — the instruction
store is `0x1800` bytes (512 x 12-byte instructions) and overflow is reported
rather than waited on.

### 4.8 Locks

* `pStatus[0x70]` — per-ring submit lock. `mtx3144` @0x31d17 spins on
  `OsInterlockedExchange(&p[0x70], 1)` with **no yield and no timeout**. On
  release, if the old value was **2** the holder was asked to re-kick: it does
  `p[0x78]--` and rewrites `0x408 = p[0x74]`. Nothing in the kernel core ever
  stores 2 there, so the writer is the X driver or the ICD. [PURPOSE UNKNOWN]
* `pStatus[0x7c]` — global hardware-access lock. The GL doorbell `mtx903`
  @0x1d40 spins **with `OsSchedule()`**; `ParhlXAASync` @0x53441 spins without.

### 4.9 Ring wrap

Everywhere a packet is appended:
`if (writePtr + packetSize > ring[0x70]) ring->vtbl[0x20](ring);` =
`BmPrimInt::SwitchToNewBuffer` (`mtx2095` @0x31fc0). With a multi-buffer pool it
picks a free buffer and emits a buffer-switch packet — no stall. **With a single
buffer it runs §4.1 in full and then `BmPrimInt::Reset`** — a complete CPU stall
on every wrap, which is a good reason to allocate more than one. [CONFIRMED]

`BmPrimInt::Reset` (`mtx2508` @0x31880) is the restart sequence:

```
WaitBusMastering (§4.1)
addr = (buffer.phys + ring.writePtr) | buffer.addressSpaceFlags;
pStatus[0x74] = addr;
write(0x404, addr & ~3);                 /* PRIMADDRESS */
v = pStatus[0x40];
if (caps & 4) v &= ~0x1f;
write(0x400, v);                         /* write-back pointer, §4.6 */
```

### 4.10 What the ICD itself does per frame

```
CB[0x00] = 1                RENDER
CB[0x234] = 0xffffffff      the client clears the result slot
ioctl 0x4621
                            kernel writes CB[0x234]: 0 ok, -1 invalid op, -3 not ready
                            kernel re-seeds the stream header with (0xff, 1)
                            the engine writes the fence page
```

`CB[0x234]`, **not the ioctl return value, is the error channel** — `mtx903`
returns 0 even when it refuses (§5.3). [CONFIRMED]

Control-block op `0x1e` brackets the whole session: issued before the first
RENDER to **attach** the drawable (`CB[0x10]` = context index, `CB[0x18]` =
drawable handle) and again after the last draw to **detach** (`CB[0x18] = 0`).
Kernel side it is `mtx1470` @0x4ffb0 -> `mtx1305` @0x463d0, which drops the old
surface's refcount before binding the new one — a MakeCurrent. The four bytes it
writes at offset 0 of region 1 are the only traffic that region ever sees, and
their meaning is [PURPOSE UNKNOWN].

---

## 5. Teardown, fault and reset

### 5.1 The normal teardown order — client side

Read off the tail of `captures/mtxtrace-5draws.txt` and the ICD's destructors.
Each step is `ioctl(fd, 0x4621)` with the control-block opcode shown. [CONFIRMED]

```
1  op 0x1e   detach the drawable       CB[0x10] = context handle, CB[0x18] = 0
2  op 0x0d   destroy each surface resource   CB[0x10] = handle
              (the texture first, then front/back/Z)
3  op 0x06   free each device buffer   CB[0x10] = the buffer handle from op 0x05
              -- the ICD munmaps the region *before* issuing this
4  op 0x0b   destroy the rendering context    CB[0x10] = context handle
              -- the ICD munmaps the 4-byte fence page first
5  ioctl(fd, _IO('F',32) = 0x4620)     destroy the ICD link
6  munmap the control block; close(fd)
```

The unmap-then-free ordering is the ICD's, and it is the right way round: the
kernel frees the backing pages in the op, so a still-mapped region would leave a
dangling user mapping.

### 5.2 The kernel-side teardown chain

Three ioctls, in the order the X driver issues them.

**`LnxDevice::Stop`, ioctl `0x40044617` (nr 23) -> `mtx3446` @0x10f0**
[CONFIRMED]

```
mtx508(dev);          /* the Sync primitive, §5.3 -- SOFTRAP + wait for retirement */
dev[0x6b8] = 0;       /* "stopped": makes the GL doorbell answer -3 from now on   */
for each client whose link belongs to this device
    mtx1204(link, dev);   /* soft detach, no fence wait -- the Sync already ran;
                             ends with link[0x14c] = 1, "orphaned"               */
```

No register writes beyond those inside `mtx508`.

**`ParhlDisableDevice`, ioctl `0xc0084615` (nr 21) -> `mtx1612` @0x1070 ->
`mtx3470` @0x2970** [CONFIRMED]

```
for each client link on this device { mtx984(link); delete link; }   /* §5.6 */
mtx3486(dev);   /* the device destructor */
delete dev;
```

`mtx3486` @0x38a20 is: if the device was ever started and the engine is dirty,
run `mtx508` first; then walk the **22-entry init/undo table** at
`.rodata + 0x1860` (`{init_fn, cleanup_fn, name}`, 0x18 bytes each) **backwards**
from the last successfully completed step. Step **19** is the WARP unit
(`mtx3810` init / `mtx3191` cleanup); the other 21 are anonymous in this release
build — every `name` string is empty — so the semantic order of teardown is only
recoverable by disassembling all 22 cleanup functions. [CONFIRMED / [PURPOSE
UNKNOWN] for the 21]

Start is the same table forwards, and **a failed init step immediately runs the
full Sync before unwinding**.

**ICD link destroy, ioctl `0x00004620` (nr 32) -> `mtx2376` @0x1bf0**: calls the
real destructor `mtx984` and deletes the link object.

**`mtx1700` (core cleanup @0x640)** runs the C++ static destructors and
unregisters its 14 ioctls. **`mtx547` (core disable @0x8d0)** walks the device
list and calls `mtx3470` on each. **Neither touches a register.** [CONFIRMED]

### 5.3 `mtx508` — the Sync primitive every teardown goes through

`mtx508` @0x387a0. This is the sequence a driver must copy to bring the engine to
a known-quiet state. [CONFIRMED]

```
tag = pStatus[0x44] += 8;  if (tag == 0) tag += 8;
if (ring.wp + 0x10 > ring.limit) SwitchToNewBuffer(ring);
ring[0] = 0x21AFDDC0;        /* header: C0 -> 0x1300, DD -> 0x1374,
                                        AF -> 0x12BC, 0x21 -> end of packet */
ring[1] = 0x00000707;        /*   0x1300 = 0x707 */
ring[2] = 0x00000000;        /*   0x1374 = 0     */
ring[3] = tag | 1;           /*   0x12BC = tag, bit 0 = block until complete */
ring.wp += 0x10;
<submit: mtx3144 fence path, or write(0x408, ringBias + wp)>
<fence wait, §4.3 -- unbounded>
/* only once the engine is idle: */
mtx3091(interruptManager);                  /* IRQ refcount--; at 0 -> write(0x8fc, 0) */
mtx3630(leaveSceneCounter);
for (i = 15; i >= 0; i--)                   /* 16 binding slots, CommonData+0x2228, stride 0x20 */
    if (slot[i]) unbind(slot[i]);
this[0x40] = 0;                             /* engine no longer dirty */
```

The longer 9-dword fence block emitted by `mtx3144` @0x31c30 is

```
0x21C0C0C0   0x00000307  0x00000004  0x00000707
0xC0AFAFDD   0x00000000  tagA|1      tagB|1      pStatus[0x8c]
```

= `0x1300 <- 0x307, 4, 0x707`, end of packet; then `0x1374 <- 0`,
`0x12BC <- tagA|1`, `0x12BC <- tagB|1`, and `0x1300` **restored** from the
shadow. That restore is why `0x1300` values whose low nibble is 6 or 7 are
deliberately not mirrored into the shadow — they are a transient framing state.
[CONFIRMED]

### 5.4 What is *not* quiesced

| what | teardown does |
|---|---|
| IRQ mask | `write(0x8fc, 0)` on the last IRQ-reference release (`mtx3091` @0x3ac20) |
| IRQ line | `free_irq()` in `mtx_irq_cleanup`, **before** `core->disable` |
| ring pointers `0x400`/`0x404`/`0x408` | **never zeroed** — only rewritten on restart |
| bus-master enable | **never disabled**. No PCI `COMMAND` bus-master clear, no undo of `0x10b0`, `0x300` or `0x308` |

So the vendor leaves the engine armed and merely guarantees the ring is drained.
An open driver that wants a genuinely quiet device has to add the disable itself;
the mmiotrace of a clean shutdown shows `0x8fc = 0` followed by zeroing the whole
`0x0000..0x0130` aperture bank, which is the closest thing to a template.

### 5.5 If the client just dies

The generic layer handles it, so a crash does not leak the GPU. `mtx_release`
(`kernel/src/mtx_fops.c`) calls `mtx_ctx_cleanup`:

```
dev->core->close(dev->core_data, ctx->core_data);
for each region in dev->memory_regions
    if (region->ctx_id == ctx->unique_id) MtxRegion_Free(ctx, region);
mtx_ctx_unregister(dev, ctx->unique_id);
```

and when the last reference goes, `mtx_dev_cleanup` (`kernel/src/mtx_dev.c`) runs
`mtx_irq_cleanup` -> `core->disable` -> free the context bitmap -> free every
remaining region -> `mtx_agp_disable`. [CONFIRMED, open source]

### 5.6 Buffer release — the kernel gives you no protection

`RELEASE_BUFFER` (`0x40184602`, `kernel/src/mtx_ioctl.c:178`) performs **no
synchronisation at all**: it validates the descriptor, decrements the refcount
and calls `MtxRegion_Free`, which unlinks the region and hands the pages back to
the allocator. The only nod to safety is a `MTX_DEBUG("WARNING: Freeing a buffer
still in use …")` that is compiled out of release builds. **A DMA buffer the
engine is still reading can be returned to the page allocator.** [CONFIRMED]

The ordering constraint is enforced entirely on the client side, and every free
path in the core does it the same way:

* `mtx479` @0x44d50 (`BmBuffer::FreeMemory`) — **first** runs §4.1
  WaitBusMastering in full, *then* releases the surface. Same shape in
  `mtx2667` @0x4c5c0 and `mtx935` @0x4fcf0 (control-block op 18: wait, then
  virtual destructor).
* Context destroy `mtx984` @0x4e7c0 — unregisters, frees its two buffers, and
  only then, at 0x4e890, performs the §4.3 fence wait on **this context's last
  submitted tag** before tearing down the surface tree.

**Rule for an open driver:** hold every buffer that a submitted command list has
ever referenced until either the ring is drained (§4.1) or the fence tag recorded
at submission time has retired, *before* issuing `RELEASE_BUFFER`.

### 5.7 The fault path — there isn't one

This is the most important negative result in the document, so it is stated
flatly.

* **No timeouts.** Every wait in §4 is an unbounded spin. No iteration counter,
  no jiffies check, no bail-out. The only bounded loops in the whole tree are
  display-side (`WaitForMemoryIdle` @0xe3d20, 100 000 iterations, **silent**
  timeout; `HWPARFSWaitLock` @0xe4510, 100 samples). [CONFIRMED — exhaustive
  loop scan]
* **No error reporting.** `mtx_parhelia.o` contains exactly ten message strings,
  all allocation failures, plus `[mtx] Parhelia patches applied: %s`. Nothing
  matching hang, timeout, lockup, recover or reset. The open-source layer's 29
  `MTX_ERROR` sites are likewise all allocation and registration errors.
  [CONFIRMED — exhaustive string scan]
* **The WARP microcode is never reloaded after a fault.** `mtx1391` has exactly
  two callers, both inside init step 19. [CONFIRMED]
* **A malformed command stream is not detected.** `mtx2434` writes every index
  byte `0x00–0xFF` straight to `0x1000 + idx*4`; its one range check is
  unreachable by construction. A SOFTRAP carrying a bogus tag with bit 0 set
  hangs the interpreter permanently. (Note `mtx2434` is **dead code in the kernel
  build** — zero relocations reference it; it is live only in the X driver, chosen
  by `pStatus[0x8fc] == 1`.) [CONFIRMED]
* **`MTX_IOCTL_HARD_RESET` (`0x00004607`) does not touch the hardware.** Its body
  is `#ifdef DEBUG`, the release module never registers it, and all it does is
  force-decrement the module use count and call `mtx_dev_cleanup` so a wedged
  module can be unloaded after a client crash. `kernel/src/reset.c` says so in
  its own comment. [CONFIRMED]

What the vendor *does* have, and does not use, is a real reset.

### 5.8 The real hardware reset — PCI config register `0xE0`

`HSLPARHardwareReset` @0xc5580 in `mtx_drv.so`. It is an exported HAL entry point
with **no caller anywhere in the driver** — it is never invoked from any hang
path. It is also the only genuine reset that exists, so an open driver's
watchdog should use it. [CONFIRMED]

```
1  bail out if (pdev[8] & 0xc0000000) is set and != 0x40000000
2  HSLPARSaveCriticalData(pdev)
3  save PCI config dwords 0x04, 0x0c, 0x10, 0x14, 0x30, 0x3c, 0x40
     (plus 0xf4, 0xf8 when (pdev[0x3a8] & 0xe0) != 0x80)
4  if device is 0x0528 or 0x2537..0x2539:  write_reg(0x308, 3)
5  read 0x300; rebuild it from PCI config 0x04; write it back with bit 9 cleared
6  write_config_dword(0xE0, 3);  write_config_dword(0xE0, 0);      <-- the strobe
7  wait 10 ms
8  restore all saved config dwords, restore register 0x300,
     HSLPARRestoreCriticalData(pdev)
```

Note it is **not** an MMIO reset bit, and it is not `0x1e40` — that address in
this driver is a MAVEN/TV I2C path.

### 5.9 The "lost link" state — the vendor's actual fault response

In place of recovery there is a **latched lost/orphaned flag**, enforced
identically on both sides of the ABI:

* ICD `Submit` @0x358820: `if (this->IsLost() && op != 3) return -3;`
* kernel `mtx903` @0x1cb8: if the link is orphaned (`link[0x14c] != 0` and its
  state != 3) **or** the device has been stopped (`dev[0x6b8] == 0`, cleared by
  `LnxDevice::Stop`), then `CB[0x234] = -3` and the ioctl returns 0 **without
  touching the hardware**.

So once a context is lost, **the only control op that still works is `0x03`
(Flush/Finish)**. Result codes seen: `0` success, `-1` invalid opcode, `-3` not
ready / lost, `-4` retry (the ICD spins on this for ops `0x10` and `0x1c`),
`-12` ENOMEM and `-14` EFAULT from the `copy_from_user` marshalling wrappers.
[CONFIRMED]

### 5.10 What an open driver must do instead

1. **Bound every wait** in §4 — `0x8f0` bit 17, `0x450` equality, `0x450` bit 22,
   `0x908`, `0x924`, `0x41C`, and both locks. Make the `pStatus[0x78]` read
   `volatile` (§4.1).
2. **On timeout, stop feeding the engine**: mask interrupts (`0x8fc = 0`), stop
   appending to the ring, mark every context lost per §5.9 — after a reset every
   fence value and every ring offset a client holds is stale.
3. **Reset via PCI config `0xE0`** (§5.8), restoring config space *and* register
   `0x300` afterwards.
4. **Re-run the whole init**: §1.1 in full, including re-streaming the WARP
   microcode, because nothing reloads it independently. The ucode loader is
   written to be re-runnable — `bReset` clears the load pointer and the overflow
   flag — which is the one piece of evidence the vendor anticipated this.
5. **Never release a buffer before its fence has retired** (§5.6).

### 5.11 Things in these paths whose purpose is not established

Listed rather than omitted; treat them as candidate errata workarounds and
replicate them.

* **`pStatus[0x84]` gates the whole `0x384` DMAPAD workaround.** `mtx3229`
  @0x24e0 defaults it to **1**, and `mtx2839` @0x38cef then overwrites it with a
  byte the *client* supplies in the 0x80-byte `START` payload (ioctl
  `0x40084616`). When it is 0 the driver skips the last-register tracking and
  every `0x384` write. Whether an open driver may leave it off is unknown; the
  vendor default is on.
* **`mtx3376` @0xa80** prints `Parhelia patches applied: %s` and the patch names
  live in `.rodata.str1.1`: `PowerM`, `EOBTag`, `Cap66Mhz`, `CompBypass`,
  `BmLocal`, `PciRead`. They map onto the arm sequence of §1.1 — `0x10b0 = 0x10`
  is *PowerM*, `0x12b8 = 0x00070601` is *EOBTag*, `0x308` is *CompBypass*, the
  Intel host-bridge quirk bit in `0x300` is *PciRead* — but the bit semantics of
  all four registers remain unknown. [mapping INFERRED]
* **`mtx514` @0x8f7c0**, the `0x910`/`0x914`/`0x938` port: writes
  `0x910 = ((arg << 9 | sub) & 0xfff) | ((idx & 0xf) << 24)`, then
  `0x938 = 0xfffffffc`, then reads `0x914` **exactly four times**, keeping the
  last. A fixed-count read, not a poll — presumably a read-pipeline latency
  requirement.
* **`pStatus[0x70] == 2`** (§4.8) — nothing in the core writes it.
* **`0x400`'s `& ~0x1f` masking** when a capability bit is set (§4.6).
* **`0x314 = 1`**, written in bursts of four and eight during the arm sequence and
  17 times over a full session, from the X driver's own BAR1 mapping.
* **`0xbc0 = 0x4087`**, a memory-configuration value from a capability vtable.
* **`pStatus[0xc0]`** holds an MMIO base pointer inside the *shared* page (used by
  `mtx508`, `mtx984`, `ParhlXAASync`), with `+0xc8`/`+0xcc` holding the register
  BAR's physical address and size. Storing a mapping pointer in a page shared
  with user space is unusual; whether it is per-process or kernel-only is not
  determined.
* **The 21 anonymous init/undo steps** (§5.2).
* **The `0x00000004` count prefix** on some ring elements but not others (§2.4a).
* **Control-block op `0x04`'s second effect**: after the idle wait the ICD stamps
  *all* per-8 KB-page fence tags free. Whether the wait is what makes that safe,
  or the driver is simply discarding pending work, is not established.
* **Record opcodes `0x0d`** (32-byte element `2,0,3,0,0,0,8,3`) and **`0x35`**
  (16-byte element, all zero) — byte-identical in every capture, emitted as the
  first two records of every state-carrying stream.
* **Record `0x24`**, always `3, 5`.
* **The `0x80808080` dword** in every CLEAR record.
* **Why the 32-entry attribute block** is initialised to `0` in the default-state
  stream and to `0xffffffff` at the first draw.

### 5.12 A stream-grammar conflict worth resolving

Two independent readings of `mtx2434`'s header-repeat feature disagree on which
indices carry it:

* `docs/parhelia-kernel-abi.md` §6.2: index `0x2a`/`0x2b` (`0x10a8`/`0x10ac`).
* This pass: index `0x22`/`0x23` (`0x1088` = saved header template,
  `0x108c` = repeat count), with `0x108c` additionally forcing a header reload
  when written.

Both are reads of the same function. Settle it before using stream compression;
neither reading affects the draw path documented here, which uses no repeats.

## 6. Open items this document does not settle

Ordered by how much they block an open driver.

1. **The state-id to register mapping** (§0). Until the 172 arms of
   `.rodata+0xba0` in `mtx2617` @0x19950 are dumped, no driver can go from a GL
   call straight to a register write. This is the single highest-value next step
   and it is a mechanical read of one jump table.
2. **The 21 anonymous init/undo steps** of the device start table at
   `.rodata+0x1860` (§5.2). Only step 19, the WARP unit, is identified. A
   from-scratch bring-up needs to know what the other 21 program.
3. **A second texture unit** (§3.4) — entirely uncaptured, and the cheapest gap
   to close: one more sweep run.
4. **The `0x51` record** — one `(index, 0)` pair, index 2 then 1, splitting the
   default-state stream into two identical halves. Read here as a render-target
   selector; unproven.
5. **Which fence counter is authoritative** — the client's descriptor tail or the
   kernel's `mtx558` epilogue (§2.4a).
6. **The header-repeat indices**, `0x22`/`0x23` versus `0x2a`/`0x2b` (§5.12).
7. **The DRAW record's `flags` field** at `+0x04`, observed 0, 1, 2, 4, 6, 0x18.
8. **`0x408` advancing by `0x20`** per submission versus a `0x14` ring-pointer
   increment (§1.1).
9. **Whether `pStatus[0x84] = 0`** — turning the `0x384` DMAPAD workaround off —
   is safe (§5.11).
10. **Vertex programs.** Control op `0x13` and record opcode `0x45` both exist
    and neither is exercised by any capture; the fixed-function path runs on the
    resident WARP microcode instead.
11. **The TEXTURE-DEF tail**, bytes `+0x30..+0x44`, which differ between the two
    texture captures (§3.3) and must contain the filter and border state.

## 7. Reproducing the decodes in this document

```
python3 tools/parhelia-glsweep-decode.py captures/glsweep-0528-raw.txt
```

is the existing decoder and predates the record grammar in §0 — it will
reproduce the per-feature deltas but mis-frames `(0x0c, N)` and `(0x05, N)`
headers as register pairs. The framing used here is the one described in §0:
`(opcode, count)` plus `count` elements of the per-opcode stride, validated by
requiring the stream to end exactly at `region0 + 0x230`. Any re-implementation
should keep that check as its self-test — it is what caught every mis-framing.

## Verification notes from the main session

**The CLEAR rect form is static-only, not yet proven on hardware.** Two
independent reverse-engineering passes read the last four dwords of the CLEAR
record as corners `(x1, y1, x2, y2)` rather than `(x, y, w, h)`, which is good
static support. But our captures **cannot distinguish the two**: every captured
clear covers the whole 256x256 window, where both readings give the identical
`0, 0, 0x100, 0x100`. Decoded payload:

```
clear_color  mask=1 argb=ff000000 depth=3f800000 stencil=0 accum=80808080
             rect = 0x0 0x0 0x100 0x100
```

The experiment that settles it is one line in the sweep harness: clear a
**sub-rectangle** (say 32,48 to 96,144) and read the four dwords back. Until
then, treat the corner reading as INFERRED.

The same payload independently corroborates a separate finding: the fifth dword
`0x80808080`, long recorded as a mystery constant, sits exactly where an
accumulation-buffer clear value belongs.

**Confirmed by three independent passes**, and therefore safe to build on: the
region-2 records are `[key][count]` headers followed by typed or paired payload,
and the region-2 indices are GL state slots rather than MMIO register offsets.
This recipe, the render-target pass and the fragment-operations pass reached
that conclusion separately, and a mechanical re-walk of a capture consumes a
stream exactly under that grammar.

## Hardware attempt, 2026-09-05 — microcode correct, still no triangle  [HW]

First run with the **seven real WARP programs** loaded instead of the mtx2785
self-test (tools/warp_programs.h, extracted by symbol).

**What now works, verified on a live 0x0528:**

```
mtx3605 gates: idle=1 908=1 924=1 busy0=1  (0x908=63af8041 0x924=00200100)
  ... all seven streamed to 0x91c ...
loaded 7 WARP programs, 2664 bytes (222 instructions)
```

Every gate passes, all seven programs stream cleanly at consecutive offsets, the
byte and instruction counts match the RE'd figures exactly, no kernel errors, X
survives. The `--physbuf` path was wired into the ring submitter so the
pushbuffer can live in VRAM — this removes the dependency on our `dma32buf`
module, which cannot be built on the test box because its kernel build tree is a
dangling symlink.

**What still does not work:** the draw submits without faulting and rasterises
**nothing**. `VRAM non-bg px = 0`.

**A false positive to be aware of:** the broad full-VRAM scan reported "3077
R/G/B px, first byte 0xec00c8". That address is `0xec0000`, the **cursor
aperture** documented in docs/parhelia-cursor-capture.md — it is the hardware
cursor image, not a triangle. Any future scan must exclude it.

**Why it almost certainly still fails**, given what the RE now says:

1. **Program binding is not implemented.** Loading the images is only half the
   job. The engine keeps an 18-entry program table and a draw binds *several*
   programs at once by entry point (`prog[0x0c]`), chosen from state. Nothing in
   the tool tells the WARP which entry point to run, so a loaded program is
   never invoked. See docs/parhelia-vertex-pipeline.md.
2. **State goes through the block-load path, not raw register writes.** Raster
   state reaches the hardware as a computed 16-byte block entry shipped by an
   opcode-0x41 block-load packet, not as the individual register writes the tool
   emits. See docs/parhelia-state-slot-map.md.

Both are now understood well enough to implement; neither was implemented at the
time of this run. The value of the run is that it isolates the remaining gap to
those two items and confirms the microcode half is correct.
