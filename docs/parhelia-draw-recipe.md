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

## 0. The record grammar — settled, and it corrects earlier readings

Everything in §1–§3 is expressed in one format, so it is stated first.

The 136 KB region ("command record stream") is rebuilt **from byte offset 0 on
every submission**. It is a flat sequence of records:

```
record := u32 opcode ; u32 count ; count * <element>
```

with a **per-opcode element size**. This framing was validated by decoding four
independent streams end-to-end and landing exactly on the byte length the driver
publishes in `region0 + 0x230`: `0x11f0`, `0x1188`, `0x1078`, `0x190`, `0x30`.
[CONFIRMED]

| opcode | element | name | payload |
|---|---|---|---|
| `0x05` | 8 | PAIRS-05 | `(index, value)` in state group 05 |
| `0x06` | 8 | SAMPLER | `((sub << 16) | stage, value)` |
| `0x07` | 24 | PIPELINE-KEY | `key, 0, 0, ffffffff, ffffffff, ffffffff` |
| `0x08` | 32 | VERTEX-FORMAT | `0, 0, u64 kptr, stride, 0, mask, 0` |
| `0x0a` | 72 | TEXTURE-DEF | see §3.3 |
| `0x0c` | 8 | PAIRS-CORE | `(index, value)`, register `0x1000 + index*4` |
| `0x0d` | 8 | PAIRS-0D | `(index, value)` in state group 0D |
| `0x0e` | 8 | TEXENV | `((sub << 16) | stage, value)` |
| `0x12` | 16 | SCISSOR-RECT | `x, y, w, h` — screen pixels, **Y from the bottom** |
| `0x15` | 36 | CLEAR | `mask, ARGB, depth_f, stencil, 0x80808080, x, y, w, h` |
| `0x1b` | 40 | DRAW | see §2 |
| `0x20` | 68 | MATRIX | `matrix_id, 16 floats` |
| `0x21` | 60 | LIGHT | two elements per 0x78-byte light block |
| `0x22` | 72 | MATERIAL | `face, diffuse4, ambient4, specular4, emission4, shininess` |
| `0x23` | 16 | LIGHTMODEL-AMBIENT | 4 floats |
| `0x24` | 8 | **[PURPOSE UNKNOWN]** | always `3, 5` |
| `0x25` | 20 | CLIP-PLANES [INFERRED] | 6 elements `plane_id, 0,0,0,0` |
| `0x26` | 16 | VIEWPORT | `x, y, w, h`, same space as `0x12` |
| `0x27` | 8 | DEPTH-RANGE | 2 floats, `(0.5, 0.5)` for the GL default |
| `0x36` | 8 | LINE/POINT-SIZE | 2 floats |
| `0x46` | — | FRAGPROG-UPLOAD | `progid, 0` then a **0x664-byte** blob, record padded to `0x680` |
| `0x51` | 8 | TARGET-SELECT [INFERRED] | one `(index, 0)` pair; index = render-target id |

### Three corrections this framing forces on the existing docs

1. **`(0x0c, N)` is a record header, not `reg 0x1030 = N`.** Index `0x0c` maps
   to register `0x1030`, so a `0x0c` header decodes as a plausible-looking
   register write. This is the whole of the "`0x1030` (index 0x0c) is
   multiplexed or mis-sliced" ambiguity in `docs/parhelia-gl-feature-map.md` §6:
   `scissor`, `cullface`, `fog` and `lighting` show "`0x1030 = 1`" because each
   emits a one-entry PAIRS-CORE record; `polygon mode` shows "= 2" because it
   emits a two-entry one. [CONFIRMED]
2. **`(0x05, N)` is likewise a header, not `reg 0x1014 = N`.** Feature-map
   contradiction #5 ("`glEnable(GL_BLEND)` writes `0x1014 = 4`") is a mis-framed
   `(0x05, 4)` header. The blend op's real writes are listed in §3.2.
   [CONFIRMED]
3. **`reg 0x1050` is reached through state group `0x05`, index `0x14`.** The
   published reading `(0x14, 3) -> 0x1050 = 3` came from framing a
   `(0x05, 2)` record as two pairs. The real record is
   `(0x05, 2) { (0x13, 1), (0x14, progid) }`. The *value* claim survives —
   it still matches the fragment program's own id — but the record shape does
   not. [CONFIRMED]

