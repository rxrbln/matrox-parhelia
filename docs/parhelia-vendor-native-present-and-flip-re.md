# Vendor native Present blit and page-flip synchronization

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note records clean-room reverse engineering of the saved Matrox binaries.
The primary sources are the Linux OpenGL ICD
`opengl6/lib64/libGL.so.1.3.0`, Linux kernel core
`kernel/mtx_parhelia.o`, Linux X driver
`xserver/7_4_0/mtx_drv.so`, and the Windows display driver
`xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll`.  Cached disassemblies are
in `scratchpad/libGL64.asm`, `scratchpad/mtxk_r.asm`, `scratchpad/mtx_drv.asm`,
and `scratchpad/pard.asm` respectively.  No target writes were made for this
analysis.

## Result

The vendor has no linear shadow or layout resolve in its normal presentation
path.  The primary is a native local-VRAM type-3 surface.  The CPU sees a
row-major alias of that same allocation through one of twenty programmable
host-aperture descriptors.  A windowed GL swap clips the drawable against its
visible region and queues native-surface-to-native-primary 2-D BITBLTs.  A
flip-capable swap instead queues a render-retirement tag and changes a hardware
START slot from the vblank interrupt.

Windowed swaps do **not** wait for vblank in the recovered Linux vendor path.
They are asynchronous engine blits and can tear.  `reg3DFlipOnVBlank` and the
GLX swap interval affect the page-flip path, not the windowed BLIT escape.

These are separate mechanisms:

1. type 2/type 3 is engine view metadata for one native allocation;
2. the host aperture gives the CPU a linear alias of that native allocation;
3. windowed Present is a 2-D blit between allocations;
4. fullscreen Present can change the display START address without copying.

## 1. The primary really is native type 3

`mtx_drv.so` `InitPrimarySurface` at `.text+0x7f1a0` constructs the screen in
local memory and ORs type 3 into its stored surface control at `0x7f242`.  It
uses the active display format and the native surface allocator.  It does not
allocate a second linear screen.

The captured live host-aperture entry is an especially useful concrete
witness:

```
MMIO 0x0010..0x001c = {
    0x10040000,             /* valid aperture address: BAR alias +0x40000 */
    0x00000000,             /* local-VRAM surface base */
    0x30030780,             /* fmt 0x30, type 3, width 0x780 = 1920 */
    0x00010708              /* depth 1, height 0x708 = 1800 */
}
```

Host descriptors are 16 bytes each at MMIO `slot * 0x10`; the bank has twenty
entries (`0x0000..0x013f`).  Dword 0 bit 28 publishes the entry, dword 1 is the
GPU surface byte address, dword 2 is the normal surface control, and dword 3
holds height `[13:0]` and depth `[27:16]`.  Linux X
`EmulMmSurfaceMapper::AllocateApertureBlock` at `0x74c40` writes dwords 1--3
before publishing dword 0.  Its aperture allocator uses 256-KiB units and an
observed extra 256-KiB guard/gap.

`MmSurfDesc::GetHostAperturePitch` at `0x745c0` returns

```
pow2ceil(max(width, 32)) * bytes_per_pixel
```

for type 3 (also for the other native view classes in this path).  Thus the
captured 1920-wide 32-bpp primary has a CPU-visible pitch of
`2048 * 4 = 8192` bytes: 7680 bytes of visible pixels and 512 bytes of row
padding.  `GetHostApertureSize` at `0x74670` applies that pitch to the
descriptor height.  The captured entry therefore spans
`8192 * 1800 = 14,745,600` bytes of aperture address space.

One descriptor covers the entire allocation.  In particular, a 1920x1200
screen needs only `8192 * 1200 = 9,830,400` bytes and is wholly contained in
that single 1920x1800 entry.  There is no per-row or per-tile descriptor list.

`MmHostAperture::Lock` at `0x72e20` returns the mapper's CPU BAR pointer plus
the aperture offset, and separately the engine-visible aperture address plus
the same offset.  It increments a lock count; it contains no memcpy, detile,
or allocation-to-allocation resolve.  `MTXScreenInit` forms exactly this BAR
pointer at `0x432da..0x432df` and passes it directly to `fbScreenInit` at
`0x433e0..0x433e7`.  Consequently ordinary fb/Xorg row-major writes pass
through the hardware translation and land in the native primary storage.

