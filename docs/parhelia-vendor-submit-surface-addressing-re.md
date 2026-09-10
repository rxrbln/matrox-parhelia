# Vendor surface descriptors and engine-address selection

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This note records clean-room static analysis of the saved Matrox Linux and
Windows binaries.  It answers one narrow implementation question: when a P690
VRAM allocation also has a host-aperture descriptor, do render, depth, texture,
and 2-D Present packets carry the descriptor's logical dword 0 or its physical
surface dword 1?

They carry the normal physical engine address represented by **dword 1**.  The
descriptor is allocated for the separate CPU host-aperture mapping.  Its
logical dword 0 is not substituted into ordinary SURF or TEX packet operands.

This corrects the allocation-to-bind conclusion in
`parhelia-p690-texture-upload-aperture-alias.md`: that note conflated the
`MmBlock` allocated for a surface with the distinct `MmBlock` owned by its
`MmHostAperture`.  The binary has both objects at the same time.

## Sources

Offsets below are image virtual addresses reported by `objdump -d -M intel`.

| Binary | SHA-256 |
|---|---|
| `matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so` | `8f5ca1d85f1c13fda731c8a0ba28db063ec0e697b7bc5dfff300c1500d564e4f` |
| `matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o` | `5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47` |
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |
| `xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll` | `52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a` |

No vendor driver was run and no target state was changed for this analysis.

## 1. The two addresses have different jobs

The twenty entries at MMIO `0x0000..0x013f` are 16-byte host-aperture
descriptors:

| Dword | Meaning |
|---|---|
| `d0`, `+0x00` | bit 28 valid/publication, plus logical host-aperture coordinate and cached BAR selector bits |
| `d1`, `+0x04` | target allocation's normal engine byte address, including memory-class bits `[1:0]` |
| `d2`, `+0x08` | universal surface control: width `[13:0]`, type `[18:16]`, format `[29:24]` |
| `d3`, `+0x0c` | universal dimensions: height `[13:0]`, depth/slices `[27:16]` |

The mapping is conceptually:

```
CPU access at BAR aperture coordinate (d0 without valid bit)
                 |
                 v
       { d1 target, d2 layout, d3 dimensions }
                 |
                 v
       native allocation in VRAM/system memory
```

The graphics engines already understand `d1`, `d2`, and `d3`.  Their ordinary
surface packets therefore use the target directly.  Feeding `d0` into SURF or
TEX adds an unintended second translation and addresses the wrong storage.

`d1` should be called the physical **engine** address, rather than necessarily
a linear BAR offset.  On P690 bank B, the engine's local-memory coordinate can
differ from the CPU's contiguous BAR offset.  The required value is the same
bank-encoded address that the allocation would use without a host descriptor.

## 2. Allocation: exact fields, order, and slot sorting

Linux X `EmulMmSurfaceMapper::AllocateApertureBlock` is `mtx862` at
`mtx_drv.so:0x74c40`.

### 2.1 Logical-coordinate allocation

At `0x74c9c` it loads mapper high-water state `mapper+0x15c`.  It calls
`MmSurfDesc::GetHostApertureSize` at `0x74cb6`, adds the size at `0x74cbb`, and
then executes:

```
74ce2: add eax, 0x7ffff
74ce7: and eax, 0xfffc0000
```

This is:

```c
start = old_high_water;
new_high_water = ALIGN(old_high_water + mapping_size + 0x40000, 0x40000);
```

The allocator therefore uses 256-KiB granularity and leaves one extra
256-KiB guard.  At `0x74cc9` it ORs the cached BAR selector at `mapper+0x160`
into the logical key.  At `0x74cec` it adds valid bit `0x10000000` for d0.
Neither operation changes d1.

### 2.2 Sorted, dense insertion

At `0x74d05..0x74d2f` the allocator walks live software records in 16-byte
steps, comparing their d0 keys, and chooses the first record greater than the
new key.  `0x74d37` onward shifts the suffix one slot upward.  Thus live
descriptors form a dense prefix sorted in ascending d0 order; new mappings
must not simply be appended.

