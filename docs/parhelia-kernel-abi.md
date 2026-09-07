# Matrox Parhelia — vendor kernel driver ABI (device init, memory, submission)

> **Superseded in part — see `parhelia-3d-integrated.md` §8.** The `reg = 0x1000 + slot*4` rule is refuted; `0x400` is contested (measured as a status/write-back page under a live context).
Provenance, three sources:

1. **Open C source.** `matroxdriver-x86_64-cvs-20090511/kernel/src/*.c` and
   `include/` are the *real vendor sources* for the whole Linux glue layer.
   Only the HAL core is binary. Quotes below are verbatim from those files.
2. **`matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o`** — the core, an
   unstripped ELF64 relocatable whose symbols were renamed to `mtxNNNN`.
   `@0x…` addresses are `.text` offsets (`objdump -d -r`).
3. **`matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so`** — the X
   driver, which *retains unmangled C++ symbols* (`OsPostBusmasteringBuffer`,
   `BmBufferPool`, `CommonData`, `SharedLockedData_t`, `MmSurface`…). It is the
   same codebase as the core, so it supplies real names for the `mtxNNNN`
   functions. This is the single most useful naming oracle in the tree.

Tags: **[CONF]** read directly from source or disassembled instructions ·
**[HW]** from a live capture on a real 0x0528 board · **[INF]** inferred, not
proven · **[OPEN]** not determined.

Cross-refs: `docs/parhelia-gpu-access-map.md` (index),
`docs/parhelia-mmio-capture.md` (mmiotrace method),
`docs/parhelia-command-construction.md` (packet payloads).

---

## 1. Layer split

Two layers, joined by `mtx_core_driver_t` (`src/mtx_coreintf.h`). **[CONF]**

* **Generic layer** — open source, `src/mtx_*.c`. Char device
  `/dev/matrox/mtx<minor>`, dynamic major, ioctl magic `'F'`. Owns memory
  regions, `mmap`, the ioctl dispatch table, IRQ registration. Knows nothing
  about the hardware.
* **Parhelia core** — `mtx_parhelia.o`, exporting exactly one symbol,
  `mtx_parhl_driver` at `.data+0x00`.

### 1.1 `mtx_parhl_driver` **[CONF]** (`objdump -r --section=.data`, offsets 0x00–0x38)

| slot | field | symbol | `.text` |
|---|---|---|---|
| +0x00 | `init` | `mtx2113` | 0x490 |
| +0x08 | `cleanup` | `mtx1700` | 0x640 |
| +0x10 | `enable` | `mtx2007` | 0xea0 |
| +0x18 | `disable` | `mtx547` | 0x8d0 |
| +0x20 | `open` | `mtx2874` | 0x6e0 |
| +0x28 | `close` | `mtx2307` | 0x760 |
| +0x30 | `irq_fast` | `mtx987` | 0x960 |
| +0x38 | `irq_slow` | `mtx2999` | 0xa10 |

`src/parhelia/MtxParhlDrv.h` describes `MtxParhlDevice` and `MtxParhlContext`
accurately — verified against `mtx987`, which indexes `pvPendingIRQ` at +0x28,
`byNbPendingIRQ` at +0x48, and `aoCommonBufferTable[4]` at +0x08…+0x20. **[CONF]**

### 1.2 Two independent submission surfaces **[CONF]**

The auth/root split in `mtx2113` (§3.2) means there are **two** paths to the
same ring, and a from-scratch driver only needs one of them:

* **X server / 2D (root)** — `ioctl 0xc018461b` (`OsPostBusmasteringBuffer`).
* **GL client / 3D (auth)** — `ioctl 0x4621`, a no-argument doorbell over a
  shared request buffer (§5).

Both converge on "append a packet to the primary ring, write `0x408`".

---

## 2. Memory management

### 2.1 Region model **[CONF]** (`src/mtx_drv.h`, `mtx_strintf.c`, `mtx_mem.c`)

```c
typedef struct mtx_region {
    struct list_head list_node;
    ULONG_PTR ctx_id;      /* owning context, -1 if kernel/shared */
    ULONG_PTR base;        /* THE MMAP COOKIE (see below)         */
    ULONG_PTR busaddr;     /* kernel-private, never exported      */
    void     *kaddr;       /* kernel virtual                      */
    ULONG     size, real_size;
    mtx_memory_type  type;
    mtx_memory_scope scope;
    ULONG io_flags; BYTE flags;
    atomic_t ref_count; INT mtrr;
} mtx_region_t;
```

**`mmap` offset is `region->base`, matched exactly** — `mtx_mmap()` calls
`MtxRegion_FindFromBase(dev, vm_pgoff << PAGE_SHIFT)`, which is an `==`
comparison over the device region list. There are no ranges and no sub-offsets:
one `mmap` per region, from its exact cookie. **[CONF]**

| `mtx_memory_type` | val | backing | `base` | mmap path |
|---|---|---|---|---|
| `MTX_MEMTYPE_IO` | 0 | PCI BAR | BAR physical | `remap_pfn_range` |
| `MTX_MEMTYPE_PCI_LOCKED` | 1 | `__get_free_pages(GFP_KERNEL\|__GFP_DMA32, order)`, `MTX_MAX_ORDER 9` ⇒ **2 MB max** | `__pa(kaddr)`, `busaddr = virt_to_bus()` | `remap_pfn_range` |
| `MTX_MEMTYPE_AGP` | 2 | agpgart | aperture address | `remap_pfn_range` |
| `MTX_MEMTYPE_SYS` | 3 | `vmalloc()` | **`kaddr - VMALLOC_START`** | `->fault` + `vmalloc_to_page` |

The `MTX_MEMTYPE_SYS` cookie is a deliberate synthetic value; the source says
why: on 64-bit kernels `VMALLOC_START - PAGE_OFFSET` exceeds what `mmap2`/
`mmap64` can carry, so a 32-bit GL client could not otherwise map vmalloc'd
memory. **[CONF]**

Scopes (`src/mtx_mem_type.h`): `KERNEL=0` (never mappable — `mtx_mmap` returns
`-EPERM`), `PRIVATE=1`, `ROOT=2`, `AUTH=3`, `ALL=4`. **[CONF]**

**A buffer's bus address is never returned to userspace.** `GET_BUFFER` yields
only `base`. `busaddr` stays kernel-side, and the kernel translates
`base → bus address` at submission time through a `std::map`-style registry
(`mtx3817`, keyed by both base and page-aligned address, rooted at
`CommonData+0x2118`). Every allocator calls it right after allocating. **[CONF]**

### 2.2 `OsAllocLockedPages` — the one allocator everything funnels through **[CONF]**

`src/mtx_os.c`:

```c
eMemType = bAgp ? MTX_MEMTYPE_AGP
                : (bContiguous ? MTX_MEMTYPE_PCI_LOCKED : MTX_MEMTYPE_SYS);
region = MtxRegion_Alloc(ctx, ulNbPages * PAGE_SIZE, eMemType, MTX_MEMSCOPE_AUTH, 0);
return region->base;
```

Core-side wrappers, all `(hctx, unusedSize, nPages, &kaddr, &base)`:

| wrapper | `.text` | `bAgp` | `bContig` | resulting type |
|---|---|---|---|---|
| `mtx3583` | 0x56f80 | 1 | 1 | AGP |
| `mtx586` | 0x570c0 | 0 | 1 | PCI_LOCKED |
| `mtx3185` | 0x571f0 | 0 | 0 | SYS |

`mtx938` @0x4c470 is the type-dispatcher: memory-type **1 → AGP**,
**2 → PCI_LOCKED**, **4 → SYS**; it rounds the byte size up to a page and
divides by `OsGetPageSize()` to get `nPages`. **[CONF]**

> Note: in all three wrappers the *second* argument is loaded by the caller and
> then dead — `nPages` is argument 3. A reading that takes argument 2 as the
> size will produce wrong buffer sizes. **[CONF]**

### 2.3 The four `CommonData` buffers **[CONF]**

`mtx1633` @`.text+0x0` is `OsGetCommonBuffers` (the name comes from
`mtx_drv.so`, which calls it through `_Z16GetCommonBuffersP4PDEV` @0x7a500).
Shared in/out struct, 0x30 bytes:

```
+0x00 u32 sizeCommonData        0xc9f0 (51696)  -> SYS,        scope KERNEL
+0x04 u32 sizeCommonLockedData  0x1             -> PCI_LOCKED, scope KERNEL
+0x08 u32 sizeSharedData        0x18            -> SYS,        scope ROOT
+0x0c u32 sizeSharedLockedData  0xae0 (2784)    -> PCI_LOCKED, scope ROOT
+0x10 ptr pCommonData          <- MtxRegion_GetLinear()
+0x18 ptr pCommonLockedData
+0x20 ptr pSharedData
+0x28 ptr pSharedLockedData
```

The two *Common* buffers are allocated lazily by the core with **scope KERNEL**,
so they are unmappable. The two *Shared* buffers are allocated by the X server
via `GET_BUFFER` (type 3 / type 1, scope 2) and only *looked up* here with
`MtxRegion_FindFromVirtual`. The sizes above are the literals in `mtx_drv.so`
@0x7a51d. **[CONF]**