Windows uses the same policy.  `MTXPARD.dll` registers
`DirectX.PrimaryLockToUserMemEnable` at `0x109b40..0x109b60` with default zero;
locking the primary into user/system memory is an optional disabled fallback,
not the normal framebuffer.  Its normal local primary is exposed through the
same four-dword host-aperture scheme.

The type-2 3-D view and type-3 2-D/display view have the same physical size,
alignment, and host-aperture pitch.  X driver
`SurfDesc2dList::AssignCurrentSurfDescTo` at `0x7ba20` re-emits one bitmap as
type 2 for 3-D or its stored type 3 for 2-D, without copying any pixels.  Live
captures show the same 1920-wide storage as `0x32020780` for the 3-D view and
`0x30030780` for the 2-D/display view.  The view switch is not a format
resolve.

## 2. Exact GL swap selection and payloads

The Linux ICD `glXSwapBuffers` at `0x35f410` resolves the drawable entirely
client-side and tail-calls the swap routine at `0x35a4a0`.  That routine:

1. flushes every pending rendering stream for the drawable;
2. emits ICD stream opcode `0x3e` when its front/back render-target state must
   change;
3. tests `[drawable+0x58]`;
4. uses kernel escape `0x10` when that field is zero (windowed BLIT), or swaps
   the device front/back handles and uses escape `0x1c` when it is nonzero
   (page flip);
5. retries when the kernel returns `-4`, its queue-full result.

There is no GLX protocol round trip or `XSync` in this route.  The recovered
payloads are:

```
BLIT, escape 0x10:
  +0x00 u32 flags = 1
  +0x08     context id
  +0x10 u64 source surface handle (back)
  +0x18 u64 destination surface handle (front/primary)
  +0x20     source rectangle pointer
  +0x28     destination rectangle pointer
  +0x40 u32 0xffffffff

FLIP, escape 0x1c:
  +0x00 u64 new-front surface handle
  +0x08 u32 swap interval
```

The ICD owns one device-global full-screen back surface, not a small surface
per window.  Window coordinates select the part of that back surface copied
to the primary.

## 3. Windowed Present clipping and rectangle mapping

Linux kernel escape `mtx986` at `0x50980` resolves and validates both handles,
obtains drawable/visible geometry with `mtx2388` or `mtx3171`, obtains the
region with `mtx2396`, constructs a composition object with `mtx3786`, and
calls `mtx3963` at `0x449d0`.

`mtx3963` iterates the region with `mtx3516`/`mtx710`.  For every visible
intersection it calls `mtx95` at `0x44950` twice so that clipping one rectangle
adjusts the corresponding source or destination rectangle by the same left,
top, width, and height deltas.  It then dispatches the resulting pair.  This
is software clip-list iteration followed by one or more engine rectangles;
the normal Present path does not copy obscured window pixels.

The final unscaled BITBLT record consumed by `mtx1637` at `0x23c20` has the
coordinates that matter to hardware:

```
record +0x04/+0x08  destination x/y
record +0x14/+0x18  source x/y
record +0x1c/+0x20  source end x/y

width  = end_x - source_x
height = end_y - source_y
```

Per-surface logical origins are added when enabled.  The engine submission is
then exactly:

```
0x1220/0x1224 = destination base/config       /* descriptor slot 0 */
0x1230/0x1234 = source base/config            /* descriptor slot 2 */
0x1300        = 0x202 | optional flags        /* BITBLT, src 2, dst 0 */
0x1304        = (source_y << 16) | source_x
0x1308        = (dest_y   << 16) | dest_x
0x138c        = (height   << 16) | width       /* fires */
```

The descriptor low 14 bits carry the surface width/pitch parameter in pixels;
the surface type supplies the native pitch rule.  There is no byte-pitch
argument in the Present packet and no linear staging address.  The primary's
normal 2-D/display descriptor is type 3.  `mtx1637` copies each current
surface's base/config verbatim from object `+0xd8/+0xdc`; it does not synthesize
a new layout or convert the source.  A rendered surface may previously have
been bound through the byte-identical type-2 3-D view and rebound to type 3 for
2-D.  A live window-swap ring capture would be needed to identify which view
value is resident for every individual blit, but this does not change the
allocation or its bytes.