At `0x75214` the insertion index is compared with `0x13`, establishing slots
0 through 19.  A driver that reserves bootstrap/root/cursor entries has fewer
than twenty slots available for transient mappings.

### 2.3 Descriptor contents and publication

At `0x7524e..0x7526d` the allocator saves target, control, dimensions, and the
prepared logical key into the selected 16-byte software record.  Its MMIO
writer programs d1, d2, and d3 first (the corresponding write sites are
`0x75344..0x75351`, `0x75456..0x75464`, and `0x75564..0x75572`).  It publishes
d0 last at `0x75675..0x75683`.

This is the safe update order:

```c
write(slot + 0x04, d1);
write(slot + 0x08, d2);
write(slot + 0x0c, d3);
write(slot + 0x00, d0 | BIT(28));
```

`d2` and `d3` are not generic placeholders.  They are the exact universal
surface description for the CPU view being mapped.  For example, the captured
1920-wide primary mapping is:

```
d0 = 0x10040000
d1 = 0x00000000
d2 = 0x30030780    /* format 0x30, type 3, width 1920 */
d3 = 0x00010708    /* one slice, height 1800 */
```

Mapped colour, depth, and texture allocations likewise retain their actual
format, type, width, height, and depth.  A depth mapping must use the depth
format/control, not the colour target's control.  A texture mapping must use
the texture layout/control and dimensions.  These fields describe how the CPU
aperture translates the mapped storage; they do not change which base the
render/depth/texture packet uses.

## 3. The host-aperture block is separate from the surface block

This object split resolves the apparent contradiction in earlier notes.

`MmSurfaceMapper::AllocateAperture`, `mtx_drv.so:0x74a60`, passes the embedded
host-aperture descriptor at `MmHostAperture+0x10` to the virtual allocator at
`0x74abb..0x74ac3`.  It then stores the returned aperture `MmBlock *` in
`MmHostAperture+0x40` at `0x74adb`, and stores its descriptor slot at `+0x48`
at `0x74ae5`.

By contrast, `MmSurface::AttachToBlock`, `mtx_drv.so:0x73710`, stores its input
normal allocation block at `MmSurface+0x78` (`0x7371c`) and reads that block's
engine address from `MmBlock+0x20` (`0x73720`).  It applies the memory class:

```
memory types 0, 1, 6, 7 -> class 0
memory type 2           -> class 2
memory type 3           -> class 3
```

The result is saved as the ordinary surface engine base at `MmSurface+0x90`
(`0x73733..0x73736`).

`MmSurface::DetachFromBlock`, `mtx_drv.so:0x738e0`, proves both associations
coexist.  It clears the normal block at `surface+0x78` and engine fields at
`+0x88/+0x90` (`0x738eb..0x73905`), then independently loads the
`MmHostAperture *` from `surface+0xa0` (`0x738e1`) and releases it at
`0x7390d..0x7391c`.

The aperture allocator does store its unmodified logical start in its own
returned `MmBlock+0x20` at `0x756db..0x756f8`.  That fact does not make the
aperture block the surface's normal attached allocation.  It belongs to the
separate object at `surface+0xa0`.

## 4. Lifetime follows CPU mapping, not command submission

`MmHostAperture::Lock`, `mtx_drv.so:0x72e20`, behaves as follows:

* if `host_aperture+0x40` already contains a block, `0x72e48` increments the
  lock count at `+0x38`;
* `0x72e57..0x72e62` returns the block's logical aperture coordinate
  (`block+0x10`) plus the requested offset at `host_aperture+0x70`;
* `0x72e6b..0x72e76` separately returns the CPU pointer (`block+0x18`) plus
  the same offset;
* if no block exists, `0x72e90..0x72e96` calls
  `MmSurfaceMapper::AllocateAperture`.

