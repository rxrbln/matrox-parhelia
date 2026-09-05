<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# A from-scratch open kernel driver for Matrox Parhelia — design

The vendor kernel module (`matroxdriver-x86_64-cvs-20090511`) builds only against
Linux 2.6.30. Everything below is written to replace it, not to wrap it.

This document is a design, not a survey. It assumes and does not re-derive:
`docs/parhelia-kernel-abi.md` (ioctl ABI, DMA packet format, indexed FIFO,
`0x12BC`/`0x41C` fence pair, interrupts, device init, verified negatives),
`docs/parhelia-command-construction.md` (the seven mappings, ring geometry,
`0x2000` slot stride, per-draw cost), `docs/parhelia-draw-recipe.md` (the 17
initialisation submissions, the per-draw sequence, the seven waits),
`docs/parhelia-3d-driving.md` (bus-type caveat), and
`docs/parhelia-2d-accel-complete.md` (the 2D engine).

Tags: **[CONF]** established in the cited document · **[HW]** proven on a real
board · **[INF]** inferred here, with the evidence given · **[DECIDE]** a design
decision this document makes · **[DATA]** blocked on a measurement we do not
have, with the experiment named.

---

## 1. Scope and structure

**Recommendation: a DRM driver — `drm_parhelia` — with GEM buffer objects over a
`drm_mm` VRAM allocator and plain coherent DMA for system memory. Not TTM. Not a
custom chardev. Not a shadow of the vendor ABI.** Render-only at first, KMS as
the destination. The kernel owns the whole drawing engine — 2D and 3D — and
userspace never touches an engine register. [DECIDE]

### 1.1 Why DRM

A custom chardev is what the vendor built, and it is the reason this hardware is
stranded: a private ABI with no in-tree consumers ages out with the kernel it was
compiled against. DRM gives, for free, everything this driver would otherwise
have to invent badly:

* GEM handles, per-file handle tables, and automatic cleanup on close — the
  vendor reimplemented this as `mtx_region_t` with an `==` match on a cookie.
* `dma_buf` / PRIME, so the DDX and a Mesa driver can share buffers.
* `drm_syncobj` and `dma_fence`, so completion has a standard shape.
* Render nodes, so a GL client needs no X-server privilege — the vendor's split
  into a *root* submission path (ioctl `0xc018461b`) and an *auth* path
  (ioctl `0x4621`) exists only because it had no equivalent.
* A KMS framework for stage 8, plus `fbcon` and VT-switch handling.

### 1.2 Why not TTM

TTM exists to migrate buffers between placements under memory pressure, driven by
fences. That requires the ability to move a buffer *after* a client has referred
to it — which requires a GPU page table. Parhelia has none: on PCI and PCIe the
engine consumes plain bus addresses, and the only translation that exists anywhere
is the AGP GART, which lives in `agpgart` and not in this device
(`docs/parhelia-3d-driving.md`, "Address-space caveat"). [CONF]

Consequently every engine-visible buffer is pinned for its whole life, and the
allocator degenerates to a range allocator. That is `drm_mm`, ~200 lines of glue,
against several thousand lines of TTM semantics we would never exercise. The
working set is also small and long-lived: one context allocates a 3 MB vertex
ring, a 672 KB command ring and a 4 MB texture heap, once, at context creation
(`docs/parhelia-command-construction.md`). Nothing about that shape wants an
eviction engine.

### 1.3 Why not a shadow of the vendor ABI

Reproducing the vendor ABI would import, specifically:

* **Kernel virtual addresses in userspace.** `GET_BUFFER` and the ICD control
  block hand out `0xffffc200_…` vmalloc pointers as buffer handles [CONF]. That
  is a KASLR infoleak on any modern kernel.
* **`mmap` cookies that are physical addresses.** For `MTX_MEMTYPE_PCI_LOCKED`,
  `region->base = __pa(kaddr)` and `mtx_mmap` matches it with `==` over the
  *device-wide* region list [CONF]. A client that guesses another client's
  cookie maps another client's buffer. There is no owner check on the mmap path.
* **No command-buffer validation at all** (§5.7 of the draw recipe), while the
  buffers contain bus addresses.
* **An error channel in a payload field** (`CB[0x234]`) rather than the ioctl
  return value, which returns 0 even when the call is refused [CONF].
* **A user-mappable page holding kernel locks** (`pStatus[0x70]`, `[0x7c]`) that
  the kernel spins on without a timeout [CONF].
* **One ioctl per draw.** A texture upload plus one textured triangle costs 30
  zero-sized doorbell ioctls, and 17 submissions complete before the client's
  first GL call [HW].

What *is* worth reusing verbatim is the hardware protocol: the packet format, the
arm sequence, the fence packets, the WARP microcode load. Those are hardware
facts. The ABI around them is not.

### 1.4 Kernel/userspace split

| Owned by the kernel | Owned by userspace |
|---|---|
| PCI probe, BAR mapping, MTRR/WC | building the command stream (register index/value packets) |
| WARP microcode load and reload | vertex, texture and shader data (written directly into mapped BOs) |
| Engine arm (`0x10b0`, `0x12b8`, `0x300`, `0x308`, `0xbc0`, `0x314`, the `0x0000–0x0130` aperture bank) | choosing placements and sizes |
| The primary DMA ring and its doorbell `0x408` | batching draws into one submission |
| Command-buffer validation and relocation | fast-path completion polling (read-only fence page) |
| Fences, interrupts, hangcheck, reset | GL/EXA semantics |
| VRAM allocation, GEM, PRIME | — |
| (stage 8) modeset, vblank, cursor, page flip | — |

Note the transfer of the aperture bank and `0x314` into the kernel. In the vendor
stack these are written by `mtx_drv.so` from its own BAR1 mapping — the kernel
module never touches `0x0000–0x0130` [CONF], and its 17 writes to `0x314` were
only found by a live mmiotrace [HW]. They are engine-arm state, so they belong
with the engine.

### 1.5 The register-range split, and the `0x384` hazard

Until KMS lands, two drivers hold a mapping of the same 8 KB BAR1. The split is
by range and each range has exactly one writer:

| Range | Owner | Contents |
|---|---|---|
| `0x0000–0x0130` | kernel | aperture / surface-descriptor bank |
| `0x0300–0x0460` | kernel | bus master, primary DMA, `0x314`, `0x384`, `0x450` |
| `0x08F0–0x0924` | kernel | status, IRQ, WARP |
| `0x0BC0`, `0x10B0`, `0x12B8` | kernel | memory config, engine arm |
| `0x1000–0x13FC` | kernel | the indexed engine register file |
| `0x1800–0x1DFF` | DDX (stage ≤7), kernel (stage 8) | display controllers, PLLs, DVO/DAC, DDC |

The one place this split leaks is the **DMAPAD write-combining workaround**. The
rule is stateful: emit `0x384 = 0` when the previous register offset written was
within 128 bytes *below* the new one; a register *read* cancels the pending pad
[CONF]. With two independent writers to the same BAR the "previous offset"
shadow is wrong by construction.

Resolution [DECIDE]: the kernel does **not** track the previous offset. It emits
`0x384 = 0` unconditionally before every CPU MMIO write to an engine register.
Kernel CPU writes to engine registers happen only during arm, microcode load and
reset — a few hundred writes per device lifetime — so the doubled write count is
free, and correct-by-construction beats a shared shadow. Inside the DMA stream the
in-band pad (index `0x20`, `0x1080`) serves the same purpose and the CPU shadow is
irrelevant. The DDX must apply the same unconditional rule to its display-range
writes; it does not today, and that is a small patch to `src/parhelia_mode.c`.

The vendor gates the whole workaround on `pStatus[0x84]`, defaulted to 1 but
overwritable by the client [CONF]. Whether it can be turned off is unknown
(draw-recipe §5.11 open item 9); we leave it on. [DATA]

### 1.6 A conflict in the primary-DMA register names, resolved