**Groups `0x05`, `0x0c` and `0x0d` all carry small `(index, value)` pairs whose
index ranges overlap.** Only group `0x0c` is proven to map to
`0x1000 + index*4` (eight independent GL calls land on exactly the predicted
registers). Treat `0x05` and `0x0d` as *separate state groups* whose register
mapping is [UNRESOLVED]; do not assume `0x1000 + index*4` for them.

### The clobbered first record — resolved

`glsweep` reads the stream after `glFinish` and always sees `(0xff, 1)` in the
first two dwords. That is the kernel overwriting the record header after
consuming the stream: it is visible in `mtxtrace` as a lone 4-byte change to
offset `0x000` in every submission that *follows* a render. `mtxtrace` snapshots
around the doorbell instead and therefore captures the **real** first opcode.
The "unrecoverable first token" caveat in
`docs/parhelia-command-construction.md` no longer applies to mtxtrace captures.
[CONFIRMED]

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
11  W 0x400 = ringLimit                      [PURPOSE UNKNOWN — only ever written here]
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
  md5 `b29644ecfcfcbbc11ef97240872892ca`, extractable with
  `tools/parhelia-extract-ucode.c`. [CONFIRMED]
* **The DMAPAD rule applies to every register write.** If the previous register
  offset written was within 128 bytes *below* the new offset, write `0x384 = 0`
  first. A register *read* cancels the pending pad.
* **`0x408` advances by 0x20 per submission in the trace** (`0x14600023`,
  `…43`, `…63`, …), while `mtx1937`'s disassembly increments the ring write
  pointer by `0x14`. One of the two readings is incomplete. [UNRESOLVED]
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
0x0378  (0x0e,112)   texture environment        -- 4 stages x 28 sub-indices
0x0700  (0x05, 24)   state group 05             -- 24 pairs
0x07c8  (0x06, 36)   sampler state              -- 4 stages x 9 sub-indices
0x08f0  (0x51,  1)   TARGET-SELECT  index 1
0x0900  (0x0c,108)   ... the identical four blocks again, for target 1
0x0c68  (0x0e,112)
0x0ff0  (0x05, 24)
0x10b8  (0x06, 36)
0x11e0  (0x0c,  1)   reg 0x10f8 = 1
```

So: **the whole default state is emitted twice, once per render target**, and
the `0x51` record selects which. `CB[0x08]` at the RENDER is 1, matching the
second block. [INFERRED — the two-copies-with-a-selector structure is plain in
the capture; that the selector means "render target" is the reading, not a proof]

`reg 0x10f8` closes the block with 1, and is the register `SwapBuffers` toggles
(§4.4). [CONFIRMED]

#### Core render state, group `0x0c` (register = `0x1000 + index*4`)

Emit all 108, in this order. Values are the GL defaults.

```
0x101c=0          depth test off        0x1020=3          polygon mode FILL
0x1024=2          blend src RGB = ONE   0x1028=0x0001ffff
0x1030=0x0000000c                       0x1038=1          depth write enable
0x103c=0          alpha test off        0x1040=0
0x104c=2                                0x1050=1          fragment program select
0x1058=3          cull: disabled        0x105c=4          depth func LEQUAL
0x1060=0          alpha ref             0x1064=8          alpha func ALWAYS
0x1068=1                                0x106c=0          blend off
0x1070=0          fog off               0x1074=0
0x1078=0                                0x1088=0
0x108c=0                                0x1090=0          fence address
0x1094=1.0f       fence value           0x1098=1
0x10a0=0                                0x10a4=0
0x10bc=0                                0x10c0=0
0x10d0=0          stencil off           0x10d4=1
0x10d8=1                                0x10dc=1          stencil op
0x10e0=8                                0x10e4=0          stencil ref
0x10e8=0xffffffff stencil mask          0x10ec=0xffffffff
0x10f0=0
0x1100..0x117c = 0    (32 registers, the vertex/attribute file)
0x1200..0x121c = 0    (8 registers)
0x1220=1          0x1224=1          0x1228=0          0x122c=0
0x1230=0          0x1234=0          0x1238=1          0x123c=0
0x1244=0          0x1248=0          0x124c=0          0x1250=0
0x125c=0          0x1260=0          0x1264=0          0x1268=1.0f
0x126c=1.0f       0x1270=0          0x1274=0          0x1278=1.0f
0x127c=0          0x1280=0          0x1284=0          0x1288=0xffffffff
0x128c=0          0x1290=0          0x1298=4096.0f    0x129c=0
0x12a0=0x0000000f 0x12a8=1.0f       0x12ac=1
```

Note `0x1100–0x117c = 0` here, whereas the *first draw* stream sets the same
32 registers to `0xffffffff` (§2). Both values are real; the attribute file is
written twice with different content. [CONFIRMED, purpose of the difference
UNKNOWN]

#### Texture environment, group `0x0e`

112 entries = **4 stages x 28 sub-indices**. The index dword is
`(sub << 16) | stage`; sub runs `0x00..0x1c` skipping `0x0c`. Emission order is
sub-major: all four stages of sub 0, then all four of sub 1, …

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

36 entries = **4 stages x 9 sub-indices** (sub `0x01..0x09`), identical for all
four stages:

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
0x0000  64-byte prologue                     [PURPOSE UNKNOWN, framing ambiguous]
                                             byte-identical in every capture:
                                             0000000d 00000001 00000002 00000000
                                             00000003 00000000 00000000 00000000
                                             00000008 00000003 00000035 00000001
                                             00000000 00000000 00000000 00000000
0x0040  (0x0c, 36)  core state:
            reg 0x105c = 2                   depth func LESS
            reg 0x1098 = 1.0f
            reg 0x126c = 0
            reg 0x1298 = 2048.0f             guard-band / viewport scale
            reg 0x1100..0x117c = 0xffffffff  the 32-register attribute file
0x0168  (0x12,  1)  SCISSOR-RECT   0, 0, 256, 256
0x0180  (0x46,  1)  FRAGPROG-UPLOAD  progid 2, 0x664-byte blob
0x0800  (0x05,  2)  group 05: index 0x13 = 1 ; index 0x14 = 2  (select progid 2)
0x0818  (0x26,  1)  VIEWPORT       0, 0, 256, 256
0x0830  (0x36,  1)  LINE/POINT-SIZE  1.0, 1.0
0x0840  (0x0c,  1)  reg 0x1230 = 1
0x0850  (0x24,  1)  3, 5                     [PURPOSE UNKNOWN]
0x0860  (0x23,  1)  light-model ambient 0.2, 0.2, 0.2, 1.0
0x0878  (0x0c,  2)  reg 0x1238 = 0 ; reg 0x1224 = 0
0x0890  (0x21, 24)  12 light blocks of 0x78 bytes, GL defaults
0x0e38  (0x22,  2)  materials, front and back, GL defaults
0x0ed0  (0x27,  1)  depth range 0.5, 0.5
0x0ee0  (0x25,  6)  six clip planes, all zero
0x0f60  (0x20,  6)  six 4x4 identity matrices, ids 1, 3, 0x10, 0x11, 0x12, 0x13
0x1100  (0x0c,  1)  reg 0x1058 = 1           cull face off
0x1110  (0x07,  1)  PIPELINE-KEY = 0x00000010
0x1130  (0x08,  1)  VERTEX-FORMAT  stride = 20
0x1158  (0x1b,  1)  DRAW
```