`MmHostAperture::Unlock`, `mtx_drv.so:0x72ec0`, decrements the lock count.  At
zero it marks the aperture releasable.  The `+0x48 == -2` policy path calls
`ReleaseAperture` at `0x72ee0..0x72eec`; other policies retain a cached
mapping.  Surface teardown also releases the separate host aperture through
the `MmSurface::DetachFromBlock` path above.

`EmulMmSurfaceMapper::FreeApertureBlock`, `mtx_drv.so:0x75a00`, reverses the
same allocation quantum at `0x75a16..0x75a40`, locates the entry by its d0 key
at `0x75a8b..0x75aef`, and compacts every later 16-byte entry downward starting
at `0x75b1f`.  It clears the vacated tail d0 at `0x76034..0x76040` and
decrements the live count at `0x76076`.

There is no submit fence in this lifetime chain.  Vendor host descriptors are
created for a CPU aperture lock/mapping and may be cached for the surface's
lifetime.  Holding an open-driver descriptor until a fence retires is needed
only if a submitted engine command intentionally consumes its logical d0.  The
ordinary surface paths below do not.

## 5. Render colour and depth use the physical base

The Linux kernel HAL has one explicit engine-address resolver, `mtx1132` at
`mtx_parhelia.o:0x4c730`:

```asm
4c730  mov edx, [rdi+0x20]      ; allocation region/memory type
4c733  mov eax, esi             ; byte offset
4c735  add rax, [rdi+0x18]      ; physical engine allocation base
4c739  cmp edx, 1
4c745  or  rax, 2               ; system/AGP class
4c73e  cmp edx, 2
4c750  or  rax, 3               ; system/PCI class
```

It never reads the host-aperture mapper, descriptor slot, or logical d0.
`mtx1450` begins a normal bind by zeroing the byte offset at `0x4c76b` and
calling this resolver at `0x4c774`.  Another binding route in `mtx631` loads
the allocation object and byte offset at `0x49c49..0x49c54`, calls the same
resolver at `0x49c57`, and saves the returned engine address at
`0x49c79..0x49c82`.

The colour/depth emission chain preserves that value:

* `mtx2372`, `mtx_parhelia.o:0xcc30`, copies four bases from drawable state
  `+0xb28` to draw-context state `+0x144`, controls from `+0xb38` to `+0x154`,
  and dimensions from `+0xb48` to `+0x164` at `0xccc0..0xccea`.
* `mtx2954`, `mtx_parhelia.o:0xf8e0`, emits tag `0x8d8c8988` at `0xf96c..0xf972`.
  It copies context `+0x144/+0x154` to colour base/control registers
  `0x1220/0x1224` at `0xf97c..0xf993`, and context `+0x14c/+0x15c` to depth
  base/control registers `0x1230/0x1234` at `0xf996..0xf9a9`.

Thus colour and depth bases follow `mtx1132`: physical allocation base plus
offset and class bits.  Giving either register descriptor d0 is contrary to
the vendor bind and emit paths.

The dimensions copied by `mtx2372` remain software state on this emission
path; the colour/depth register packet contains base and control.  A host
descriptor, when one is needed for CPU access, still needs dimensions because
the aperture translator consumes d3.

## 6. Texture sampling uses the physical base

`mtx792`, `mtx_parhelia.o:0x69790`, emits the per-unit texture register window.
At `0x69836..0x69857` it writes the packet:

```
tag 0x20797877
src+0x24 -> 0x11dc  TEX base
src+0x28 -> 0x11e0  surface control
src+0x2c -> 0x11e4  dimensions
0         -> pad
```

The dirty-state emitter `mtx1381` independently emits the same texture surface
triple.  Starting at `0x3f76b`, each unit is a 0x38-byte state record.  The
surface packet at `0x3f7b9..0x3f7e7` emits unit control followed by state
`+0x2c`, `+0x30`, and `+0x34`, which are `0x11dc`, `0x11e0`, and `0x11e4`.