Since `MtxRegion_GetLinear` returns `region->kaddr`, the pointers written into
+0x10…+0x28 are **kernel virtual addresses** — this is the origin of the
`0xffffc200_xxxxxxxx` values visible in shared mappings on 2.6.30. **[CONF]**

`CommonData` offsets in use: **[CONF]**

| off | contents |
|---|---|
| +0x10 | register-shadow state (§4.1); this *is* the Shared-locked buffer |
| +0x2030 | register `MEMHANDLE` |
| +0x2050 | interrupt manager |
| +0x2118 | address-translation registry |
| +0x2168 | primary bus-master ring object |
| +0x21a8 | video bus-master object |
| +0x2198 | device capability object |
| +0x2440 | feature flags (gate bits 0x70000 / 0x7e000, §6.1) |

---

## 3. The ioctl ABI

`MTX_IOCTL_MAGIC = 'F'` (0x46). Generic ioctls at NR offset 0, parhelia core at
NR offset 20 (`MTX_PARHL_IOCTL_NROFF`), `MTX_IOCTL_MAXNR = 40`. **[CONF]**

Argument-struct sizes were confirmed two ways: computed from
`src/mtx_ioctl.h`, and read back as immediates from both `mtx_drv.so` and
`libGL.so.1.3.0`.

### 3.1 Generic ioctls **[CONF]** (registered in `src/mtx_drv.c:mtx_init`)

| ioctl | value | argument | size | auth / root |
|---|---|---|---|---|
| `GET_MEMORY_INFO` | `0x80304600` | `mtx_ioctl_get_memory_info_t` | 48 | – / – |
| `GET_BUFFER` | `0xc0184601` | `mtx_ioctl_buffer_t` | 24 | auth / – |
| `RELEASE_BUFFER` | `0x40184602` | `mtx_ioctl_buffer_t` | 24 | auth / – |
| `DRIVER_VALUE_REQUEST` | `0xc03c4603` | `mtx_ioctl_value_t` | 60 | auth / – |
| `ADD_USER` | `0x00004604` | – | 0 | **never registered** |
| `REMOVE_USER` | `0x40044605` | `ULONG` | 4 | **never registered** |
| `GET_MINOR` | `0xc0104606` | `MtxIoctlPacket_GetMinor` | 16 | auth / – |
| `HARD_RESET` | `0x00004607` | – | 0 | auth / root, `#ifdef DEBUG` only |

`ADD_USER` / `REMOVE_USER` exist in the header but no `MtxIoctl_Register` call
references them — they are dead. `HARD_RESET` is compiled out of release
builds. A from-scratch driver needs neither. **[CONF]**

```c
/* GET_MEMORY_INFO — every member is __attribute__((aligned(8))), so the
   layout and the ioctl number are identical for 32- and 64-bit clients. */
struct { u64 fb_base; u32 fb_size; /*pad*/ u64 reg_base; u32 reg_size;
         /*pad*/ u64 agp_base; u32 agp_size; /*pad*/ };   /* 48 bytes */
/* offsets: fb_base 0, fb_size 8, reg_base 16, reg_size 24,
            agp_base 32, agp_size 40 */

/* GET_BUFFER / RELEASE_BUFFER */
struct { u32 type;            /* 0 */   /* mtx_memory_type              */
         u32 scope;           /* 4 */   /* mtx_memory_scope             */
         u32 createNewRegion; /* 8 */
         u32 size;            /* 12 */  /* in; out = region->size       */
         u64 base;            /* 16 */  /* OUT: the mmap cookie         */ };  /* 24 */

/* DRIVER_VALUE_REQUEST — reads/writes a module parameter by symbol name */
struct { u32 type;  /* 0 = PARM_WRITE (root only), 1 = PARM_READ */
         u32 value;
         char parm_name[50]; };                                       /* 60 */

/* GET_MINOR */
struct { u32 iBusNumber, iDeviceNumber, iFunction, iMinor /*out*/; };  /* 16 */
```

`GET_MEMORY_INFO` hands out `reg_base`/`reg_size`, and the register region is
type `IO` scope `AUTH`, so **userspace can mmap BAR1 and drive registers
directly**. That matters — see §9. **[CONF]**

### 3.2 Parhelia core ioctls **[CONF]** (`mtx2113` @0x490; all `bCoreIoctl=1`)

`MtxIoctl_Register(cmd, handler, bCoreIoctl, bRestrictedAccess, bSuperUserOnly)`.

| ioctl | NR | dir/size | handler | access | function |
|---|---|---|---|---|---|
| `0xc0804614` | 20 | RW 128 | `mtx913` @0xf70 | root | `ParhlEnableDevice` — create render context (`new 0x6d0`, init `mtx2839`) |
| `0xc0084615` | 21 | RW 8 | `mtx1612` @0x1070 | root | `ParhlDisableDevice` — destroy by handle |
| `0x40084616` | 22 | W 8 | `mtx3710` @0x10b0 | root | `LnxDevice::Start(PDEV*)` |
| `0x40044617` | 23 | W 4 | `mtx3446` @0x10f0 | root | `LnxDevice::Stop(PDEV*)` |
| `0xc0804618` | 24 | RW 128 | `mtx1545` @0x12a0 | root | `OsAlloc{Aperture,Surface}BlockMemory` |
| `0x40804619` | 25 | W 128 | `mtx3971` @0x15d0 | root | `OsFree{Aperture,Surface}BlockMemory` |
| `0xc060461a` | 26 | RW 96 | `mtx2230` @0x17a0 | root | `OsGetLocalMemoryInfo` — per-head info by index |
| `0xc018461b` | 27 | RW 24 | `mtx391` @0x18c0 | root | **`OsPostBusmasteringBuffer` — submit** |
| `0x4018461c` | 28 | W 24 | `mtx345` @0x1170 | root | `ParhlStartDevice` — bind buffers to a render context |
| `0x4010461d` | 29 | W 16 | `mtx532` @0x1920 | root | `OsSetVSyncInterruptState` |
| `0xc038461f` | 31 | RW 56 | `mtx1775` @0x1a50 | **auth** | ICD link **create** |
| `0x00004620` | 32 | `_IO` | `mtx2376` @0x1bf0 | **auth** | ICD link **destroy** |
| `0x00004621` | 33 | `_IO` | `mtx903` @0x1c60 | **auth** | **ICD doorbell** |

NR 30 (`0x…461e`) is unused. `mtx1700` (cleanup) unregisters the same 13.
The userspace names come from `mtx_drv.so`'s exported symbols; each was matched
by resolving the ioctl call site to the nearest preceding symbol. **[CONF]**

Notable arguments: **[CONF]**

* `0xc0804618` — `arg[0x20] == 1` selects a 0x78-byte *surface* record
  (heap `pDev->[0x2138]`, `mtx236`); otherwise a 0xc0-byte *buffer*
  (heap `pDev->[0x2130]`, `mtx3114`). Returns physical address, pitch, size.
* `0x4018461c` — `{u32 id0; u32 id1; u64 handle;}`, stored via `mtx1548` into
  `renderctx[0x6b8]`.
* `0x4010461d` — `{u32 mask; u32 head; u32 enable; u32 arg;}`. `head` 0/1/2 →
  IRQ-manager slot `+0`/`+0x78`/`+0x168`; head 2 is additionally gated on
  register `0x1dc4 & 8`. `mask` must be `2`, `0x10000000` or `0x20000000`.

```c
/* 0xc018461b — OsPostBusmasteringBuffer (mtx_drv.so @0x5c9f0) */
struct { u32 type;   /* 0 = primary (2D/3D), 2 = video          */
         u32 start;  /* command-buffer start                    */
         u32 end;    /* end | mode bits (low 2 bits, see §6.1)  */
         u32 v0, v1, v2; /* video variant only (@0x5ca30)       */ };  /* 24 */
/* userspace early-out: if (start == end) return success without the ioctl */
```

### 3.3 ICD link create, `0xc038461f` → `mtx1775` **[CONF]**

56-byte in/out struct. Allocates a `0x150`-byte link object
(`mtx1942(link, obj, OsGetCurrentProcessId())`), stores it at
`hcorectx->[0x30]`, and returns the mmap cookie of the control block.

```
+0x00 u64 IN  device/PDEV handle
+0x08 u64 OUT mmap cookie of the ICD control block  (= link->[0xf8])
+0x10 u8      caps0        (device vtable *0x20)
+0x11 u8      1
+0x12 u8      caps1
+0x13 u8      caps2
+0x14 u32     guardband min x   0 or 0xc5ff0000 (-8160.0f)
+0x18 u32     guardband min y   0 or 0xc5ff0000
+0x1c u32     guardband max x   0 or 0x45ff0000 (+8160.0f)
+0x20 u32     guardband max y   0 or 0x45ff0000
+0x24 u32     8              [INF] max texture units
+0x28 u32     0xffff         [INF] max primitive count
+0x2c u8      0
+0x2d u8      0
```

`mtx1942` @0x4dcb0 allocates three buffers, **one page each** (`nPages = 1`,
argument 3 — see the note in §2.2): **[CONF]**