The light, material, clip-plane and matrix records are the fixed-function T&L
state; the WARP unit runs transform *and* lighting, so a driver that wants the
vendor's pipeline must supply them. Matrix ids `0x10..0x13` are the four texture
matrices. [INFERRED from the ids and the identity contents]

### 2.2 The DRAW record — `(0x1b, 1)`, 40-byte payload

```
+0x00  u32   primitive     the raw GL enum: POINTS 0, LINES 1, TRIANGLES 4,
                           TRIANGLE_STRIP 5, TRIANGLE_FAN 6, QUADS 7
+0x04  u32   flags         [PURPOSE UNKNOWN] observed 0, 1, 2, 4, 6, 0x18
+0x08  u64   kernel pointer to the draw/context object (from buffer create)
+0x10  u64   descriptor offset in the command ring      = slot
+0x18  u64   descriptor end                             = slot + 0x1d
+0x20  u64   vertex data offset in the vertex ring      = slot + 0x40
```

`slot = draw_index * 0x2000` in **both** rings. Verified across five consecutive
draws: `0x0000, 0x2000, 0x4000, 0x6000, 0x8000, 0xa000`. [CONFIRMED]

The ICD passes the GL primitive enum straight through; it does **not** decompose
strips, fans or quads. The kernel turns the enum into the `0x10b0` value listed
in `docs/parhelia-command-construction.md`. Only wide lines are CPU-emulated.
[CONFIRMED]

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
slot+0x0c   0x00000002    register-write list, 2 registers
slot+0x10   0x21212425    index tag: 0x25 -> reg 0x1094,
                                     0x24 -> reg 0x1090,
                                     0x21 -> END OF PACKET (twice)