`docs/parhelia-3d-driving.md` §1 names `0x400` PRIMADDRESS and `0x404` PRIMEND.
`docs/parhelia-kernel-abi.md` §6.4 and `docs/parhelia-draw-recipe.md` §4.6 name
`0x404` PRIMADDRESS (written `& ~3`), `0x408` PRIMEND (the doorbell, carrying a
bus address), and `0x400` a **status write-back pointer**. The latter is read
directly off `mtx2508`/`mtx3229` and matches the live arm trace, which writes
`0x404 = ringBase` and then `0x408 = ringBase | 0x23`.

**Follow the kernel-abi/draw-recipe naming.** `0x400` = write-back pointer,
`0x404` = ring base, `0x408` = doorbell. [DECIDE] The `0x404 = start` /
`0x408 = end|3` recipe that drew a fill on hardware is the *direct*-submission
mode of the same registers (`pStatus[0x8fc] == 2`, "direct PRIMEND"), not a
contradiction.

---

## 2. Memory management

### 2.1 The buffer classes the hardware actually needs

| Class | Backing | Engine sees it as | Used for |
|---|---|---|---|
| **VRAM** | BAR0, `drm_mm` range allocator | a **byte offset within video memory** (surface descriptor dw0 = 0 is the start of the framebuffer) [CONF] | scanout, pixmaps, render targets, depth, textures, vertex heap, command heap |
| **DMA32-coherent system** | `dma_alloc_coherent` with a 32-bit mask | a plain 32-bit bus address | the primary ring, the validated secondary command buffers, the fence write-back page |
| **Shmem system, CPU-only** | `drm_gem_shmem` | never | staging, GEM objects a client wants but the engine never reads |

There is no fourth class. **AGP is deliberately not implemented.** [DECIDE] See
§2.5.

### 2.2 VRAM first — this is the vendor's own design intent, not a guess

The ICD's allocation preference order for the vertex ring, command ring and
texture heap is class 1 → 2 → 3 → 4, i.e. **VRAM → PCI-locked → AGP → system**,
and the capture shows it walking that list and only landing on system memory
because the vendor kernel's allocator failed all three earlier classes
(draw-recipe §1.2, submissions 10–13; the kernel logs
`Failed to allocate locked pages (nb: 801, type: 2)`) [CONF]. The failure is the
`MTX_MAX_ORDER = 9` cap discussed below, plus a genuinely absent AGP on that box.

So an open driver should put **every buffer the engine reads in VRAM**: the
command heap, the vertex heap, the texture heap, render targets. 128–256 MB is
ample for a part of this era. The client maps them write-combining and writes
them with streaming stores, exactly as the vendor's own client already does
(the mmiotrace's 384,680 undecodable `rep stosq` lines are bulk CPU writes into
the BAR0 aperture [HW]).

**[INF], and it matters:** those `rep stosq` writes into BAR0 are the strongest
evidence that the vendor's system-memory vertex/texture heaps were *staging*
buffers copied by the CPU into VRAM, not DMA sources. It also explains the
otherwise impossible fact that a `vmalloc`'d — physically scattered — 3 MB vertex
ring works on a device with no GART. If that reading is right, an open driver
that allocates the heaps in VRAM directly removes a whole CPU copy per frame.
Experiment that settles it: allocate the vertex heap in VRAM, bind it, and check
whether any bus address for it ever needs to reach the hardware. Related open
item: where the vertex-ring base is bound is unresolved in the vendor stack too
(`docs/parhelia-command-construction.md`, "Still open"). [DATA]

### 2.3 The `MTX_MAX_ORDER = 9` trap and how to not fall in it

The vendor allocates contiguous system memory with
`__get_free_pages(GFP_KERNEL|__GFP_DMA32, order)` and refuses any order above 9,
capping a contiguous allocation at **2 MB** — the 801-page (3,280,896 byte)
vertex ring is rejected *before* the allocator is even called [CONF].

An open driver avoids this three ways, in order of importance:

1. **Do not need large contiguous system memory.** With §2.2 the only contiguous
   system allocations left are the primary ring (propose 4 × 64 KB) and one 4 KB
   fence page per context. Both are far below any order limit.
2. **Use `dma_alloc_coherent`, never `__get_free_pages` + `virt_to_bus`.**
   `dma_alloc_coherent` goes through CMA and the DMA API, so it can satisfy large
   orders where the buddy allocator cannot, and it returns a genuine
   `dma_addr_t` — correct behind an IOMMU, which `virt_to_bus()` is not. Call
   `dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))` at probe and fail the probe
   if it is refused: `0x404`, `0x408`, `0x1090` and the SECADDRESS/SECEND stream
   fields are all 32 bits wide, so the device cannot address above 4 GB.
3. **Never hard-code an order ceiling in the driver.** Sizes come from the
   caller; a request that the allocator cannot satisfy fails with `-ENOMEM` and
   is reported to userspace as such — not silently retried into a different class
   the way the vendor does, which is precisely why an allocation failure there
   surfaces to the GL client as an indefinite block in `glFinish` rather than an
   error [CONF].

### 2.4 GEM object model

One object type, two backings, decided at create time and immutable thereafter:

```c
struct parhelia_bo {
	struct drm_gem_object	base;
	enum parhelia_bo_place	place;		/* VRAM | DMA32 | SHMEM */

	/* VRAM */
	struct drm_mm_node	vram;		/* offset within BAR0 == GPU address */

	/* DMA32 */
	void			*kaddr;
	dma_addr_t		dma;

	/* SHMEM: struct drm_gem_shmem_object semantics, no GPU address */

	struct dma_resv		*resv;		/* base.resv; fences live here    */
	bool			engine_visible;	/* VRAM or DMA32                  */
	u32			map_flags;	/* WC by default; UC on request   */
};
```

Rules:

* **A VRAM BO's GPU address is its `drm_mm` offset.** It is not a bus address and
  it is safe to expose to userspace: it is meaningless outside this device's
  video memory. Surface-descriptor `dw0` takes it verbatim.
* **A DMA32 BO's bus address is never exposed.** Only the kernel needs it, and it
  is a host physical address. Userspace refers to buffers by GEM handle and lets
  the relocation mechanism (§3.4) place the address.
* **Engine-visible BOs are pinned from create to destroy.** No migration, no
  eviction, no shrinker — see §1.2.
* **CPU mappings are write-combining by default.** `arch_phys_wc_add()` over the
  whole of BAR0 at probe, `pgprot_writecombine` on the fault path. A BO may
  request uncached for readback (`PARHELIA_BO_MAP_UC`); the driver never offers
  cached, because there is no cache-maintenance point in the submission path.
* **Freeing waits.** `drm_gem_object_free` blocks on the BO's `dma_resv` fences
  before releasing the range. The vendor does the exact opposite — `RELEASE_BUFFER`
  performs no synchronisation at all and can hand a page the engine is still
  reading back to the page allocator [CONF] — and pushes the ordering constraint
  onto the client. Do not repeat that.

### 2.5 Bus types

`docs/parhelia-3d-driving.md` establishes that address *production* is bus-type
dependent while the register protocol is not.

* **PCI / PCI-X** (the capture board, `102b:0528`, no AGP and no PCIe capability):
  plain bus addresses. This is the only configuration any capture covers. [HW]
* **PCIe** (LX/P650/P690 `2537`/`2538`/`2539`): plain bus addresses again, with
  IOMMU translation where one is active. The DMA API handles this transparently
  *provided* addresses come from `dma_alloc_coherent`/`dma_map_*`. Nothing else
  changes.
* **AGP** (`0x0527`, AGP `0x0528`): the aperture is GART-translated and the
  path is entirely uncaptured.

**Decision: do not implement AGP DMA.** [DECIDE] With §2.2 every engine-visible
buffer except the primary ring is in VRAM, and the primary ring can be in VRAM
too if the address-space selector for VRAM is known (§3.2, [DATA]). AGP support
would mean carrying `agpgart` bindings that modern kernels have been actively
removing, to serve a path we cannot test, for a memory class we do not need. On
an AGP board the driver runs with VRAM-only placements; if that turns out to be
insufficient the fallback is DMA32 coherent memory through the AGP *host* bridge
as ordinary PCI DMA, which AGP boards also support.