| link field (kaddr / base) | wrapper | type | use |
|---|---|---|---|
| +0xf0 / **+0xf8** | `mtx3185` | SYS | the ICD control block, returned above |
| +0x110 / +0x120 | `mtx586` | PCI_LOCKED | DMA-visible page |
| +0x118 / +0x128 | `mtx586` | PCI_LOCKED | DMA-visible page |

---


### 3.4 Where the ICD's device handle comes from **[BIN]** (2026-09-07)

`ICD_CREATE` (`0xc038461f`) takes the device/PDEV handle at `+0x00`, and a
client cannot synthesise it. Traced through `libGL.so.1.3.0`:

```
init(obj, const char *devpath, u64 handle)   @0x3590d0
    rbp = rdx                         ; 3rd argument = the handle
    obj[0] = open(devpath, O_RDWR)
    ioctl(fd, GET_MEMORY_INFO, &st)   ; obj[0x90]=fb base, obj[0x94]=fb size
    rsi = rbp
    call @0x358c30                    ; the ICD_CREATE wrapper
@0x358c30:
    struct[0x00] = rsi                ; the handle, passed straight through
    ioctl(fd, 0xc038461f, struct)
    cb = mmap(fd, struct[0x08], 0x238)   ; NOTE 0x238 bytes, not a page
    memset(cb, 0, 0x238)
    obj[0x08] = cb
```

So the handle enters libGL as its **third argument** - it comes from the
GLX/DRI layer, which got it from the X server. The X server created the PDEV
with the **root** ioctl `ParhlEnableDevice` (`0xc0804614`, RW 128).