slot+0x14   fence value   -> reg 0x1094   = the submission serial
slot+0x18   fence address -> reg 0x1090   = the serial page's bus address
```

Captured verbatim for five consecutive draws with only the fence value changing
(2, 3, 4, 5, 6, 7) and the address constant. [CONFIRMED]

Index `0x21` **terminates** a packet; index `0x20` (`reg 0x1080`, DMAPAD) is the
mid-packet filler. Getting these the wrong way round produces a stream the
engine mis-parses.

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

Each subsection lists **only the delta** against §2. All of it comes from
single-feature captures, so the attribution is direct — with the one caveat that
the ICD flushes state lazily, so a capture's stream also carries the *cleanup* of
the previous operation. Those cleanup writes are marked.

### 3.1 Depth test

`glEnable(GL_DEPTH_TEST)` + `glVertex3f`, total stream `0x88` bytes:

```
(0x0c, 1)   reg 0x101c = 1                depth test enable
(0x07, 1)   PIPELINE-KEY = 0x00000011     was 0x10
(0x08, 1)   VERTEX-FORMAT stride = 24     was 20 — z appears
(0x1b, 1)   DRAW
```

Related, each a single PAIRS-CORE pair:

| GL call | record | register | value |
|---|---|---|---|
| `glDepthFunc(f)` | `(0x0c,1) (0x17, v)` | `0x105c` | `f - GL_NEVER + 1`, i.e. NEVER 1 … ALWAYS 8 |
| `glDepthMask(FALSE)` | `(0x0c,1) (0x0e, 0)` | `0x1038` | 0 / 1 |
| `glDisable(GL_DEPTH_TEST)` | `(0x0c,1) (0x07, 0)` | `0x101c` | 0 |

The **vertex stride grows to 24** and the position attribute becomes `x,y,z`;
the colour follows it. The DRAW record and the ring packet are unchanged.
[CONFIRMED]

### 3.2 Blending

`glEnable(GL_BLEND)` + `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`,
total stream `0xd0` bytes:

```
(0x0c, 6)   reg 0x106c = 1        blend enable
            reg 0x1014 = 4        [PURPOSE UNKNOWN — blend equation? always 4 here]
            reg 0x1028 = 5        source alpha factor   = GL_SRC_ALPHA
            reg 0x1024 = 5        source RGB factor
            reg 0x1030 = 6        dest alpha factor     = GL_ONE_MINUS_SRC_ALPHA
            reg 0x102c = 6        dest RGB factor