---

## 3. Command submission

### 3.1 The single most important decision: what the client submits

**The uAPI command buffer is a raw Parhelia DMA packet stream — headers of four
register indices plus their data dwords — not the vendor's record grammar.**
[DECIDE]

The vendor's client emits an opaque `[opcode][count]` record stream of *GL state
slot ids*, and the kernel translates slot ids to registers through a 172-arm jump
table (`mtx2617`, `.rodata+0xba0`). That table is unread, and
`docs/parhelia-draw-recipe.md` §6 names it "the single highest-value next step"
and "blocks a from-scratch driver". It blocks a driver only if the driver decides
to reproduce that indirection. We should not: our userspace knows the registers —
they are documented in `parhelia-2d-accel-complete.md`, `parhelia-fragment-isa.md`,
`parhelia-rendertarget.md` and the rest — and emitting them directly removes an
entire translation layer along with the RE item that gates it.

The consequences are all good: the kernel validator works on a format that is
completely specified (`docs/parhelia-kernel-abi.md` §6.2, decoded from the
vendor's own software interpreter `mtx2434`); the same format serves 2D and 3D;
and it is the format the engine actually consumes, so nothing is lost in
translation.

### 3.2 Ring geometry

* **The primary ring is kernel-owned and never mapped to userspace.** Four
  buffers of 64 KB (`dma_alloc_coherent`, or VRAM once §2.2's selector question
  is settled). Multiple buffers, because with a single buffer every wrap costs a
  full CPU stall — the vendor's `SwitchToNewBuffer` runs the complete
  `WaitBusMastering` and then `BmPrimInt::Reset` on a single-buffer pool [CONF].
* **Ring entry** = the 5-dword secondary-call packet, verbatim:
  `{0x20292820, 0, start|space, end|space, 0}` — decoding as
  `DMAPAD=0; SECADDRESS=start; SECEND=end; DMAPAD=0` [CONF]. This is a classic
  MGA two-level DMA: the kernel ring "calls" the command buffer.
* **Doorbell** = `0x408 = ringBusBias + ringWritePointer`, i.e. a bus address,
  not an offset [CONF].
* **Restart / rewind**: `WaitBusMastering`, then `0x404 = addr & ~3`,
  then `0x400 = writeBackAddr` (`& ~0x1f` when the capability bit is set) [CONF].
* **Idle reference**: read `0x450` **at probe, before any register write**, and
  keep it. The vendor's idle test is a full 32-bit equality of `0x450` against a
  snapshot taken at init — there is no documented mask to use instead [CONF].
  Getting this wrong makes every idle wait a hang.

**[DATA] The low bits of SECADDRESS/SECEND are an address-space selector and its
encoding is only partly known.** The vendor emits `|2` for a system-memory
secondary buffer, the arm trace writes `0x408 = ringBase | 0x23`, and our own
hardware-proven ring used `end|3` on a `GFP_DMA32` buffer [HW]. No capture covers
a VRAM-resident command buffer. Experiment: place a known-good 2D fill stream in
VRAM and sweep the two low bits, watching for the fill to land. Until then the
kernel places ring and secondary buffers in DMA32 system memory, where `|2`/`|3`
are attested. Everything else stays in VRAM.

### 3.3 Copy-and-validate, not validate-in-place

**The kernel copies every client command stream into a kernel-owned,
client-unmappable secondary buffer, validating and relocating as it copies.**
[DECIDE]

This is not optional, and the reason is specific to this hardware: the command
stream contains **bus addresses**. Index `0x24`/`0x25` program a fence write-back
address and value; index `0x28`/`0x29` are SECADDRESS/SECEND, a nested DMA jump;
several surface and texture descriptors carry base addresses. A client that can
write any of those unchecked can make the GPU read or write arbitrary host
physical memory — a full system compromise from an unprivileged render node. The
vendor does not check, which is defensible only because its submission path is
root-only.

Validate-in-place is not sufficient: the client keeps the mapping and can rewrite
the stream between validation and fetch. The copy removes the TOCTOU window
completely because the executed bytes are never mapped to the client.

The cost is a `memcpy`. Measured per-draw command traffic is **24 bytes**, and
the largest single command-stream write in a whole textured-triangle session is
**3372 bytes**, during context init [HW]. This is noise.

Secondary buffers come from a pool of 64 KB DMA32 buffers, recycled when the
submission's fence retires.

### 3.4 What the validator checks

Framing, from `docs/parhelia-kernel-abi.md` §6.2 [CONF]:

* A packet is one header dword plus up to four data dwords. Header bytes are
  register indices consumed **LSB first**; `reg = 0x1000 + index*4`.
* Index `0x20` (`0x1080`, DMAPAD) consumes a data dword and discards it — the
  legal mid-header filler.
* Index `0x21` (`0x1084`) **terminates the packet**; the next dword is a new
  header. It consumes no data dword. Getting `0x20` and `0x21` the wrong way
  round yields a stream the engine mis-parses — the validator must model this
  exactly, and its walk must consume the buffer to its declared end with no
  remainder. That end-exactly check is the same self-test that caught every
  mis-framing during the RE (`docs/parhelia-draw-recipe.md` §7); keep it.

Index policy — **whitelist, not blacklist** [DECIDE]:

| Index | Register | Policy |
|---|---|---|
| `0x20`, `0x21` | `0x1080`, `0x1084` | allowed (framing) |
| `0x22`, `0x23`, `0x2a`, `0x2b` | header-repeat template / count | **rejected**. Two independent readings of `mtx2434` disagree on which pair carries the repeat feature (draw-recipe §5.12), and a repeat count is a loop the validator would have to unroll. No draw path uses it. |
| `0x24`, `0x25` | `0x1090`, `0x1094` | **rejected** — fence write-back address and value. Kernel-emitted only (§4). |
| `0x28`, `0x29` | `0x10A0`, `0x10A4` | **rejected** — SECADDRESS/SECEND. Nested jumps are the kernel's business; this is the arbitrary-DMA hole. |
| `0xAF` | `0x12BC` | **rejected** — DWGSYNC. Kernel-emitted only; a tag with bit 0 set means "block until complete" and a bogus one never retires. |
| `0x88`,`0x8A`,`0x8C`,`0x8E` | `0x1220`/`0x1228`/`0x1230`/`0x1238` | allowed **only** with a matching relocation entry (surface base) |
| `0x77` | `0x11DC` | allowed only with a relocation entry (texture base) |
| `0xC0` | `0x1300` (DWGCTL) | allowed, and **shadowed** by the kernel, because the kernel's own fence packet restores `0x1300` from a shadow. Mirror the vendor's rule: values whose low nibble is 6 or 7 are transient framing state and are not mirrored [CONF]. |
| everything else | | allowed if present in the whitelist table, else rejected |

The whitelist is seeded from the 2D register map (`parhelia-2d-accel-complete.md`
§2), the captured 3D state stream, the fragment/vertex ISA docs, and the default
state list, and grows empirically: a module parameter
`parhelia.log_rejected_index=1` logs every rejection with its index so the table
can be extended from real workloads rather than guessed at.

**A collision to be aware of when writing the table:** index `0x2C` is register
`0x10B0`, which the arm sequence writes as `0x10` ("bus-master arm", `mtx3376`,
[CONF]) *and* which the ICD writes as the primitive-type selector
`0x00040000 | (topology << 12) | entry` [CONF]. Two solid readings of the same
register. Most likely a multi-field register whose init value is benign; the
validator must allow `0x2C` from clients regardless, and the kernel must not
assume its arm value survives a draw.

**CORRECTION (2026-09-05): the vendor has no client relocations, and neither
should we.** The paragraph below described `value = surface_base + entry_offset −
entry_delta` as the vendor's model; that arithmetic is not on the path. The
vendor resolves a **32-bit buffer handle** through a `std::map` at bind time
(record key 0x08, `mtx2530`) and emits the base-address registers itself —
clients never place a GPU address in a command buffer. See
`docs/parhelia-buffer-binding.md` for the decode. The struct below therefore
stands as the **DRM ABI we propose**, not as reversed vendor format, and the
`0x88`/`0x8a`/`0x8c`/`0x8e`/`0x77` rows in the table above should read *rejected*
rather than *allowed with a relocation entry*: those registers are
kernel-emitted. A bind record `(slot, bo_handle, offset)` replaces them.

For reference, the proposed relocation entry, if a future path ever needs one:

```c
struct drm_parhelia_reloc {
	__u32 cmd_dword;	/* index of the data dword to patch, within the cmd BO */
	__u32 bo_index;		/* index into the submit's BO array                    */
	__u32 delta;		/* byte offset added to the BO's GPU address           */
	__u32 type;		/* PARHELIA_RELOC_VRAM_OFFSET | _SURF_DESC_DW0 | ...   */
};
```

The kernel resolves `bo_index` to a pinned BO, computes the address in the BO's
own address space (a VRAM offset for a VRAM BO, a bus address for a DMA32 BO),
adds `delta`, checks `delta + access_size <= bo->size`, and writes the result
into the **copied** stream. `cmd_dword` must land on a data dword the framing
walk actually identified as data — patching a header byte would be a way to
smuggle a rejected index past the walk, so the validator records a data-dword
bitmap during the walk and rejects any relocation that does not hit one.

### 3.5 Batching

**One ioctl per submission, N draws per submission.** [DECIDE] The client
accumulates state and draws into its own command BO and submits at flush,
`glFlush`, buffer swap, or when the BO fills. The vendor's one-doorbell-per-draw
(30 ioctls for a texture upload and one triangle [HW]) is the single largest
avoidable cost in that stack and there is no hardware reason for it: the ring
takes any number of secondary-call packets before the doorbell is rung.

Per submission the kernel appends, in this order — this is the vendor's own
ring sequence, and it is worth copying byte-for-byte because it is `[CONF]` and
we do not know which parts are erratum workarounds:

```
(a) cache-flush packet, 6 dwords:
       0x00000004, 0xc0afafc0, mode|7, fence|1, fence, saved(0x1300)
    (header 0xc0afafc0 = {0x1300, 0x12BC, 0x12BC, 0x1300})
(b) one secondary-call packet per validated command buffer:
       0x20292820, 0, start|space, end|space, 0
(c) the kernel's fence element, 3 dwords:
       0x21212425, seqno, fenceBusAddr
(d) doorbell: 0x408 = ringBusBias + ringWritePointer
```

Two things not to copy: the vendor's *hybrid* submission mode, where SECEND's bit
0 selects "jump plus inline copy of the descriptor tail into the primary ring"
(draw-recipe §2.4a), exists to let a client-built descriptor carry its own fence
element. Our kernel emits the fence itself, so plain jumps (bit 0 clear) suffice
and the ring stays simple. And the client-side fence at `(c)`'s position in the
vendor's client descriptor is redundant with the kernel's — the two counters are
in lockstep and which is authoritative is unresolved [CONF]; with one emitter the
question disappears.

**[DATA]** The `0x00000004` / `0x00000002` count prefixes appear on some ring
elements and not others, and `mtx2434` reads no count field at all (draw-recipe
§2.4a). Since the kernel builds the ring we simply pick the vendor's exact byte
sequences above and confirm on hardware; a ring dump around a `0x20292820`
settles the general rule.

**[DATA]** The arm trace shows `0x408` advancing by `0x20` per submission while
`mtx1937` increments the ring write pointer by `0x14`. We control both, so we
track our own write pointer; but if the hardware requires a `0x20` granularity
this shows up immediately as a mis-fetch, so pad ring entries to 32 bytes if the
first bring-up misbehaves.

### 3.6 The doorbell, the FIFO and the pre-submit gate

* `0x314 = 1` kicks the front end. Purpose unknown, written in bursts during arm
  and 17 times across a session [HW]. Replicate it: after every doorbell.
* The indexed write FIFO is 16 deep and only matters for **CPU** writes to
  `0x1000–0x13FC`. The kernel counts its own writes and, on the 17th since the
  last drain, polls `0x450` bit 22 [CONF]. Per-draw traffic goes through DMA and
  never touches this counter.
* The pre-submit bus gate on `0x318`/`0x31C` is taken only when the ring's
  address-space selector is 0 [CONF]. With ring buffers in DMA32 system memory
  (selector 2/3) it does not apply; implement it anyway, bounded, because a VRAM
  ring may need it.

---

## 4. Fencing and synchronisation

### 4.1 The two hardware completion signals

Parhelia offers both, and they are complementary:

1. **The engine fence register pair.** A `0x12BC` write in the command stream
   emits a tag; `0x41C` reads back the last retired tag. Tags come from a counter
   **incremented by 8**, skipping 0; **bit 0 of the written value means "block
   until complete"**; `0x41C` reads back masked `& ~7` [CONF]. This requires an
   MMIO read to observe.
2. **The memory write-back word.** Register `0x1090` holds the 32-bit bus address
   of a 4-byte completion word and `0x1094` the value to write there; the engine
   writes it, and the CPU polls memory. Across five draws the packets carried
   3, 4, 5, 6, 7 and the page read back exactly those values [HW].

### 4.2 Design

**The write-back word is the primary signal; the `0x12BC`/`0x41C` pair is kept
for engine bookkeeping and idle waits.** [DECIDE]

* Per **context**, the kernel allocates one 4 KB DMA32-coherent page and maps it
  **read-only** into the client. The kernel keeps the bus address; the client
  never sees it.
* The kernel's fence element (§3.5(c)) programs `0x1090 = fencePageBusAddr` and
  `0x1094 = seqno` where `seqno` is the context's monotonic 32-bit counter,
  skipping 0.
* The kernel also emits a `0x12BC` tag with **bit 0 clear** in the same
  submission, from the step-8 tag domain, so `0x41C` stays a usable idle probe
  and the vendor's wrap-safe comparison remains valid:

  ```c
  /* mtx177 @0x4dc80, wrap-safe: */
  completed = read(0x41C) & ~7;
  latest    = ctx->last_issued_tag;
  done      = ((completed >= tag) != (completed > latest));
  ```

* Userspace fast path: `glFinish` is `while (READ_ONCE(*fence_page) < mine) cpu_relax();`
  — no ioctl, no interrupt, exactly as the vendor's ICD does it, and the one part
  of its design that is genuinely good.
* Userspace blocking path: `DRM_IOCTL_PARHELIA_GEM_WAIT` and
  `DRM_IOCTL_SYNCOBJ_WAIT` sleep on a `dma_fence` signalled from the interrupt
  handler. The vendor has **no completion ioctl at all** [CONF]; a client that
  wants to block has to burn CPU. We provide both and let the client choose.

### 4.3 Timeline

One `dma_fence_context` per DRM context; `seqno` is the fence seqno, so the
timeline is naturally ordered and `dma_fence_is_later` works unmodified. On
submit, the fence is attached to the `dma_resv` of every BO in the submission's
BO array (exclusive for written BOs, shared for read BOs), which is what makes
"never free a buffer the engine is still reading" automatic rather than a rule
the client has to obey (contrast draw-recipe §5.6).

Cross-context and cross-device ordering ride on `drm_syncobj` in/out arrays in
the submit ioctl. Hardware-side waits are not available — Parhelia has no
semaphore-wait command — so a cross-context dependency is resolved by the kernel
*not submitting* until the dependency fence signals, on a workqueue. That is the
correct shape for hardware of this class and it costs nothing at the common
single-context case.

### 4.4 Interrupts

`request_irq(..., IRQF_SHARED, ...)`. Hard IRQ handler, no tasklet:

```c
st = read(0x8F4);			/* status                              */
if (!st) return IRQ_NONE;		/* shared line, not ours               */
write(0x8F8, st);			/* ack: write the same bits back       */
if (st & PARHELIA_IRQ_ENGINE)  parhelia_fence_process(dev);
if (st & PARHELIA_IRQ_VBLANK0) drm_handle_vblank(dev, 0);
if (st & PARHELIA_IRQ_VBLANK1) drm_handle_vblank(dev, 1);
return IRQ_HANDLED;
```

Mask register is `0x8FC` [CONF].

**[INF] Bit assignments.** `OsSetVSyncInterruptState` accepts exactly three
masks — `2`, `0x10000000`, `0x20000000` — for heads 0, 1 and 2, with head 2
additionally gated on `0x1DC4 & 8` [CONF]. So `0x8F4` bit 1 is head-0 vblank,
bit 28 head 1, bit 29 the second-head-capable variant. The arm sequence enables
`0x8FC = 0x00010000` [HW], so bit 16 is very likely engine/DMA completion —
but nothing proves it, and `0x8F0` bit 17 (a *different* register) is the
documented "primary DMA finished" status.

**[DATA] Which bit of `0x8F4` signals ring completion is not established.**
Bring-up plan: run stage 4 with `0x8FC = 0xFFFFFFFF`, log every `0x8F4` value
against known-completed seqnos, and pin the bit. Until then the fence timeline
is driven by a 1 ms polling timer, which is correct if slow, and the switch to
IRQ is a one-line change.

### 4.5 Every wait is bounded

The vendor has seven distinct wait primitives and **none of them has a timeout**
[CONF]. Worse, `WaitBusMastering` step 1 spins on a non-`volatile` shared field
whose load the compiler hoisted, so all seven sites contain
`cmp DWORD PTR [rdx+0x78],0 / jne <itself>` — an infinite loop that never
re-reads memory [CONF].

Rules for the open driver:

* Every poll is `readl_poll_timeout()` (or `readl_poll_timeout_atomic()`), with
  a deadline, and a timeout is a **fault** that enters §5 — never a silent
  continue. (The vendor's only two bounded loops are display-side and time out
  *silently*.)
* Every read of a shared memory word is `READ_ONCE()`.
* The composite engine-drained wait, bounded, in the vendor's order:

  ```
  1  READ_ONCE(pending_submits) == 0            (kernel-private, not shared)
  2  poll 0x8F0 until bit 17 (0x00020000) set   primary DMA finished
  3  poll 0x450 until == probe_snapshot         full 32-bit equality (§3.2)
     fifo_count = 0
  ```
* The WARP idle wait, needed before any microcode load or reload [CONF]:
  `0x908` until `(v & 0xfff7ffbf) == 0x63a78001`, then `0x924` until
  `(v & 0xfff) == 0x100` with `0x908` still matching, then `0x8F0` until
  bits 24–25 are clear.
* No kernel lock is ever held across a hardware wait, and no lock lives in a
  user-mappable page.

---

## 5. Reset and fault recovery

The vendor stack has no working path: its wait-for-idle contains the hoisted
infinite loop above; its `MTX_IOCTL_HARD_RESET` is `#ifdef DEBUG`, never
registered in release builds, and does not touch the hardware; the only genuine
reset — `HSLPARHardwareReset` — is an exported HAL entry point with **no caller
anywhere in the driver**; and the WARP microcode is never reloaded after a fault
[CONF]. Its actual fault response is a latched "lost link" flag that makes every
subsequent submission return `-3` forever.

### 5.1 Fault detection

* **Hangcheck timer per submission.** Armed at doorbell, cancelled when the
  seqno retires. Default deadline 500 ms, module parameter. On expiry, one
  retry window: if `*fence_page` or `0x41C` advanced at all since the last
  check, re-arm — the engine is making progress on a long batch. If nothing
  advanced, declare a hang.
* **Any bounded wait that times out** (§4.5) declares a hang immediately.
* **Corroborating reads** for the log: `0x8F0` (bit 17 primary-DMA-finished,
  bits 24–25 WARP busy), `0x908` (WARP idle signature `0x63a78001`), `0x924`
  (`& 0xfff == 0x100` when the pipe is ready), `0x450`, `0x41C`, and the ring
  read/write pointers. Dump the offending secondary buffer to a debugfs
  `parhelia/hang` blob — we have the copy, and the client cannot have altered it.

### 5.2 Quiesce

In this order:

1. `write(0x8FC, 0)` — mask all interrupts.
2. Stop the submit path: take the device submit mutex, refuse new doorbells.
3. Bump the device **reset epoch**.
4. Signal every in-flight `dma_fence` with `-ECANCELED` (`dma_fence_set_error` +
   `dma_fence_signal`) so no waiter is stuck. This is the step whose absence in
   the vendor stack turns a hang into a permanently blocked `glFinish`.

### 5.3 Reset

There is **no engine-only soft reset** on this hardware — recovery is a
chip-level reset plus a full re-init [CONF]. The sequence is
`HSLPARHardwareReset`, which we invoke because the vendor never does:

```
1  pci_save_state(); additionally save config dwords 0x40, 0xE0,
   and 0xF4/0xF8 when (cfg[0x3A8] & 0xE0) != 0x80
2  write_reg(0x308, 3)                       /* 0x0528 and 0x2537..0x2539 */
3  v = read_reg(0x300); rebuild from PCI config 0x04; write back with the
   documented bit cleared
4  pci_write_config_dword(0xE0, 3)
5  pci_write_config_dword(0xE0, 0)           /* the strobe */
6  msleep(10)
7  pci_restore_state(); restore 0x40/0xE0/0xF4/0xF8; restore reg 0x300
```

**[DATA] The two sources disagree on which bit of `0x300` is cleared at step 3**
— `docs/parhelia-kernel-abi.md` §5.8 says bit 9, `docs/parhelia-2d-accel-complete.md`
Op 12 says bit 8. Read the instruction once more before implementing; both
readings come from the same function. Related and also unsettled: the
`0x300` quirk-bit lane disagreement in kernel-abi §9.3 (`0x0800` vs `0x080000`,
`0x80` vs `0x8000`) — the byte-granular reading given there is believed correct
but has never been checked on hardware.

Note this is a *chip* reset: it takes the display down with it. See §5.6.

### 5.4 Re-init

After the strobe, nothing survives. Re-run, in full:

1. The arm sequence: `0x10b0 = 0x10`, `0x12b8 = 0x00070601`,
   `0x300 = (read & 0xe0e0ff3f) | agpbits | 0x100 [| quirks]`,
   `0x308 = 0x83` on `0x0528` else `0x01` [CONF].
2. The `0x0000–0x0130` aperture bank — 20 slots × 4 dwords
   `{base, size, 1, 0x10001}` [HW].
3. `0xbc0 = 0x4087`, `0x920 = 1`, the `0x314` kicks.
4. **Re-stream the WARP microcode.** Nothing reloads it independently. Full idle
   wait (§4.5), `0x918 = byteOffset / 12`, three dwords per instruction to
   `0x91C`, then `0x920 |= 1`. Exactly one image is known: 4812 bytes, 401
   instructions of 12 bytes, md5 `b29644ecfcfcbbc11ef97240872892ca`,
   extractable with `tools/parhelia-extract-ucode.c` and already checked in as
   `tools/warp_ucode.h` [CONF]. The instruction store is `0x1800` bytes = 512
   instructions and the loader reports overflow rather than waiting on it — the
   one bounded error path in the whole vendor engine, and evidence that reload
   after a fault was anticipated (`bReset` clears the load pointer and the
   overflow flag).
5. Re-snapshot `0x450` for the idle reference (§3.2).
6. Reset the primary ring: `0x404 = base & ~3`, `0x400 = writeBackAddr`,
   `0x408 = base | space`.
7. `0x8FC` = the enable mask.

The microcode should ship as a `request_firmware()` blob (`parhelia/warp.fw`)
with the in-tree `warp_ucode.h` as a fallback, so a future second revision is a
firmware update rather than a driver change. Do not assume a second revision
exists without a fresh hash [CONF].

### 5.5 Context recovery

After a reset, every fence value, every ring offset and every engine-side surface
binding a client holds is stale.

* Contexts created before the current reset epoch are **dead**: any submit
  against them returns `-ECANCELED`.
* `DRM_IOCTL_PARHELIA_GET_RESET_STATS` reports, per context,
  `{reset_count, batch_active, batch_pending}` — the i915/amdgpu shape, which is
  exactly what `GL_ARB_robustness` needs to raise `GL_GUILTY_CONTEXT_RESET`.
* The context whose submission was in flight is marked **guilty**; contexts merely
  queued behind it are **innocent** and may be resubmitted by their clients.
* BOs survive. Their contents are undefined but their allocations, GPU addresses
  and mappings are intact, so a client re-creates a context and re-uploads rather
  than re-allocating everything.
* A per-context "ban" counter: N guilty resets in a row and the *file* is banned,
  to stop a client from cycling the GPU forever.

The vendor's "lost link" latch (draw-recipe §5.9) is the same idea with no way
out; the epoch plus a re-createable context is the way out.

### 5.6 The display problem, and why it argues for KMS

A chip-level reset blanks the display. While modeset lives in the DDX (stages
≤7), the kernel cannot restore it. Two options, and the choice is explicit:

* **Stage ≤7 (UMS DDX):** the full `0xE0` strobe is **disabled by default**
  (`parhelia.allow_chip_reset=0`). Recovery is best-effort: quiesce, ban, re-arm
  the engine and re-stream the microcode *without* the config-space strobe, then
  test the engine with a known-good internal 2D fill. If that fails, the device
  stays quiesced, all contexts fail, and the log says so plainly. This is honest:
  there is no soft engine reset, so a soft recovery may simply not work.
* **Stage 8 (KMS):** the strobe is enabled. The kernel owns the modeset state and
  re-runs it after the reset, and the DDX/compositor sees a normal
  `drm_atomic` restore. This is the shipping configuration.

An intermediate is available if stage 8 is far off: send a `DRM_PARHELIA_EVENT_RESET`
to any DDX holding the primary node and let it re-run its own modeset. The DDX
already has a complete native modeset path, so this is a small addition — but it
is a workaround, and KMS is the fix.

---

## 6. The uAPI

`DRM_IOCTL_BASE 'd'`, driver ioctls from `DRM_COMMAND_BASE`. All structs are
64-bit clean with explicit padding, `__u64` for pointers, and every reserved
field must be zero (checked, so it can be used later). No kernel pointer, no host
physical address and no kernel virtual address ever crosses this boundary.

```c
#define DRM_PARHELIA_GET_PARAM		0x00
#define DRM_PARHELIA_GEM_CREATE		0x01
#define DRM_PARHELIA_GEM_MMAP_OFFSET	0x02
#define DRM_PARHELIA_GEM_INFO		0x03
#define DRM_PARHELIA_GEM_WAIT		0x04
#define DRM_PARHELIA_CTX_CREATE		0x05
#define DRM_PARHELIA_CTX_DESTROY	0x06
#define DRM_PARHELIA_SUBMIT		0x07
#define DRM_PARHELIA_GET_RESET_STATS	0x08

/* ---- parameters ---- */
#define PARHELIA_PARAM_CHIP_ID		1	/* PCI device id                    */
#define PARHELIA_PARAM_VRAM_SIZE	2	/* bytes usable by the allocator    */
#define PARHELIA_PARAM_VRAM_APERTURE	3	/* bytes visible through BAR0       */
#define PARHELIA_PARAM_BUS_TYPE		4	/* PCI | PCIE | AGP                 */
#define PARHELIA_PARAM_UAPI_VERSION	5
#define PARHELIA_PARAM_HAS_WARP		6	/* microcode loaded, 3D usable      */
#define PARHELIA_PARAM_MAX_CMD_SIZE	7	/* bytes per command BO per submit  */
#define PARHELIA_PARAM_RESET_EPOCH	8

struct drm_parhelia_get_param {
	__u32 param;
	__u32 pad;
	__u64 value;			/* out */
};

/* ---- buffer objects ---- */
#define PARHELIA_BO_PLACE_VRAM		0	/* engine-visible, GPU addr = offset */
#define PARHELIA_BO_PLACE_DMA32		1	/* engine-visible, coherent system   */
#define PARHELIA_BO_PLACE_SHMEM		2	/* CPU only, never engine-visible    */

#define PARHELIA_BO_MAP_WC		(1u << 0)	/* default            */
#define PARHELIA_BO_MAP_UC		(1u << 1)	/* readback           */
#define PARHELIA_BO_SCANOUT		(1u << 2)	/* pitch/alignment    */

struct drm_parhelia_gem_create {
	__u64 size;			/* in; page-rounded                        */
	__u32 place;
	__u32 flags;
	__u32 handle;			/* out                                     */
	__u32 pad;
	__u64 actual_size;		/* out; the rounded size, so the client     */
					/* never has to guess and retry            */
};

struct drm_parhelia_gem_mmap_offset {
	__u32 handle;
	__u32 pad;
	__u64 offset;			/* out; a fake mmap offset, not an address */
};

struct drm_parhelia_gem_info {
	__u32 handle;
	__u32 pad;
	__u64 size;			/* out */
	__u64 gpu_addr;			/* out; VRAM byte offset, or 0 if not VRAM */
	__u32 place;			/* out */
	__u32 flags;			/* out */
};

struct drm_parhelia_gem_wait {
	__u32 handle;
	__u32 flags;			/* WAIT_WRITE: also wait on shared fences  */
	__s64 timeout_ns;		/* < 0 = infinite, 0 = poll                */
};

/* ---- contexts ---- */
struct drm_parhelia_ctx_create {
	__u32 flags;
	__u32 id;			/* out                                     */
	__u64 fence_page_offset;	/* out; mmap offset of the RO fence page   */
};

/* ---- submission ---- */
struct drm_parhelia_submit_bo {
	__u32 handle;
	__u32 flags;			/* READ | WRITE                            */
};

struct drm_parhelia_submit_cmd {
	__u32 handle;			/* command BO (any placement, CPU-written) */
	__u32 pad;
	__u64 offset;			/* byte offset of the stream, 4-aligned    */
	__u64 size;			/* byte length, multiple of 4              */
};

#define PARHELIA_SUBMIT_NO_IMPLICIT_SYNC	(1u << 0)

struct drm_parhelia_submit {
	__u32 ctx_id;
	__u32 flags;

	__u32 nr_cmds;			/* command buffers, executed in order      */
	__u32 nr_bos;
	__u32 nr_relocs;
	__u32 nr_in_syncobjs;
	__u32 nr_out_syncobjs;
	__u32 pad;

	__u64 cmds;			/* -> struct drm_parhelia_submit_cmd[]     */
	__u64 bos;			/* -> struct drm_parhelia_submit_bo[]      */
	__u64 relocs;			/* -> struct drm_parhelia_reloc[]          */
	__u64 in_syncobjs;		/* -> __u32[]                              */
	__u64 out_syncobjs;		/* -> __u32[]                              */

	__u64 seqno;			/* out; the value the fence page will reach */
	__u32 reset_epoch;		/* out; changed => the context is dead      */
	__u32 pad2;
};

struct drm_parhelia_get_reset_stats {
	__u32 ctx_id;
	__u32 flags;
	__u32 reset_count;		/* out; device-wide resets                 */
	__u32 batch_active;		/* out; this context was guilty            */
	__u32 batch_pending;		/* out; this context was innocent          */
	__u32 pad;
};
```

Standard DRM ioctls carry the rest: `PRIME_HANDLE_TO_FD` / `FD_TO_HANDLE`,
`SYNCOBJ_*`, `GEM_CLOSE`.

### 6.1 What this deliberately does differently from the vendor

| Vendor | Here | Why |
|---|---|---|
| error in `CB[0x234]`, ioctl always returns 0 | errno | a zero return that means failure is a bug factory |
| one zero-sized doorbell ioctl per draw | one submit per flush, N draws | 30× fewer syscalls on the measured workload |
| no validation of client buffers containing bus addresses | copy-and-validate with relocations | arbitrary DMA from an unprivileged client, otherwise |
| mmap cookie == physical address, device-wide `==` lookup | GEM handles, per-file tables, fake mmap offsets | cross-client buffer access and an infoleak, otherwise |
| kernel virtual addresses returned to userspace | never | KASLR infoleak |
| `RELEASE_BUFFER` frees pages the engine may be reading | free waits on `dma_resv` | use-after-free by DMA |
| no completion ioctl; clients spin | RO fence page **and** a blocking wait | a blocking client should sleep |
| kernel spins on a lock in a user-mappable page | no shared locks | an unprivileged client can hang the kernel |
| a rounded size the client must discover by retrying | `actual_size` returned | the vendor's ICD literally retries with the returned size |
| 13 core ioctls, several dead | 9, all live | `ADD_USER`/`REMOVE_USER` are never registered; `HARD_RESET` is compiled out |

### 6.2 Stability

Freeze the uAPI before the Mesa driver starts (end of stage 5). `PARHELIA_PARAM_UAPI_VERSION`
exists so a userspace driver can refuse a kernel it does not understand;
additions go through new params, new ioctl numbers and reserved-must-be-zero
fields, never by reinterpreting an existing field.

---

## 7. Modeset and coexistence with the DDX

### 7.1 Where modeset lives today

`src/` already does native modeset, EXA and XAA, hardware cursor, DDC and RandR,
and it works on hardware up to 1920×1200 on both the original `0x0528` and the
P690. It maps BAR0 and BAR1 itself through `pci_device_map_range()` and drives
the 2D engine by **direct MMIO PIO** — `parhelia_exa.c` writes
`0x1000 + idx*4` one register at a time, no DMA. That is why the DDX and a
render-only DRM driver can coexist at all at first: they use disjoint mechanisms.

### 7.2 The plan

**Destination: KMS in the kernel; the DDX keeps only its EXA, ported onto the DRM
node.** [DECIDE] Transitional UMS is a development mode
(`parhelia.modeset=0`), not a shipping configuration.

Three reasons the destination is not negotiable:

1. **Reset needs it** (§5.6). Without KMS the kernel cannot restore the display
   after the only reset this hardware has.
2. **There is no console today.** No fbdev, no `fbcon`, no VT-switch arbitration.
   `drm_fbdev` gives all three for free once KMS exists.
3. **Two writers to one BAR** is a standing hazard, and the `0x384` shadow (§1.5)
   is the first place it bites.

### 7.3 Migration path, concretely

The DDX code maps onto KMS almost one-to-one, because the RE already separated
the pieces:

| DDX file | KMS object |
|---|---|
| `parhelia_mode.c` per-head timing | 2 × `drm_crtc`. Exactly two heads exist — `0x1B00` is head 1's timing block, **not** a third head. |
| `parhelia_pll.c` | `crtc->mode_set` clock programming; per-head FS/PLL at `0xE10 + fs*4`, plus the >135 MHz DC-clock tap fix (1920×1080 / 148.5 MHz needs X=6) |
| DVO/DAC routing, the head→output crossbar | `drm_encoder` per output; the device-id split is real — original `0x1C20` vs LX/P690 twin `0x1C54`/`0x1C58` |
| `parhelia_ddc.c` | `drm_connector.get_modes` via `drm_do_get_edid`; the per-connector DDC head selector is already implemented |
| `parhelia_cursor.c` | `drm_plane` cursor. Keep the software-cursor fallback: on P690, row 0 renders but rows > 0 never land, and that is unresolved. |
| `parhelia_randr.c` | disappears — RandR comes from KMS |
| `parhelia_video.c` (Xv) | stays in the DDX, or becomes an overlay `drm_plane` later |

Constraints to encode in `atomic_check`: two CRTCs maximum; the per-head PLL
and DC-clock-tap rules; and the DC-sync `ApplyVidReset` strobe as the commit
step. The pixel-clock sharing constraint between heads noted as a TODO in the
dual-head work must be settled before the atomic check can be written. [DATA]

Migration order, with X never broken:

1. Stages 0–5: DRM render-only, `parhelia.modeset=0`. DDX unchanged, still UMS.
   Both drivers bind the same device; the range split of §1.5 applies.
2. Stage 6: DDX EXA moves onto `DRM_PARHELIA_SUBMIT`. The DDX opens the render
   node, allocates pixmaps as VRAM BOs, and stops writing engine registers
   entirely. Its BAR1 mapping shrinks to the display range. Big win on its own:
   2D goes from PIO to DMA.
3. Stage 8: KMS lands behind `parhelia.modeset=1`. The DDX gains a KMS path (or
   is replaced by `xf86-video-modesetting` plus the DRM node — but that loses
   EXA acceleration until a Mesa driver exists, so keeping the parhelia DDX with
   EXA-over-DRM is the better intermediate).
4. When Mesa exists: `xf86-video-modesetting` + glamor, and the DDX retires.

---

## 8. Staged implementation plan

Each stage has one testable milestone. No stage depends on unresolved RE.

**Stage 0 — skeleton.** PCI driver for `102b:0527/0528/2537/2538/2539`; map BAR1
uncached and BAR0 write-combining (`arch_phys_wc_add`); `drm_dev_alloc`,
`DRIVER_GEM | DRIVER_RENDER`; `GET_PARAM`. Snapshot `0x450` before any write.
Touch nothing else.
*Milestone:* `modprobe drm_parhelia`; `/dev/dri/renderD128` exists; `lspci -k`
shows the binding; `GET_PARAM` returns the right chip id and VRAM size; X with
the existing DDX still starts and displays.

**Stage 1 — memory.** GEM object, `drm_mm` VRAM allocator, DMA32 allocator,
mmap with WC, `GEM_CREATE/INFO/MMAP_OFFSET/CLOSE`, PRIME export/import.
*Milestone:* a test tool allocates a VRAM BO at a known offset, writes a pattern
through the WC mapping, and a second read-back mapping returns it byte-exact;
PRIME round-trip between two fds yields the same GPU address; closing the fd
frees the range (verified in `debugfs`). Do **not** validate by writing VRAM at a
guessed stride and looking at the screen — that aliases into fake artefacts.

**Stage 2 — engine arm, in-kernel.** Arm sequence, aperture bank, WARP microcode
load, primary ring, direct doorbell. A debugfs-triggered internal submission of
the fully-proven 2D solid fill.
*Milestone:* the fill lands in a VRAM BO and is verified by reading that BO back
through its CPU mapping (not by reading registers). Reproduces the
already-achieved 120 000-pixel DMA-ring result, now from the DRM driver.
`0x908` reads the WARP idle signature `0x63a78001` after the microcode load.

**Stage 3 — submit path.** `CTX_CREATE`, `SUBMIT` with copy-validate-relocate,
BO array, `dma_resv` attachment, polled seqno; `GEM_WAIT` on a timer.
*Milestone:* two tests. (a) A userspace 2D fill through the ioctl produces the
identical VRAM contents as stage 2. (b) A hostile-buffer suite — truncated
packet, non-terminating header, index `0x28`, index `0xAF`, index `0x24`,
out-of-range relocation, relocation pointing at a header byte, oversize command
BO, unaligned offset — every case returns `-EINVAL` and **nothing is submitted**;
verified by checking the ring write pointer did not move.

**Stage 4 — interrupts and fences.** IRQ handler, `dma_fence` timeline,
`drm_syncobj` in/out, the read-only fence page.
*Milestone:* the IRQ count equals the submission count over 10 000 submissions
(`/proc/interrupts`); a blocking `GEM_WAIT` sleeps rather than spins (measured by
CPU time); the fence page in userspace reaches the seqno the submit returned; and
the `0x8F4` completion bit is identified and recorded here.

**Stage 5 — hangcheck and reset.** Bounded waits everywhere, hangcheck timer,
quiesce, epoch, guilty/innocent, `GET_RESET_STATS`, re-init and microcode reload.
*Milestone:* a deliberately hanging submission (a blocking `0x12BC` tag emitted
by an internal test path, which is the known hang) is detected within the
deadline; all waiters wake with `-ECANCELED`; the device re-inits; a fresh
context draws the stage-2 fill correctly; `GET_RESET_STATS` reports the hanging
context guilty and a bystander innocent. **uAPI frozen at the end of this
stage.**

**Stage 6 — DDX on DRM.** Port `parhelia_exa.c` from PIO to
`DRM_PARHELIA_SUBMIT`; pixmaps become VRAM BOs; the DDX stops mapping the engine
register range.
*Milestone:* `x11perf -aa10text -copywinwin500 -rect500` before and after, with
the after strictly faster; and an `mmiotrace` of an X session shows zero DDX
writes below `0x1800`.

**Stage 7 — 3D bring-up through the uAPI.** A userspace test that emits the
default-state stream and one Gouraud triangle as register packets, per
`docs/parhelia-draw-recipe.md` §1.3 and §2, through `SUBMIT`.
*Milestone:* a correctly-coloured triangle in a VRAM render target, verified by
reading the target back — the same result `tools/parhelia-tri.c --3d` produces by
direct MMIO, now through the kernel. This is also the point at which the
fragment-shader input selector question (`0x1264` bits [9:5]) gets its capture.

**Stage 8 — KMS.** Port modeset from the DDX per §7.3; `drm_fbdev`; vblank on
the `0x8F4` bits; page flip; cursor plane; enable the chip reset.
*Milestone:* `fbcon` on the console; VT switch in and out of X; `modetest`
enumerates 2 CRTCs and the DVI/VGA connectors with correct EDIDs from both heads;
a page flip runs at the refresh rate; X on `xf86-video-modesetting` at
1920×1200; and a forced hang now recovers with the display intact.

**Stage 9 — Mesa.** Out of scope for this document; see
`docs/parhelia-mesa-driver.md`. The uAPI above is what it targets.

---

## 9. Where the M9 (`0x0540`) diverges

The M9 is a **different render engine** and must not be mixed into the Parhelia
engine code. It is, however, the same driver: PCI probe, GEM, `drm_mm`, fences,
syncobj, hangcheck, uAPI and the display lineage are all shared. The split is one
`struct parhelia_engine_ops` with two implementations.

What differs, from `docs/parhelia-m9-3d-submission.md` and
`docs/parhelia-m9-dma.md`:

| Parhelia | M9 |
|---|---|
| direct MMIO to `0x1000–0x13FC` drives the engine (`ExecuteEmulDma`) — so a PIO fallback and a software replay exist | the window **reads back all zero** and never renders; it is a decode shadow the DMA parser reads. There is no PIO path, and no software-replay validator oracle. |
| `0x404` PRIMADDRESS, `0x408` PRIMEND doorbell, `0x400` write-back | `0x400` PRIMADDRESS; an 8-dword ring descriptor at `0x500–0x524`; `0x314` kick and `0x31C` park/resume |
| completion by in-stream `0x1090`/`0x1094` write-back and `0x41C` | completion by write-back addresses programmed once into `0x1068`/`0x106C`; the driver polls memory, not `0x8F0` |
| packet terminator index `0x21`, pad `0x20` | terminator `0x45` |
| engine arm is the documented `0x10b0`/`0x12b8`/`0x300`/`0x308` sequence | a mandatory `EnableHardware`/`InitBoard` step behind a C++ virtual call (`m9x28002`, vtable `*0x30`) that is **not decoded** — this is the hard gap, and it is why every direct-MMIO M9 EXA attempt consumed operations and drew nothing |
| 2 display heads | 4 display controllers, DC `+0x48` start / `+0x58` pitch, 9 FS PLLs, a different DAC/DVO block |

Two rules follow. First, **the M9 engine backend cannot be written blind** —
each wrong guess risks a card lockup — so its bring-up must go through the
read-back diagnostic already identified: program the known context registers and
read them back *without* triggering execution, to learn which latch. Second,
because the M9 has no MMIO fallback, its stage-2 milestone is harder than
Parhelia's and should not gate Parhelia's schedule.

The M9's `0x400`/`0x404` literals also appear as *command opcodes* inside its
BmBuffer builders. Only writes through the register accessor are MMIO. That
pitfall produced bad leads once already.

---

## 10. Open items this design depends on

Ordered by how much they block.

1. **The address-space selector in SECADDRESS/SECEND / `0x408`** (§3.2). Blocks
   putting the primary ring and command buffers in VRAM.

   **Measured on hardware, 2026-09-05, 0528, X not running.** The two low bits of
   `0x408` were swept against the known-good 2D fill with the ring in DMA32
   system memory, each value from a fresh boot where the previous one had left
   the engine busy:

   | `0x408` low bits | pixels | `0x404` after | engine after |
   |---|---|---|---|
   | `3` | **120000** | advanced to `end` | idle |
   | `2` | 0 | still at `start` | left busy |
   | `1` | 0 | still at `start` | left busy |
   | `0` | not measurable — the preceding run had already left the engine busy | | |

   So **both bits are required** for a system-memory ring; neither alone starts a
   fetch, and clearing either leaves the engine latched and busy rather than
   erroring. Vendor ground truth agrees: `captures/mmio-draw-0528-decoded.txt`
   shows `0x404 = 0x14600000` (low bits **clear**) and every `0x408` write as
   `…|3`, advancing by `0x20` per submission, with the ring at a system physical
   address.

   **RESOLVED (2026-09-05), and it reframes the measurement.** The low two bits
   are not two enables — they are the **memory-space tag** that `mtx1132`
   (`.text+0x4c730`) ORs into every resolved GPU address: `0` = local
   framebuffer, `2` and `3` = the two system-memory classes. A DMA32 ring needs
   `|3` because 3 is its space; clearing either bit names a different space, and
   a system address read as a VRAM offset is out of range on a 256 MB card,
   which is exactly the stall observed. A VRAM ring should therefore take
   tag `0`. See `docs/parhelia-buffer-binding.md`.

   The original note follows. What it does
   **not** settle by itself is VRAM: under the MGA-lineage reading bit 1 would be
   "fetch across the bus", so clearing it should select local video memory — but
   the measurement above cannot distinguish that from "bit 1 is simply required",
   because the address submitted was a system address either way, and a system
   address read as a VRAM offset is out of range on a 256 MB card. Testing that
   properly needs a ring built in VRAM, which previously overran and locked the
   engine (`memory: parhelia-vram-ring-hazard`), so it needs a bounded packet and
   a health check either side.
2. **Which `0x8F4` bit signals engine completion** (§4.4). Blocks IRQ-driven
   fences; a polling timer is the stand-in. Experiment: unmask everything, log
   `0x8F4` against known completions.
3. **Whether the vertex/texture heaps must be DMA-read or are CPU-staged into
   VRAM** (§2.2). Decides whether a per-frame CPU copy exists at all. Related:
   where the vertex-ring base is bound is unresolved in the vendor stack too.
4. **The `0x300` bit cleared during reset** — bit 8 or bit 9 (§5.3), and the
   `0x300` quirk-bit lanes from kernel-abi §9.3. Both are single re-reads of the
   disassembly plus one hardware check.
5. **`0x3050`.** `parhelia-2d-accel-complete.md` records the vendor's own
   pushbuffer doorbell as `MMIO 0x3050 = 1`, but `GET_MEMORY_INFO` on the capture
   board reports `reg_size = 0x2000` — 8 KB — so `0x3050` lies **outside BAR1**.
   Either it is in a different aperture, or the register size differs by board, or
   one reading is wrong. It does not block the design (we use `0x408`), but it
   should be settled before anyone tries the alternative doorbell.
6. **The per-head pixel-clock sharing constraint** (§7.3), needed to write the
   KMS `atomic_check`.
7. **`0x400`'s `& ~0x1f` masking** when a capability bit is set, and the meaning
   of `0x314`, `0xbc0 = 0x4087`, `0x910`/`0x914`/`0x938`, and the 21 anonymous
   init/undo steps of the device start table. All are replicate-verbatim items:
   we copy the vendor's values and do not pretend to understand them.
8. **Whether `pStatus[0x84] = 0`** — disabling the `0x384` DMAPAD workaround —
   is safe. We leave it on; turning it off would remove one MMIO write per
   register write.

None of these blocks stage 0 through 3.