For exact-size compatible surfaces the routing function `mtx2091` at `0x52120`
reaches the direct `mtx3087`/`mtx1637` route.  It normalizes reversed endpoint
pairs at `0x522f8..0x52332`, records X/Y reversal, and compares source and
destination extents.  Size mismatch and special formats take the more general
`mtx3634` route; scaling is implemented there by a DPS textured rectangle
(`mtx1389`, DWGCTL base `0x1000cc`).  A normal window swap has equal source and
destination extents and therefore does not require that scaled route.

### Overlap

Normal window Present copies between distinct back and primary allocations,
so source/destination overlap is impossible.

For a general same-allocation copy, `mtx1637` does contain overlap handling.
At `0x240ef..0x24157` it compares both axes, changes the starting coordinates
to the last row/pixel when needed, and sets DWGCTL `0x2000` for the Y case and
`0x1000` for the X case.  Windows `MTXPARD.dll` independently contains the
same branches at `0x10dbcf..0x10dc28`, followed by the same `SRC`, `DSTXY`, and
`LEN` writes at `0x10defa..0x10df32`.

Those bits still lack a direct hardware-capture validation, and the separate
Linux XAA screen-copy entry point avoids them by copying through a temporary
surface.  They are therefore evidence about the vendor kernel/Windows general
blitter, not a reason to depend on them for Present.  A direct window-swap path
only needs distinct allocations and clipped rectangles.

### Windows packet cross-check

The Windows display DLL's general native BITBLT at `0x10d900` is structurally
the same implementation.  It builds opcode 2/GXcopy state at
`0x10da03..0x10da29`, gets both native surface bases and controls at
`0x10dd42..0x10dd89`, writes them to `0x1220/0x1224` and `0x1230/0x1234` at
`0x10de21..0x10de70`, and emits coordinates and size to
`0x1304/0x1308/0x138c` at `0x10dee4..0x10df32`.  It then emits the familiar
`0x1300 = 0x707`, `0x1374 = 0` synchronization pair.  This independently
confirms that native-to-native BITBLT is the cross-platform vendor primitive.

## 4. Windowed blit retirement is asynchronous

`mtx986` does not call the flip queue, a vblank wait, or the vblank ISR helper.
After queueing the region's engine work, a nonzero returned tag causes it to
append this raw four-dword DMA packet at `0x50b94..0x50bb1`:

```
0x21afddc0
0x00000707               /* 0x1300 DWGCTL = idle/sync value */
0x00000000               /* 0x1374 = 0 */
tag | 1                  /* 0x12bc DWGSYNC */
```

It then returns success.  The normal path does not poll `0x41c`.  This makes
the kernel escape an asynchronous enqueue, while preserving a tag that later
users of the surfaces can wait on.

When a six-entry per-escape tag history is full, `mtx986`
`0x50bff..0x50cd7` reads the retired tag at MMIO `0x41c`, masks it with `~7`,
and checks whether the oldest saved tag can be reused.  If it cannot, the
escape returns `-4`; otherwise it allocates the next nonzero tag by adding 8,
stores it in the circular array at escape object `+0x130`, and continues.
This is queue back-pressure/tag reuse, not a vblank wait.

The direct blitter itself also marks both participating surfaces with a tag.
`mtx1637` allocates tags in multiples of 8 at `0x23e64..0x23e98` and can append
the five-dword packet at `0x23eab..0x23ee0`:

```
0xc0afafc0
0x00000707
tag | 1
tag
saved engine state
```

The ring is kicked by writing its tail to MMIO `0x408`.  Surface hazards can
therefore be checked by tag retirement without waiting after every Present.

## 5. Fullscreen flip queue and vblank synchronization

Kernel escape `mtx646` at `0x50680` resolves the new-front surface and the
`reg3DFlipOnVBlank` policy.  Policy 0 selects immediate latching, policy 1
forces an interval of at least one, and policy values at least 2 honor the
application interval.  It builds a flip record and calls `mtx3795` at
`0x315b0`; no free software entry returns `-4`.