**Consequence for a from-scratch driver:** running as root we can create our own
render context with `0xc0804614` rather than borrowing X's, then feed that
handle to `ICD_CREATE`. That is the concrete route to submitting without X -
and it is the same gap as "engine/context init" (task #18).

Corrections to §3.3 and §5.2: the control block is mapped **0x238 bytes**, and
libGL memsets it to zero immediately after mapping.

## 4. Register access

### 4.1 There is no inline MMIO **[CONF]**

Every access goes through `memWriteDword(MEMHANDLE, offset, val)` /
`memReadDword(MEMHANDLE, offset, u32 *out)` (`src/mtxvxd/MtxMem.c`), with the
handle from `CommonData->[0x2030]`.

### 4.2 `0x384` = DMAPAD, a write-combining erratum workaround **[CONF]**

Every one of the ~200 call sites is preceded by this inlined guard on a shadow
object `pRegShadow = CommonData->[0x10]` (which lives in the *Shared locked*
buffer, i.e. it is visible to userspace):

```c
if (pRegShadow->[0x84]) {                       /* workaround enabled */
    last = pRegShadow->[0x88];
    if (last < TARGET && last + 0x80 >= TARGET)
        memWriteDword(h, 0x384, 0);             /* DMAPAD dummy write */
    pRegShadow->[0x88] = TARGET;
}
memWriteDword(h, TARGET, value);
```

A *read* sets `[0x88] = 0xffffffff`, suppressing the pad for the next write.
The rule is therefore: **if the previous register write was within 128 bytes
*below* the new offset, insert a dummy write to `0x384` first.** 60 distinct
`0x384` sites; this fully accounts for the `W 0x0384 0x0` lines in
`captures/mmio-draw-0528.txt`.

> **Correction to earlier notes.** `0x384` was previously logged as a
> "soft-trap". It is not. The fence/soft-trap register is `0x12BC` (§7).

`pRegShadow` layout in use: **[CONF]**

| off | contents |
|---|---|
| +0x78 | busy/lock flag, spun on before a ring restart |
| +0x84 | DMAPAD workaround enabled |
| +0x88 | last register offset written (`0xffffffff` after a read) |
| +0x8c | shadow of register `0x1300` |
| +0x90 | indexed-FIFO write count (§6.3) |
| +0x98 | shadow of register `0x8fc` |
| +0x9c | expected `0x450` value |
| +0xe0 | AGP capabilities |

### 4.3 Register map touched by the core **[CONF]**

| reg | R/W | meaning |
|---|---|---|
| `0x300` | RW | AGP/bus command mirror |
| `0x308` | W | bus-master enable (`0x83` on 0x0528, else `0x01`) |
| `0x384` | W | **DMAPAD** (WC workaround) |
| `0x400` | W | primary-DMA limit / third pointer — meaning **[OPEN]** |
| `0x404` | W | **PRIMADDRESS** — ring base, written `& ~3` |
| `0x408` | W | **PRIMEND** — ring tail, the doorbell |
| `0x41C` | R | **fence completion readback** |
| `0x450` | R | bit 22 = indexed-write-FIFO empty; also compared against `pRegShadow->[0x9c]` |
| `0x8f0` | R | status: bit 17 = primary-DMA finished; bits 24–25 = WARP busy |
| `0x8f4` | R | **IRQ status** |
| `0x8f8` | W | **IRQ acknowledge** (write the status bits back) |
| `0x8fc` | RW | **IRQ enable mask** |
| `0x908` | R | WARP idle signature, `(v & 0xfff7ffbf) == 0x63a78001` |
| `0x910`/`0x914`/`0x938` | W/R/W | unidentified **[OPEN]** |
| `0x918` | W | **WARP ucode load address** (instruction index) |
| `0x91c` | W | **WARP ucode data FIFO** |
| `0x920` | W | WARP/engine control, bit 0 = enable, bit 31 forced 0 |
| `0x924` | R | WARP pipe status, ready when `(v & 0xfff) == 0x100` |
| `0x318`/`0x31c` | R | DMA channel status, pre-submit gate |
| `0xbc0` | W | memory-config, value from a capability vtable |
| `0x10b0` | W | `0x10` — bus-master arm |
| `0x12b8` | W | `0x00070601` — bus-master configuration |
| `0x12BC` | W | **SOFTRAP / fence tag write** |
| `0x1300` | RW | engine mode, software-shadowed |
| `0x1800`/`0x19a8`/`0x1a00`/`0x1ba8` | RW | display controller heads |
| `0x1dc4` | R | bit 3 = second-head IRQ available |

---

## 5. The GL submission path

### 5.1 The doorbell — `ioctl(fd, _IO('F',33) = 0x4621, 0)` → `mtx903` **[CONF]**

No ioctl argument. There is **no kernel thread, no timer, and no interrupt** in
the submission path; everything happens synchronously inside this call.

```c
link = hcorectx->[0x30];        if (!link) return -EINVAL;
CB   = link->[0xf0];            if (!CB)   return -EINVAL;   /* control block */
if (link->[0x14c] && CB[0x00] != 3)   { CB[0x234] = -3; return 0; }
if (!link->[0xd8]->[0x6b8])           { CB[0x234] = -3; return 0; }

lock = &hcorectx->[0x10]->[0x20]->[0x7c];
while (OsInterlockedExchange(lock, 1)) OsSchedule();   /* spin with yield */

op = CB[0x00];
switch (op) { ... }             /* jump table .rodata+0x00, ops 0x0c..0x16 */
*lock = 0;
```

Jump table (`.rodata+0x00`, 11 entries, index `op - 0x0c`): ops **12, 14, 16,
17, 20, 22** have dedicated handlers (`.text` 0x1ffc, 0x1f57, 0x1f20, 0x1e0c,
0x1dcc, 0x1d87); 13, 15, 18, 19, 21 fall through to the generic dispatcher
`mtx3683`. `libGL` validates op ids client-side against the mask `0xf07ffe78`
(ops 3–6, 9–22, 28–31) before issuing the ioctl. **[CONF]**

### 5.2 Control block — the 4096-byte SYS mapping **[CONF]**

One page (§3.3). `libGL` `memcpy`s 0x220 bytes in, issues the ioctl, and
`memcpy`s them back out.

```
+0x000 u32   opcode  (1 = execute command list; 3..0x1f = control ops)
+0x008 u32   target/surface index for op 1, bounds-checked vs link->[0x20]
+0x010 [0x220 bytes]  in/out payload; for op 1 the first dword is the
                      byte offset of the command list within link->[0x28]
+0x230 u32   command-list length in bytes
+0x234 s32   result status written by the kernel (-1 invalid op, -3 not ready)
```

### 5.3 Generic dispatcher `mtx3683` @0x4de50 **[CONF]**

```c
if (op == 1) {                                      /* RENDER */
    idx = CB[0x08];
    tgt = (idx < link->[0x20]) ? link->[0x10][idx] : NULL;
    mtx558(tgt, link->[0xd8], CB);
    return 1;
}
if (op - 3 > 0x1c) { CB[0x234] = -1; return 0; }
CB[0x234] = <table .rodata+0x1fe8>[op - 3](link);   /* 29 entries, ops 3..31 */
```

Ops 7, 8, 19 and 23–27 are invalid — exactly complementing `libGL`'s
`0xf07ffe78` mask.

### 5.4 Command-list interpreter `mtx558` @0x44ef0 **[CONF]**

```c
if (CB[0x10] + CB[0x230] > link->[0x28]->[0x10]) return;    /* bounds check */
p = link->[0x28]->[0x00] + CB[0x10];
CB[0x234] = -1;
while (p < base + CB[0x10] + CB[0x230]) {
    op = *(u32 *)p; payload = p + 8;
    if (op > 0x51) break;
    switch (op) { <jump table .rodata+0x1b08, 82 entries> }
}
```

So a command is `u32 opcode; u32 pad; <payload>`, opcodes **0x00–0x51**.
`mtx558` sets `pCmdBuf->[0x88] = 2` (the DMA mode bits later OR'd into
SECADDRESS) and calls the ring-append twice — once for `pDev->[0x2170]` and
once for `ctx->[0x68]`.

### 5.5 The submit-request buffer — the 139264-byte mapping **[HW]**

Established from `captures/glsweep-0528-raw.txt` (`tools/parhelia-glsweep.c`),
which snapshots every mapping around each GL call. Per draw, **only offsets
0x008…0x028 change; nothing else in the region moves** — in particular nothing
at 0x230/0x234.

```
tri_flat     008=00000004 010=009b8004 014=ffffc200 018=00002000 020=0000201d 028=00002040
tri_gouraud                                         018=00004000 020=0000401d 028=00004040
tri_second                                          018=00006000 020=0000601d 028=00006040
lines        008=00000001                           018=0000a000 020=0000a01d 028=0000a040
```

The changed window is exactly 0x28 bytes, matching the ICD-side finding that
`libGL` builds **0x28-byte submit records** and posts them with request opcode
`0x1b` via `reserve(reqbuf, opcode, elemsize=0x28, count)` / `commit`
(reported by the ICD analysis, not re-verified here). Successive draws rewrite
the *same* offsets rather than advancing, so this is a **single-slot request
buffer reused per flush** (count = 1), not an array. Aligning the record to
start at region offset 0x008:

| record off | region off | observed | meaning |
|---|---|---|---|
| +0x00 | 0x008 | 4 / 1 / 0 / 5 / 6 | primitive code; written **only when the primitive changes** |
| +0x08 | 0x010,0x014 | `0xffffc200_009b8004` | u64 kernel context handle (a vmalloc pointer, §2.3) |
| +0x10 | 0x018 | 0x2000 / 0x4000 / 0x6000 / 0xa000 | **START** — 0x2000 slot stride |
| +0x18 | 0x020 | 0x201d / 0x401d / 0x601d | **END \| 1** (END = START + 0x1c) |
| +0x20 | 0x028 | 0x2040 / 0x4040 / 0x6040 | START + 0x40 — vertex-data offset in the slot |

Two internal corroborations: `END - START = 0x1c` = the 7-dword descriptor
already documented in `docs/parhelia-gpu-access-map.md` §5; and START+0x40 is
exactly where the same document records vertex data landing. **[HW]**

The primitive codes are the GL enums (`GL_POINTS`=0, `GL_LINES`=1,
`GL_TRIANGLES`=4, `GL_TRIANGLE_STRIP`=5, `GL_TRIANGLE_FAN`=6). **[HW]**

> **Superseded readings.** This region is *not* the command-list buffer of §5.4
> (nothing at `CB[0x230]` ever changes) and *not* the one-page control block of
> §5.2 (writes occupy a 0x28-byte window, and `libGL` maps 0x238 bytes at the
> control-block cookie, which is a different 4096-byte mapping). Both readings
> appeared in earlier drafts of this work; both are wrong.

> **Unresolved conflict.** Record +0x00 is read here as the primitive code —
> the capture shows exactly the five GL enums, changing only with the
> primitive. A separate ICD-side analysis reads a type field
> `ctx[0xa050] = 0xd`. These are most likely *different fields*: `0x1b` is the
> **request opcode** passed to `reserve()`, which belongs to the request-buffer
> header, not to the record; and `ctx[0xa050]` is an ICD context field. The
> capture evidence for "record +0x00 = primitive code" is direct; the
> reconciliation is **[INF]**.

---

## 6. The DMA ring

### 6.1 Ring append + PRIMEND — the submit **[CONF]**

Two entry points, structurally identical, differing only in caller:

* **`mtx3257` @0x32f20** — reached from `ioctl 0xc018461b` via `mtx391`.
* **`mtx1937` @0x32360** — reached from the command-list interpreter `mtx558`
  and from the ucode loader `mtx1391`.

> These were disassembled independently by two analyses, one each. Both emit
> the identical 5-dword packet and the identical `0x408` write. They are
> believed to be the 3D and 2D/blit variants of one routine, but **whether they
> differ in any other respect is [OPEN]** — neither was diffed against the
> other instruction by instruction.

`mtx1937(pCmdBuf, u32 start, u32 end, u32 startFlags, u32 endFlags)`:

```c
if (start >= end) return;                       /* nothing queued */
if (endFlags == 0) {                            /* blocking/non-DMA post */
    q    = pDev->[0x2440];
    mask = (q & 0x7e000) ? 1 : 0x57;
    want = (q & 0x7e000) ? 1 : 0x50;
    do { reg = (pDev->[0x2440] & 0x70000) ? 0x31c : 0x318;
    } while ((memRead(reg) & mask) != want);
}
r = pCmdBuf->[0x80];                            /* the primary ring object */
if (r->[0x60] + 0x14 > r->[0x70]) r->vtbl[0x20](r);   /* wrap: SwitchToNewBuffer */
r->[0x60] += 0x14;
p = (u32 *)r->[0x60];
p[-5] = 0x20292820;                             /* packet header  */
p[-4] = 0;
p[-3] = start | startFlags;
p[-2] = end   | endFlags;
p[-1] = 0;
if (r->[0x98]) { mtx3144(r); return; }          /* fence path, §7 */
memWriteDword(h, 0x408, r->[0x9c] + (u32)r->[0x60]);   /* PRIMEND = bus addr */
```

Ring object fields: `[0x60]` kernel-VA write pointer, `[0x68]` last-submitted
mark, `[0x70]` limit, `[0x9c]` **bus-address − kernel-VA delta**. So
`0x408` receives a *bus address*, not an offset. **[CONF]**

`startFlags` comes from `pCmdBuf->[0x88]` (set to 2 by `mtx558`); `endFlags`
from the buffer descriptor's `[0x34]` (address-space flags). This is where the
low mode bits in `end|3` originate. **[CONF]**

### 6.2 Packet format — decoded exactly from `mtx2434` @0x398e0 **[CONF]**

`mtx2434` is a **software interpreter of the DMA stream** (used when
bus-mastering is off, and for validation). It is the authoritative spec:

```c
while (p != end) {
    if (pDev->[0x2020] == 0 || 0x1000 + 4*(pDev->[0x2024] & 0xff) == 0x1084) {
        if (pDev->[0x10ac]) {                       /* repeat-header active */
            pDev->[0x2024] = pDev->[0x10a8]; pDev->[0x10ac]--;
        } else {
            pDev->[0x2024] = *p++;                  /* fetch a new header   */
        }
        pDev->[0x2020] = 4;  continue;
    }
    idx = pDev->[0x2024] & 0xff;
    reg = 0x1000 + idx*4;
    val = *p++;
    pDev->[0x2024] >>= 8;  pDev->[0x2020]--;
    pDev->[0x1020 + idx*4] = val;                   /* 256-dword reg shadow */
    switch (reg) {
      case 0x1080: continue;                        /* DMAPAD  — discard    */
      case 0x108c: pDev->[0x2020] = 0; continue;    /* force new header     */
      case 0x109c: continue;                        /* shadow only          */
      case 0x10a0: case 0x10a4: continue;           /* SECADDRESS / SECEND  */
      case 0x12bc: <fence, §7>; continue;
      case 0x1300: if (((val & 0xf) - 6) > 1) pRegShadow->[0x8c] = val; break;
    }
    <FIFO check, §6.3>
    memWriteDword(h, reg, val);
}
```

Hard facts this pins down:

* A packet is **one header dword + up to four data dwords (0x14 bytes)**.
* The header's four bytes are register indices consumed **LSB first**.
* **register = `0x1000 + index*4`** (index 0x00–0xFF ↔ 0x1000–0x13FC).
* **index 0x20 (`0x1080`) = DMAPAD** — consumes a data dword, discarded.
* **index 0x21 (`0x1084`) = terminate this packet; the next dword is a new
  header.** This is why emitted blocks are 6 or 9 dwords rather than multiples
  of 5 — packets may be short, and an emitter may leave the stream mid-packet
  and let the next emitter supply the remaining data dwords.
* **index 0x28 (`0x10A0`) = SECADDRESS, index 0x29 (`0x10A4`) = SECEND.**
* **index 0x2a/0x2b (`0x10A8`/`0x10AC`) = header-repeat register + repeat
  count** — a stream-compression scheme: set a header and a count, and the same
  header is reused for N packets without re-fetching it.
* index 0x23 (`0x108C`) forces a new header; index 0x27 (`0x109C`) is shadow-only.

So the ring packet `0x20292820` decodes as
`DMAPAD=0; SECADDRESS=start|flags; SECEND=end|flags; DMAPAD=0` — i.e. **this is
a classic MGA two-level DMA**: the kernel-owned primary ring "calls" the
userspace-built secondary command buffer. **[CONF]**

> This supersedes the earlier note in `docs/parhelia-gpu-access-map.md` that
> `0x408 = end|3` submits the client buffer directly. `0x408` advances the
> *primary ring* tail; the client buffer address travels inside the packet.

### 6.3 The 16-deep indexed write FIFO **[CONF]**

Registers 0x1000–0x13FF go through a 16-entry write FIFO. The driver counts
writes in `pRegShadow->[0x90]` and, on reaching 16, polls **`0x450` bit 22**
until the FIFO drains (`mtx3863` @0x34e04, `mtx3948`, `mtx2434`).

### 6.4 Ring restart / rewind — `mtx2508` @0x31880 **[CONF]**

```c
while (pRegShadow->[0x78] != 0) ;                 /* spin on the shared lock */
poll 0x8f0 until bit 17 (0x00020000) set;         /* primary DMA finished    */
poll 0x450 until == pRegShadow->[0x9c];  pRegShadow->[0x90] = 0;
addr = (pBuf->[0x58] + pRing->[0x60]) | pBuf->[0x34];
pStatus->[0x74] = addr;
memWriteDword(0x404, addr & ~3);                  /* PRIMADDRESS             */
v = pStatus->[0x40];
if (pCaps->vtbl[0x20]() & 4) v &= ~0x1f;
memWriteDword(0x400, v);
```

`0x400` is the third register of the primary-DMA triple; the value is a limit
or wrap pointer, optionally 32-byte aligned. Its precise semantics are
**[OPEN]** — the binary only ever writes it here.

---

## 7. Fence and completion

The real fence pair is **`0x12BC` (write tag) / `0x41C` (read completion)**.
`0x450` is a status word, not the fence. **[CONF]**

From `mtx2434`, on meeting index `0xaf` (register `0x12BC`) in the stream:

```c
tag = val & ~1;
<poll 0x450 until bit 22 set — indexed write FIFO empty>
memWriteDword(h, 0x12bc, tag);              /* SOFTRAP: emit the tag        */
if (val & 1)
    do { memReadDword(h, 0x41c, &done); } while (done != tag);  /* wait      */
```

**Tag allocation rule** (`mtx3144` @0x31c30): tags come from `pStatus->[0x44]`,
incremented by **8** each time, **skipping 0**; **bit 0 = "block until
complete"**. **[CONF]**

`mtx3144` also shows the commit block — 0x24 bytes = 9 dwords:

```
[0]=0x21c0c0c0  [1]=0x0307  [2]=0x00000004  [3]=0x0707
[4]=0xc0afafdd  [5]=0x00000000  [6]=fence1|1  [7]=fence2|1  [8]=shadow(0x1300)
```

Decoded with §6.2. Header `0x21c0c0c0` yields indices `c0, c0, c0, 21`
(LSB first), so `0x1300←0x0307`, `0x1300←4`, `0x1300←0x0707`, then index
**0x21 terminates the packet**. The next dword `0xc0afafdd` is a new header,
indices `dd, af, af, c0` → `0x1374←0`, `0x12BC←fence1|1`, `0x12BC←fence2|1`,
`0x1300←saved`. Optional padding to a
0x1000 boundary uses `0x21212121`. Then an `OsInterlockedExchange` lock on
`pStatus->[0x70]`, `OsInterlockedIncrement(pStatus->[0x78])`, and
`0x408 = pStatus->[0x74]`. **[CONF]**

### 7.1 How completion reaches userspace without an ioctl **[CONF]**

`pStatus` lives in the **Shared locked** buffer — `MTX_MEMTYPE_PCI_LOCKED`,
scope `ROOT`, 2784 bytes — which is mapped into the client. So the ICD polls
memory, never the kernel:

| off | contents |
|---|---|
| +0x40 | ring limit value written to `0x400` |
| +0x44 | fence tag allocator |
| +0x70 | `OsInterlockedExchange` lock |
| +0x74 | last PRIMEND value written |
| +0x78 | `OsInterlockedIncrement`/decrement pending-submit counter |

This is the whole synchronisation story: **no completion ioctl exists.**

---

## 8. Interrupts

### 8.1 Wiring **[CONF]**

`src/mtx_irq.c` is open source: `request_irq(..., IRQF_SHARED, ...)` →
`mtx_irq_interrupt` → `dev->core->irq_fast(core_data)`. Return `< 0` →
`IRQ_NONE`; `> 0` → `tasklet_schedule(&dev->irq_bh)` whose `func` is
`core->irq_slow`.

### 8.2 ISR core `mtx1415` @0x3adb0 **[CONF]**

```c
memReadDword(h, 0x8f4, &st);                 /* IRQ status                   */
if (!st) return NULL;
memWriteDword(h, 0x8f8, st);                 /* ACK: write the bits back     */
for (i = 0; i <= 4; i++) {                   /* 5 records, stride 120 (0x78) */
    rec = irqmgr + i*120;
    if ((st & rec->[0x68]) && rec->[0x70]) return rec;   /* mask && enabled  */
}
return NULL;
```

`irqmgr + 5*120 = irqmgr + 0x258` is the device back-pointer, so there are
exactly **5 slots**; `OsSetVSyncInterruptState` only ever enables 0, 1 and 3.

### 8.3 `mtx987` (irq_fast) / `mtx2999` (irq_slow) **[CONF]**

```c
/* top half */
irq = hcoredev->[0x08]->[0x2050];            /* = CommonData->[0x2050]       */
rec = mtx1415(irq, &iter);  hcoredev->[0x48] = 0;
while (rec) {
    if (!mtx4025(rec))                       /* not fully handled up here    */
        hcoredev->[0x28 + 8*hcoredev->[0x48]++] = rec;   /* 4 deferred slots */
    rec = mtx682(irq, &iter);
}
if (hcoredev->[0x48]) { mtx3091(irq); return 1; }   /* mask via 0x8fc, run bh */
return 0;

/* bottom half */
for (i = 0; i < count; i++) mtx3330(hcoredev->[0x28 + 8*i]);
count = 0;  mtx2070(irq);                    /* re-enable via 0x8fc          */
```

`CommonData->[0x2050]` is the interrupt manager, and `CommonData` is
`aoCommonBufferTable[0]` — the vmalloc'd Common data buffer.

---

## 9. Device init

### 9.1 First open **[CONF]** (`src/mtx_fops.c:mtx_open` → `mtx_dev_init`)

1. Core selected by PCI device id: 0x0527, 0x0528, 0x2537, 0x2538, 0x2539.
   **0x0529 (Phoenix) is not supported by this driver.**
2. `ClientIoRemap` **BAR1** (BAR2 if BAR0 is a 64-bit BAR) as the register
   region — type `IO`, scope `AUTH`.
   **BAR0 (framebuffer) is never ioremapped**, only MTRR write-combined.
3. `mtx_dev_do_tests()` — sets `bm_flags` (`src/mtx_agp_flags.h`), forces
   `PCI_COMMAND_MASTER`, clears `ENHMEMACC` on i845/i850/i830/i860.
4. `dev->core->enable(...)` = `mtx2007`.
5. `mtx_irq_init(dev)`.

### 9.2 `mtx2007` (enable) @0xea0 **[CONF]**

`new 0x90` core-device (`[0x00]` = hdev, `[0x28..0x40]` = 4 deferred-IRQ slots,
`[0x48]` = count, `[0x50]` = context list); `new 0x38` default context →
`[0x88]`; `LocMemSet(&cd[0x8], 0, 0x20)`; `mtx728(cd)`; `mtx3376(cd)`.

`mtx728` @0x2610 allocates the two ROOT-scope shared regions
(`MtxRegion_Alloc(hctx, 0x18, SYS, ROOT, 0)` and
`MtxRegion_Alloc(hctx, 0xae0, PCI_LOCKED, ROOT, 0)`), then calls `mtx3229` with
a parameter block: `+0x00` locked VA, `+0x10` locked phys, `+0x18` locked size,
`+0x20` regs phys, `+0x28` regs size, `+0x30` FB phys, `+0x38` FB size,
`+0x3c` bm_flags, `+0x40` SMP flag, `+0x50/0x58/0x60` shared VA/phys/size,
`+0x68` host PCI handle, `+0x70` device PCI handle.

### 9.3 `mtx3376` @0xa80 — the bus-master arm **[CONF]**

Reproduces `captures/mmio-draw-0528.txt` exactly:

```c
h = MtxDevice_GetRegisterHandle(coredev);
pInfo = coredev->[0x18];              /* the 24-byte shared info buffer      */
devid = pInfo->[0x08]; hostid = pInfo->[0x10]; agp = pInfo->[0x14];

memWriteDword(h, 0x10b0, 0x10);

v = 0; v.b0 |= 0x01; v.b2 |= 0x07; v.b1 = (v.b1 & ~1) | 6;
v &= 0xc00fffff;                                       /* == 0x00070601      */
memWriteDword(h, 0x12b8, v);

a  = memRead(0x300) & 0xe0e0ff3f;
a |= (agp & 0xff000000) | ((agp << 16) & 0x00100000) | ((agp & 7) << 16);
a |= 0x0100;
if (devid == 0x0527102b && hostid == 0x03051106) a |= 0x40;      /* VIA quirk   */
if (devid != 0x0527102b)                         a |= 0x080000;
if (hostid == 0x25008086 || hostid == 0x25018086) a |= 0x80;     /* Intel quirk */
memWriteDword(h, 0x300, a);                            /* traced: 0x1f0b0140 */

memWriteDword(h, 0x308, (devid == 0x0528102b) ? 0x83 : 0x01);
```

`0x308 = 0x83` implies the captured board reports PCI id `102b:0528`, which
matches. `0x2537102b` is additionally special-cased on `pInfo->[0x0c] <= 1`.

> **Disagreement, recorded.** The two independent readings of the two quirk
> constants differ: one read `a |= 0x0800` (not `0x080000`) for the non-0x0527
> case and `a |= 0x8000` (not `0x80`) for the Intel case. The values above are
> the ones that follow from the byte-granular writes actually in the
> instruction stream (`or BYTE PTR [rsp+0xa], 0x08` → bits 16–23, and
> `or BYTE PTR [rsp+0x8], 0x80` → bits 0–7), and are believed correct — but
> this has **not** been checked on hardware, and the alternative differs only
> in which byte lane is set. Verify before relying on either.

### 9.4 WARP microcode load — `mtx1391` @0x9f6b0 **[CONF]**

`mtx1391(this, pDesc, bool bReset, int iEntryIdx)`:

```c
if (bReset) { this->[0xa3118] = 0; this->[0xa3121] = 0; }
n = pDesc->[0x18];                              /* BYTE count                */
if (this->[0xa3118] + n > 0x17ff) { this->[0xa3121] = 1; return 0; }
mtx1937(...);                                   /* flush the ring first      */

/* full engine-idle wait */
poll 0x8f0 until bit 17 set;
poll 0x450 until == pRegShadow->[0x9c];
poll 0x908 until (v & 0xfff7ffbf) == 0x63a78001;
poll 0x924 until (v & 0xfff) == 0x100;
poll 0x8f0 until (v & 0x03000000) == 0;

memWriteDword(h, 0x918, this->[0xa3118] / 12);  /* load address, unit = 12 B */
src = pDesc->[0x20];
for (i = 0; i < n; i += 12) {                   /* 3 dwords per instruction  */
    memWriteDword(h, 0x91c, *src++);
    memWriteDword(h, 0x91c, *src++);
    memWriteDword(h, 0x91c, *src++);
}
this->[0xa3118] += n;
return (old / 3) + 4 * pDesc->[0x2c + 4*iEntryIdx];   /* entry-point address */
```

* **A WARP instruction is 12 bytes = 3 dwords.**
* **Ucode RAM is `0x1800` bytes = 512 instructions**; overflow sets a flag and
  refuses the load.
* `0x918` counts *instructions*, i.e. byte offset / 12. (The divide is
  `imul 0xAAAAAAAB; shr 32; shr 3` = `>> 35`; reading it as `/3` then `/8`
  separately gives the same answer but obscures the unit.)

Caller `mtx676` @0xa0230 builds the descriptor on the stack:
`+0x18 = 0x12cc`, `+0x20 = &mtx2785`, `+0x28 = 4`, then
`mtx1391(this, desc, bReset = 1, idx = 8)`. **[CONF]**

`mtx656` @0xa0e40 then enables the engine:

```c
v = mtx3806(this);
v &= ~(1u << 31);
v.bit0 = this->[0x2190]->[0xa3114] ? 0 : 1;
memWriteDword(h, 0x920, v);                     /* the traced 0x920 = 1      */
```

### 9.5 There is only one microcode image **[CONF]**

`mtx2316` @0x9fc70 selects between two loaders on `this->[0xa3114]`:

| `[0xa3114]` | loader | what it does |
|---|---|---|
| 0 | `mtx1391` | streams the ucode to the `0x91c` hardware FIFO |
| ≠0 | `mtx1650` @0x9fc40 → `mtx1397` @0x9f5b0 | `LocMemCpy` into a buffer — **software WARP emulation** |

The same `[0xa3114]` clears bit 0 of `0x920` (§9.4), consistent with "no
hardware WARP". So this is **one image with two consumers**, not two images.
`0x12cc` appears exactly once in the object, at `mtx676`'s descriptor. **[CONF]**

**Corrected claim.** An earlier analysis reported that `mtx2785` in this 2009
CVS object is byte-*different* from `tools/warp_ucode.h`, implying two
microcode revisions of equal length. **That is false.** It came from computing
the file offset as `sh_offset + (st_value - sh_addr)`. In an `ET_REL` object
`st_value` is *already* section-relative (`readelf -s` shows `0x84ec0`; `nm`
prints `0x99aa0` because it adds `sh_addr`), so the correct offset is
`sh_offset + st_value`. The wrong formula lands `sh_addr` = 0x14be0 bytes
early — at `0x14d0e0` instead of `0x161cc0` — and reads 4812 bytes of unrelated
`.data` that look plausible.

Verified:

```
symbol      mtx2785 (.data, section index 14)
st_value    0x84ec0 (section-relative)
size        4812 bytes = 1203 dwords = 401 instructions x 12 bytes
file offset 0x161cc0 = 1449152
md5         b29644ecfcfcbbc11ef97240872892ca
```

Extracting at `0x161cc0` reproduces `tools/warp_ucode.h` **dword-for-dword**.
`tools/warp_ucode.h`'s own header already records the same location
(".data @0x99aa0", the `nm`-style address). Use
`tools/parhelia-extract-ucode.c`, which resolves the symbol through the ELF
section headers and handles the `ET_REL` case:

```
gcc -O2 -o parhelia-extract-ucode tools/parhelia-extract-ucode.c
./parhelia-extract-ucode .../kernel/mtx_parhelia.o -c -o tools/warp_ucode.h
```

**As of this writing exactly one WARP microcode image is known**, 4812 bytes,
md5 `b29644ec…`, present in the 2009 CVS `mtx_parhelia.o` and reported
byte-identical between the 0x0528 and P690 kernel modules on the target
machines. Do not assume a second revision exists without a fresh hash.

401 instructions × 12 bytes independently corroborates the 12-byte instruction
size derived from the load loop — relevant to `docs/parhelia-vertex-isa.md`.

---

## 10. Verified negatives

These redirect real work, so they are stated explicitly. Both were established
by enumerating **every** literal register offset in `mtx_parhelia.o` and
resolving all 34 computed-offset call sites (which reduce to `0x1000 + i*4`,
`0x1800 + h*0x200`, `0x1824/0x19c8/0x1a24/0x1bc8 + i*4`, `0x1924 + h*0x200`,
and `idx*16`). **[CONF]**

* **The kernel module never writes `0x314`.**
* **The kernel module never touches registers `0x0000`–`0x0130`** — the
  base/limit aperture bank.

Both appear in `captures/mmio-draw-0528.txt`, so both must come from
**userspace writing BAR1 directly**, which it can: `GET_MEMORY_INFO` exports
`reg_base`/`reg_size` and the register region is scope `AUTH`. For a
from-scratch driver, the aperture bank and the `0x314` kick belong to the X
driver (`mtx_drv.so`), not to the kernel. `docs/parhelia-gpu-access-map.md`
§1's "DMA descriptor / pull-aperture setup" should be re-captured against
`mtx_drv.so`, not looked for in the module.

---

## 11. Merged from-scratch recipe

1. **Arm the bus master** — `0x10b0 = 0x10`; `0x12b8 = 0x00070601`;
   `0x300 = (read & 0xe0e0ff3f) | agpbits | 0x100 [| quirks]`;
   `0x308 = 0x83` on 0x0528 else `0x01`. (§9.3)
2. **Load the WARP ucode** — idle-wait (`0x8f0` bit 17 → `0x450` → `0x908` ==
   `0x63a78001` → `0x924 & 0xfff` == `0x100` → `0x8f0 & 0x03000000` == 0),
   `0x918 = byteOffset / 12`, stream 3 dwords per instruction to `0x91c`, then
   `0x920 |= 1`. (§9.4)
3. **Set up the ring** — one DMA-coherent buffer. `0x404 = ringBase & ~3` and
   `0x400 = limit` only on start/rewind. (§6.4)
4. **Submit** — append `{0x20292820, 0, start|mode, end|flags, 0}` to the ring,
   then `0x408 = ringBusBias + writePointer`. Insert `0x384 = 0` whenever the
   previous register offset was within 128 bytes below the new one. (§4.2, §6.1)
5. **Fence** — put `0x12BC = tag` in the stream (index 0xaf); poll `0x41C` for
   completion. Drain the 16-deep indexed FIFO with `0x450` bit 22. (§6.3, §7)
6. **Interrupts** — read `0x8f4`, write the same bits back to `0x8f8` to ack,
   mask/unmask via `0x8fc`. (§8)
7. **Not needed from the kernel** — `0x314` and `0x0000`–`0x0130`. (§10)

---

## 12. Open items

* `0x400` semantics in the primary-DMA triple (§6.4).
* `0x910` / `0x914` / `0x938` (`mtx514`).
* Whether `mtx3257` and `mtx1937` differ beyond their callers (§6.1).
* The `0x300` quirk-bit lane disagreement (§9.3) — needs a hardware check.
* The reconciliation of record +0x00 with the ICD-side `ctx[0xa050]` reading
  (§5.5).
* Where the 139264-byte submit-request buffer is allocated. It is created
  through the method-12 buffer-create path (`libGL` @0x357d20 → op 12 →
  `.text+0x1ffc`) with the size computed kernel-side from a 0x1c-byte
  descriptor; the size is not a literal in any of the three binaries.


## Appendix: the "texturing is broken" false alarm, and the real cause  [CONF]

Recorded in full because the wrong diagnosis was well-evidenced and cost real
time. Anyone hitting this symptom should not repeat it.

**Symptom.** The first `glTexImage2D` of any size blocked forever — reproduced at
4x4, 8x8, 16x16, 32x32 and 64x64, with both `GL_RGB` and `GL_RGBA`, before and
after a rendered frame. Meanwhile the kernel logged, on every GL start:

```
[mtx] *ERROR* Parhelia OS: Failed to allocate locked pages (nb: 801, type: 2)
[mtx] *ERROR* Failed to create device buffer (size: 3280896, type: 1)
```

**The wrong conclusion.** Those lines fit a hard vendor limit perfectly:
`MTX_MAX_ORDER` is 9 in `kernel/src/mtx_drv.h`, so `mtx_get_pg_order()` caps
allocations at 512 pages (2 MB) and returns -1 for the 801-page (3,280,896 byte)
request *before* `__get_free_pages` is reached — which is exactly why the
driver's own "Failed to allocate PCI memory" message never appears. The AGP
fallback genuinely cannot work on this box either (PCI board, nForce3 bridge,
`/proc/agp` absent, only intel/via/sis AGP modules present). It was concluded
that texturing was unusable here and could not be captured. **That was wrong.**

**Disproof.** `ioquake3` reaches `----- finished R_Init -----`, which is past
`R_InitImages`, so the vendor stack uploads textures on this box perfectly well.
The allocation failures are **non-fatal noise** — the driver falls back to
another memory type and continues.

**The real cause was in the test harness.** Backtrace of the hung process:

```
#0  ceilf () from /usr/lib64/libstdc++.so.6
#1..#4  ?? () from /usr/lib64/libGL.so.1
#5  glTexImage2D () from /usr/lib64/libGL.so.1
```

The harness was linked **without `-lm`**, so libGL's `ceilf` bound to the copy
exported by `libstdc++` instead of libm, and the driver's mip-level loop inside
`glTexImage2D` never terminated. Adding `-lm` fixes it at every texture size,
and the full 33-operation feature sweep then completes including all texture
ops. quake3 was never affected because it links libm.

**Rule: any capture harness built against this vendor libGL must link `-lm`.**
The `-Wl,--unresolved-symbols=ignore-in-shared-libs` workaround needed to link
against this libGL makes the failure easy to hit, because it lets the wrong
symbol bind silently.

**Still true and worth keeping:** the `MTX_MAX_ORDER` = 9 limit is real, the
801-page PCI_LOCKED request really does fail, and AGP really is unavailable on
this box — it simply is not fatal. If a workload ever does need that buffer,
rebuild the vendor module with `MTX_MAX_ORDER` raised to 10. Everything up to
`mtxvxd.o` builds; the final link needs a kernel source tree, and
`/lib/modules/2.6.30-dist/build` on the test box dangles into a T2 tree that no
longer exists.

## Appendix: the ioctl-to-GPU path, observed live  [HW]

A kernel oops during a texture-format sweep produced a complete call chain,
which **confirms on live hardware** the dispatcher structure recovered
statically in sections 4 and 6:

```
system_call_fastpath
  sys_ioctl -> do_vfs_ioctl -> vfs_ioctl
    mtx903+0x420        ioctl entry / doorbell
      mtx3683+0xc7      generic dispatcher
        mtx2359+0x20
          mtx2856+0x5d
            mtx263+0x155   <-- NULL dereference, CR2 = 0x14
```

So the userspace path really is: **one ioctl on `/dev/matrox/mtx0` enters
`mtx903`, which hands off to the generic dispatcher `mtx3683`, which routes to
the per-operation handler**, and only then does the driver touch the hardware.
Nothing in userspace reaches the GPU through this path directly — but note the
separate finding below that userspace ALSO maps BAR1 and writes it without the
kernel's involvement.

### What kernel-side MMIO a full GL session actually performs  [HW]

From an mmiotrace covering `modprobe mtx`, X start, a GL context, a texture
upload and a textured draw — 317 register writes in total:

| Register | writes | role |
|---|---|---|
| 0x408 | **216** | PRIMEND ring doorbell — the dominant traffic by far |
| 0x314 | 17 | kick |
| 0x040 | 11 | aperture/descriptor bank |
| 0x044 / 0x048 / 0x04c | 5 each | aperture/descriptor bank |
| 0x384 | 3 | DMAPAD |
| 0x920 | 2 | WARP arm |
| 0x300 / 0x308 | 2 each | engine arm |
| **0x91c** | **0** | **microcode FIFO — never written** |

**Two structural caveats, both important:**

1. **mmiotrace only sees the kernel's `ioremap` mapping.** Userspace maps BAR1
   itself (`MTX_IOCTL_GET_MEMORY_INFO` hands out `reg_base`/`reg_size`, region
   type `MTX_MEMTYPE_IO`), and those writes use different page tables, so they
   are **invisible** to this trace. Zero writes to 0x91c here therefore does
   **not** prove the microcode is never streamed — only that the *kernel* does
   not stream it. This weakens, but does not settle, the mtx676 question.
2. The trace's 384,680 `UNKNOWN` lines are accesses to BAR0 at 0xd0fc0000 with
   opcode `f3,48,ab` = `rep stosq` — bulk CPU writes of texture/vertex data into
   the aperture, which mmiotrace cannot decode into address/value pairs.

**Correction to a previously documented verified negative:** section 10 states
the kernel module never writes 0x314. The trace shows **17 kernel-side writes to
0x314**, so that negative is wrong as stated.

### A driver bug worth knowing: GL_INTENSITY oopses the kernel  [HW]

`glTexImage2D(..., GL_INTENSITY, ...)` reliably takes down the vendor kernel
module with a NULL dereference at `mtx263+0x155` on the path above. Captured
while sweeping internal formats. `GL_RGBA`, `GL_RGB`, `GL_ALPHA`,
`GL_LUMINANCE` and `GL_LUMINANCE_ALPHA` all complete normally, so the fault is
specific to that format rather than to the sweep. An open driver should treat
`GL_INTENSITY` as unsupported on this hardware until proven otherwise, and any
capture harness should avoid it — it kills the box's driver mid-run.

## Appendix: the userspace side, traced live  [HW]

Captured with `tools/parhelia-mtxtrace.c` (LD_PRELOAD) around a GL client that
uploads one texture and draws one textured triangle. Raw:
`captures/mtxtrace-textured-draw.txt`. This is the half mmiotrace cannot see.

### The full chain, end to end

```
open("/dev/matrox/mtx0")                      -> fd
ioctl(fd, 0x80304600)   nr=0   48 B  dir=r    -> GET_MEMORY_INFO
ioctl(fd, 0xc038461f)   nr=31  56 B  dir=rw   -> create buffer, returns an offset
mmap64(fd, offset, len)                       -> the buffer in userspace
... build command/vertex data by plain stores into those mappings ...
ioctl(fd, 0x4621)       nr=33   0 B  dir=none -> DOORBELL (x30 for this workload)
```

**`GET_MEMORY_INFO` output decodes exactly as documented** — verified against
`lspci`:

```
d0000000 00000000 10000000 ffffffff febfe000 00000000 00002000 00000002 ...
  fb_base  = 0xd0000000     fb_size  = 0x10000000   (256 MB)
  reg_base = 0xfebfe000     reg_size = 0x00002000   (8 KB)
```

**The buffer-create ioctl returns the mmap cookie.** `nr=31`'s output carries
`0x0c822000` at byte offset 8, and the very next `mmap64` uses exactly that as
its file offset. So the mmap "offset" is **not** a physical or GPU address — it
is a handle the kernel hands back from the create call. That is how a userspace
buffer becomes GPU-visible.

### The seven mappings, with their real offsets

| mmap offset | length | region role |
|---|---|---|
| 0x0c822000 | 0x1000 | control page (`+0x230` = submitted stream length) |
| 0x00e24000 | 0x1000 | control/status page |
| 0x0c8a9000 | 0x22000 (136 K) | command record stream |
| 0x18b11000 | 0x1000 | submission serial page |
| 0x0ccf0000 | 0x320000 (3 M) | vertex ring |
| 0x0d013000 | 0xa8000 (672 K) | command ring |
| 0x0d0d3000 | 0x400000 (4 M) | texture heap |

The lengths match the ICD's allocation constants and the regions identified in
docs/parhelia-command-construction.md, now with the device offsets that produce
them.

### Submission cost

One texture upload plus one textured triangle issued **30 doorbell ioctls**
(`nr=33`, `_IO('F',33)` = 0x4621, zero-sized — no payload, the work is already
in the shared buffers). This is the ioctl that reaches `mtx903` and thence the
ring doorbell writes seen kernel-side.

### Method notes that cost time

- **Nothing is visible unless direct rendering is active.** Without it the client
  makes no matrox `open`, no ioctl and no mapping at all — the X server does the
  work — and a capture silently comes out empty. Check
  `grep -c matrox /proc/<pid>/maps` (expect 7).
- **X takes a long time to initialise the mtx device.** `pgrep X` succeeding
  does not mean the device is up; the module refcount is still 0 and clients
  fall back to indirect rendering. Poll the X log for `Parhelia device started`
  (it appears twice) or wait for `lsmod` to show a refcount of 2 before
  capturing, rather than sleeping a fixed interval.
- The vendor stack calls `open64`/`mmap64`, not `open`/`mmap`; interposing only
  the latter logs nothing.


## The buffer-create ioctl (nr=31) — corrected, and how to replay it

**Only field +0x00 of the 56-byte request is an input.** It is a live kernel
pointer to the X server's PDEV object. Everything a capture shows at
+0x08..+0x30 is **uninitialised caller stack**, not fields: `ICDLinkImpl::Open`
(libGL `0x358c30`) allocates a 64-byte frame, writes only `[rsp]`, and calls.
The kernel reads the buffer exactly once, at offset 0 (`mtx1775` `0x2698`).

Output layout, written by the kernel:

    +0x08  mmap cookie for the 0x238-byte control block
    +0x10  caps0   +0x11  1 (literal)   +0x12 caps1   +0x13 caps2
    +0x14  guard band min, IEEE float -- 0xc5ff0000 = -8160.0f, or 0 if disabled
    +0x1c  guard band max --             0x45ff0000 = +8160.0f
    +0x24  8       multi-pass texture-stage threshold (NOT a unit count; 4 units)
    +0x28  0xffff  max vertices per submitted batch
    +0x2c  0       hardware line enable
    +0x2d  0       hardware wide-line enable

The last two are **hard-coded zero in the kernel**, which independently confirms
from a third direction that the wide-line software fallback is a driver policy
decision rather than a hardware limit.

### Where the handle comes from

The X server creates the PDEV via ioctl `0xc0804614` and the handle travels to
the client over the **GLX wire** (`X_GLXVendorPrivateWithReply`; libGL reads it
at reply+0x0c, `0x360d69`). `mtx_drv.so` never issues nr=31; `libGL` never
issues the create. So a GL client only borrows an address the server owns.

**It cannot be replayed from a capture** -- it is a raw kernel address, invalidated
by an X restart, a module reload or a reboot. Take it live from the current X
session.

### ⚠️ Passing an unverified handle can oops the machine

`mtx3645` (`.text+0x3430`) walks the PDEV list **without NULL-checking the head**.
A non-zero handle when no PDEV exists dereferences NULL in the ioctl path.
Passing zero is safe (early exit, clean EINVAL). **Never sweep this field.**

Other state rules: a second nr=31 on one fd returns `-EBUSY`; release with
`ioctl(fd, 0x4620)` or close. And the returned **cookie cannot be replayed
either** -- `mtx_mmap` validates it through `MtxRegion_FindFromBase` and, for
region type 1, requires the mapping fd's own context id.

## Vendor bug: allocation failure oopses the kernel [HW, observed]

Replaying with a live handle got as far as real buffer creation and then hit
this:

    [mtx] *ERROR* Failed to create device buffer (size: 3280896, type: 1)
    [mtx] *ERROR* Parhelia OS: Failed to allocate locked pages (nb: 801, type: 1)
    BUG: unable to handle kernel NULL pointer dereference at 0000000000000004
    IP: [<...>] mtx3100+0x59/0x3c0 [mtx]
    Call Trace: mtx2428 <- mtx2032 <- mtx3627 <- mtx892

The driver fails to pin 801 pages (3.2 MB, the vertex ring) on a 497 MB box and
then **dereferences the NULL it just failed to allocate**. The process is left
unkillable and the machine needs a reboot.

**It is NOT memory pressure.** Retried on a freshly booted box with **428 MB
free** and the page cache dropped, the driver still fails -- and the first
failure is for **2 pages, 8192 bytes**:

    [mtx] *ERROR* Failed to create device buffer (size: 8192, type: 2)
    [mtx] *ERROR* Parhelia OS: Failed to allocate locked pages (nb: 2, type: 2)
    ... then 801 pages (3.2 MB) for type 2 and type 1, and the same oops

Two pages cannot fail for capacity on a box with 428 MB free, so the `type`
argument is what is being refused, not the amount. `type` 1 and 2 both fail.
The allocation is made against the PDEV's heap selector (`pdev->[0x4c]`, copied
from the create request's field +0x48 by `mtx2839`), so the likely reading is
that a *borrowed* PDEV does not carry whatever per-client heap state a real GL
client establishes -- a real client allocates from the same PDEV successfully,
which is what glxgears does throughout.

That is where the replay now stops, and it is a state question rather than a
resource one. Chasing free memory is a dead end.

For the **open kernel driver this remains a specification point**: every
device-buffer allocation must be failure-checked and must unwind. The vendor
driver dereferences the NULL it just failed to allocate, which is reachable
whenever the allocation is refused for any reason -- as here, where the reason
was not scarcity at all.


## Why device-buffer allocation fails, decoded (2026-09-05)

Three independent causes, none of them memory pressure. All from `mtx.ko`,
which still carries **real symbol names** for the OS glue (`OsAllocLockedPages`,
`MtxRegion_Alloc`, `mtx_mem_alloc_agp/pci`, `mtx_open`, `mtx_ioctl*`) even
though `mtx_parhelia.o` is fully obfuscated. That layer is where the answers are.

### Three heaps, chosen by the client

`mtx938` dispatches on the request's memory-type field:

| request type | wrapper | internal region type | backing |
|---|---|---|---|
| 1 | `mtx3583` | 2 | `agp_allocate_memory` + `agp_bind_memory` |
| 2 | `mtx586` | 1 | `__get_free_pages(GFP_KERNEL\|__GFP_DMA32)` |
| 4 | `mtx3185` | 3 | `vmalloc` (not DMA-able) |

The `nb` and `type` in "Failed to allocate locked pages" are the **page count**
and the **internal** region type, not the request type.

### Cause 1: AGP never came up, and it fails silently

`mtx_mem_alloc_agp` (`0xc39a0`) bails out before allocating if `agp_drv` is NULL
or `agp_drv->[0x6c]` (the enabled flag) is zero -- **returning 0 with no
message**. Neither of its two error strings appears in our logs, so it never got
as far as trying.

**Verified on hardware.** `GET_MEMORY_INFO` (nr=0) carries the driver's AGP
aperture at `+0x20` (base) and `+0x28` (size), and on this box both read
**zero**, with no `[mtx] AGP aperture at ...` line in dmesg. The kernel's own
agpgart has an aperture (64M @ 0xc8000000); **mtx's AGP state does not**. So
every request-type-1 allocation is doomed here, quietly.

That makes `nr=0` a cheap pre-flight check: if `+0x20`/`+0x28` are zero, do not
expect AGP-backed buffers to work.

### Cause 2: the DMA32 path caps at 2 MiB

`mtx_mem_alloc_pci` (`0xc37a0`) searches orders 0..9 and gives up at
**order 9 = 2 MiB**, returning NULL *without printing anything*. Our failing
request is 3,280,896 bytes -- the 3 MB vertex ring -- which needs order 10. No
amount of free memory helps.

(Note: an order-9 cap was once guessed at in this project as the cause of an
unrelated `glTexImage2D` hang and correctly retracted -- that symptom really was
a missing `-lm`. The cap is real, but it explains *this* failure, not that one.)

### Cause 3: request type 2 is broken in the vendor driver

`mtx938`'s type-2 branch calls `mtx586` and then jumps to a `xor eax,eax`
return-false, **never copying the returned pointers out**. So request type 2
always reports failure and leaks whatever it allocated. Do not model an open
driver on that path.

### The oops is a separate defect: an unresolved pixel format

The NULL dereference at `mtx3100+0x59` (`movzx edx,[rax+4]`) is **not** a
consequence of the allocation failures. `desc->[8]` is NULL, and it traces back
through `mtx263` (`desc[8] = req[0]`) to `mtx3061`, where
`req[0] = mtx1431(pdev->[0x6c0], userreq->[8])`.

`mtx1431` -> `mtx473` validates the format code against a jump table covering
**0x14..0x50**, returning 0 for anything else; `mtx957` then linear-scans the
device's format table and returns NULL on a miss. Nothing checks the result
before `mtx3100` dereferences it.

The fault address being 4 rather than 0x178 also proves the format *table*
pointer was valid -- the table exists and the **lookup missed**.

So a client can oops this kernel with a bad format enum. For the open driver
that is a hard requirement: **range-check the format code and NULL-check the
table lookup before use.**

### Contract for an open driver

* Offer the three heaps, but **do not cap DMA allocations at MAX_ORDER** -- a
  3 MB vertex buffer is ordinary for GL. Use scatter-gather through the
  GART/IOMMU rather than one contiguous block. This is the single most important
  correction to the vendor design.
* Fail loudly when the GART is unavailable instead of returning NULL silently.
* Validate every userspace-supplied enum before dereferencing anything derived
  from it.