(0x0c, 3)   reg 0x101c = 0 ; reg 0x105c = 2 ; reg 0x1038 = 1   [previous op's cleanup]
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
(0x0c, 4)   reg 0x103c = 1        alpha test enable
            reg 0x1064 = 5        alpha func GREATER (same 1..8 enum as depth)
            reg 0x1060 = 0x80     reference, 8-bit: round(0.5 * 255)
            reg 0x106c = 0        [previous op's cleanup]
```

### 3.3 A texture

Total stream `0x1078` bytes (mtxtrace, 16x16 RGBA) or `0x808` (glsweep, 64x64).
Both decode identically in structure:

```
(0x0a, 1)   TEXTURE-DEF        80-byte record, layout below
(0x0e, 1)   TEXENV  stage0 sub 0x00 = 4
(0x46, 1)   FRAGPROG-UPLOAD    progid, then a 0x664-byte blob; record padded to 0x680
(0x05, 2)   index 0x13 = 1 ; index 0x14 = progid      SELECT FRAGMENT PROGRAM
(0x0c, 1)   reg 0x1224 = 0
... the usual light / material / matrix / clip-plane revalidation ...
(0x0e, 1)   TEXENV  stage0 sub 0x18 = 2
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

| GL | index | register | value |
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

`glClear` is a `(0x15, 1)` record:
`[mask][ARGB8888][depth float][stencil][0x80808080][x][y][w][h]`, mask
1 = colour, 2 = depth, 3 = both, rect in screen pixels. The `0x80808080` dword
is constant in every clear. [PURPOSE UNKNOWN]

Wide lines are **CPU-emulated**: `glLineWidth(4.0)` emits pipeline key `0x852`,
stride 32, *window-space* pre-transformed vertices, and four parallel 1-pixel
lines. Do not look for a width register.

---

## 4. Synchronisation

Four distinct mechanisms, at four different levels. A driver needs all four.

### 4.1 The indexed-write FIFO — `0x450` bit 22

Registers `0x1000–0x13FF` go through a **16-entry** write FIFO. The vendor
counts writes in the shared register shadow (`pRegShadow[0x90]`) and, on reaching
16, polls `0x450` until bit 22 (FIFO empty) is set, then resets the count.
Also polled at ring rewind, where the wait is `0x450 == pRegShadow[0x9c]`
(an expected value the driver maintains). [CONFIRMED — `mtx3863` @0x34e04,
`mtx3948`, `mtx2434`]

**When:** before any register write that would be the 17th outstanding one, and
before emitting a `0x12BC` fence tag.

### 4.2 The engine fence — `0x12BC` write / `0x41C` read

```
tag = value & ~1
poll 0x450 until bit 22 set            (drain the indexed FIFO first)
write 0x12BC = tag                      SOFTRAP: emit the tag
if (value & 1)
    do { read 0x41C } while (result != tag);
```

Tags come from `pStatus[0x44]`, **incremented by 8** each time and **skipping
0**; **bit 0 means "block until complete"**. The fence commit block is nine
dwords:

```
0x21c0c0c0  0x0307  0x00000004  0x0707
0xc0afafdd  0x00000000  fence1|1  fence2|1  shadow(0x1300)
```

which decodes as `0x1300 <- 0x0307, 4, 0x0707`, packet end, then
`0x1374 <- 0`, `0x12BC <- fence1|1`, `0x12BC <- fence2|1`, `0x1300 <- saved`.
Padding to a `0x1000` boundary uses `0x21212121`. [CONFIRMED — `mtx3144`
@0x31c30]

### 4.3 The per-submission serial — how completion reaches user space

This is the mechanism a GL client actually waits on, and it needs **no ioctl and
no interrupt**. Every draw packet carries:

```
reg 0x1090 = bus address of a 4-byte completion word
reg 0x1094 = the value to write there
```

`0x1090` is the mmap cookie of the PCI_LOCKED serial page created by
control-block op `0x0a` (§1.2) — for that memory class the cookie *is* the
physical/bus address. `0x1094` is a monotonic submission serial. Across five
draws the packets carried 3, 4, 5, 6, 7 and the serial page read back exactly
those values. `glFinish` is "spin until `*serialPage >= myValue`".
[CONFIRMED, both halves observed in the same capture]

**A driver must therefore allocate one DMA-visible page per context, hand its
bus address to `0x1090` in every packet, and poll it.** There is no completion
ioctl in this ABI.

### 4.4 Ring-level status

| register | test | used for |
|---|---|---|
| `0x8f0` bit 17 | set | primary DMA finished — before a ring rewind and before a ucode load |
| `0x8f0` bits 24–25 | zero | WARP idle |
| `0x908` | `(v & 0xfff7ffbf) == 0x63a78001` | WARP idle signature |
| `0x924` | `(v & 0xfff) == 0x100` | WARP pipe ready |
| `0x318` / `0x31c` | `(v & mask) == want`, mask/want `1`/`1` or `0x57`/`0x50` | pre-submit gate on the non-DMA post path |
| `0x8f4` / `0x8f8` / `0x8fc` | read / write-back-to-ack / enable mask | interrupts |

Ring rewind (`mtx2508` @0x31880) is the full sequence: spin on the shared lock
`pRegShadow[0x78]`, wait `0x8f0` bit 17, wait `0x450 == pRegShadow[0x9c]`, reset
the FIFO counter, write `0x404 = ringBase & ~3`, write `0x400 = limit`.

### 4.5 What the ICD itself does per frame

From the capture, in order, around `glFinish`:

```
CB[0x00] = 1        RENDER
CB[0x234] = 0xffffffff   client clears the result slot
ioctl 0x4621
                    kernel writes CB[0x234] = 0 on success, -1 invalid op, -3 not ready
                    kernel overwrites the stream's first record header with (0xff, 1)
                    the engine writes the serial page
```

`CB[0x234] == -3` means the context is not ready (no bound render target); the
kernel returns 0 from the ioctl anyway, so **the ioctl return value is not the
error channel — `CB[0x234]` is**. [CONFIRMED — `mtx903` @0x1c60]

Also observed but not explained: control-block op `0x1e` is issued immediately
before the first RENDER and again after the last draw, with `CB[0x10] = 1`, and
it writes four bytes at offset 0 of region 1. It is the only op that touches
region 1. [PURPOSE UNKNOWN]