The software queue contains 32 records.  `mtx3795` copies the plane flags,
surface/start value, interval and state into a free record, then calls
`mtx742` at `0x313e0`.  `mtx742` allocates a monotonically increasing nonzero
tag in steps of 8, saves it in the queue record, and emits a render-retirement
packet before kicking MMIO `0x408`.  Its common one-interval form at
`0x31444..0x31473` is:

```
0xc0afddc0
0x00000706
0x00000000
tag | 3
saved engine state
```

The larger form used by another queue mode begins at `0x3149f` and includes
the same tag (`tag | 4`, then `tag | 1`) around context housekeeping.  The key
ordering property is identical: the tag is in the same bus-master stream after
the preceding render commands.

The vblank interrupt calls `mtx4025` at `0x3a590`.  It increments the per-head
vblank counter, reads `0x41c` and masks `~7`, finds eligible queue records with
`mtx2430`, and calls `mtx2073` only when the saved render tag has retired.
Intervals greater than one are counted in this ISR (`vblank_count - base`,
modulo interval at `0x3a6d0..0x3a6e7`); there is no userspace busy wait.

The display primitive uses four hardware START slots:

| Plane | START register bank | Control pending field | Active field |
|---|---:|---:|---:|
| head 0 graphics | `0x1824 + slot*4` | `0x1800[8:7]` | `0x1800[17:16]` |
| head 1 graphics | `0x1a24 + slot*4` | `0x1a00[8:7]` | `0x1a00[17:16]` |
| head 0 second plane | `0x19c8 + slot*4` | `0x19a8[10:9]` | plane-specific |
| head 1 second plane | `0x1bc8 + slot*4` | `0x1ba8[10:9]` | plane-specific |

`mtx3863` at `0x34a60` writes `surface_offset & 0x07fff800` to the selected
START slot.  `mtx2073` selects it in the plane control register.  Control bit
6 requests immediate latching; leaving bit 6 clear latches at retrace.  The
polarity is fixed by policy 0 (never synchronize) setting bit 6.  `mtx416` at
`0x34170` reports a head-0 graphics flip retired when
`((ctrl >> 7) & 3) == ((ctrl >> 16) & 3)`.

Thus fullscreen synchronization has two independent gates:

1. the DMA retirement tag proves rendering into the new front has completed;
2. the vblank ISR selects the START slot at the requested retrace interval.

The ICD is double buffered even though hardware supplies four START slots.
Windows independently exposes the same bank: `MTXPARD.dll` `0xa9680` derives
the graphics START register from `0x1824`, and `0xa96d0` selects
`0x1824/0x1828/0x182c/0x1830` for slots 0--3.  Its policy constructor at
`0x10b612..0x10b633` registers the `FlipOnVBlank` string with default 3 and
stores the resulting policy object at manager `+0x1a0`.  This agrees with the
Linux default of application-controlled vblank flipping.

## 6. Implementation consequences

For the open stack's fast path:

* Keep the primary as one native scanout/render allocation and map it for the
  CPU through a pinned host-aperture descriptor.  Report the aperture pitch
  (8192 bytes at 1920x32 bpp), not 7680 bytes.
* Do not allocate a private native scanout BO plus a linear shadow and do not
  run a full-frame layout resolve.  The vendor does neither.
* Render through the native type-2 3-D view and use the byte-identical type-3
  2-D/display view for X/scanout.  Changing the descriptor view does not move
  pixels.
* Implement windowed Present as clipped 1:1 BITBLTs from back to primary using
  descriptor slots 2 and 0 and the `SRC`, `DSTXY`, `LEN` packet above.  Carry a
  GPU fence/tag; do not wait synchronously after every blit.
* Do not throttle windowed Present on vblank unless an explicit compositor or
  Present policy asks for it.  The recovered vendor GL window path does not.
* For eligible fullscreen drawables, queue the render tag and perform the START
  slot selection in the vblank ISR.  The display latch and the engine fence
  solve different ordering problems and both are required.

The remaining narrow uncertainty is the exact type value resident in the
source descriptor during every kernel window blit.  The binaries prove that
type 2 and type 3 address the same bytes and that no conversion occurs; a
single live ring capture of a vendor window swap would settle only that
metadata choice.  It does not justify any shadow, detile, composite resolve,
or CPU copy.