The texture base is kernel-produced binding state, and its allocation path
uses the same physical resolver described in section 5.  Neither texture
emitter consults descriptor d0.  Host-aperture allocation is useful while the
CPU uploads or reads texture storage, but the sampler continues to receive d1.

## 7. 2-D Present uses the physical base

Linux X `Surface2d::Enable`, `mtx_drv.so:0x7bf00`, loads the normal surface
engine address from `surface+0x90` at `0x7bf35` and stores it as the active
bitmap base at `bitmap+0xe0` at `0x7bf3b`.  It may OR class 3 for a non-local
surface at `0x7bf50..0x7bf6c`.  It does not load `surface+0xa0`, its
`MmHostAperture`, or the aperture block's logical coordinate.

`DeviceBitmap::ProgSurfDesc`, `mtx_drv.so:0x7c350`, then emits:

```
7c39d  mov esi, [bitmap+0xe0]   ; base
7c3a3  mov [packet+base], esi
7c3a6  mov ebx, [bitmap+0xc4]   ; control
7c3ac  mov [packet+ctrl], ebx
```

This is the direct path used by the native-surface 2-D blit underlying
windowed Present.

The saved Windows Parhelia driver provides an independent cross-check in its
BITBLT implementation at `MTXPARD.dll:0x10d900`:

* `0x10dd42..0x10dd60` forms the destination from the surface allocation base
  plus origin and saves its control;
* `0x10dd66..0x10dd89` does the same for the source;
* `0x10de21..0x10de70` writes destination base/control to `0x1220/0x1224` and
  source base/control to `0x1230/0x1234`;
* `0x10defa`, `0x10df1f`, and `0x10df32` emit source position, destination
  position, and length before starting the blit at `0x10df43..0x10df4e`.

There is no host-descriptor lookup between surface allocation and those base
register writes.  Present source and destination operands therefore remain
their physical engine addresses even if either allocation also has a CPU
host-aperture descriptor.

## 8. Windows P690 corroboration

The P-series Windows driver `MtxPser/mtxpserd.dll` has a separate aperture
capacity decision at `0x8fae7..0x8fb0f`.  It takes the smaller policy/hardware
capacity and framebuffer size, applies a default 128-MiB ceiling when VRAM is
larger (`0x8faf3..0x8fb01`), and rounds down with `& 0xfffc0000` at `0x8fb07`.

This independently supports a 256-KiB-granular virtual host-aperture space
whose capacity is managed separately from surface allocation.  It does not
turn that aperture coordinate into the allocation's normal engine address.

## 9. Guidance for `submit_dynamic_va`

The implementation should preserve these invariants:

1. `parhelia_vram_gpu_addr()` and every SURF/TEX relocation must return the
   allocation's normal bank-encoded physical engine address, irrespective of
   whether the BO has a host-aperture descriptor.
2. A descriptor mapping records `d0 = logical | valid`, `d1 = physical engine
   surface`, and the exact `d2/d3` for the mapped surface view.  These fields
   must never be swapped.
3. Keep live entries in ascending d0 order in a dense prefix.  Shift all four
   dwords when inserting or removing; program d1/d2/d3 before publishing d0.
4. Reserve long-lived bootstrap, root, and cursor CPU mappings.  The hardware
   has twenty total slots, so descriptors should not be acquired for every
   submitted render, depth, texture, or Present BO.
5. Tie transient descriptor lifetime to actual CPU aperture access or retained
   VMA/cache policy.  A submit fence alone is neither the vendor allocation
   trigger nor its release condition for these surface classes.
6. Retargeting d1/d2/d3 under a live d0 must be serialized against all CPU
   users of the old mapping.  Ordinary packet users do not require retargeting
   because they address their physical allocations directly.

A descriptor may intentionally supply a logical address to a specialized
host-data path, such as the vendor's host vertex mechanisms.  That does not
generalize to render colour, depth, texture sampling, or 2-D Present.  Those
four paths are explicitly physical in the saved drivers.
