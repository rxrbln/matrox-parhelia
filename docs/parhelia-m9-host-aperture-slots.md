<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9148 BAR0 host-aperture descriptor slots

Covers the M9148 (PCI `102b:0540`, BAR0 256 MiB prefetchable, MMIO BAR2 8 KiB).
It gives the encoding of the 20-slot host-aperture bank at MMIO `0x000..0x13f`
and a slot layout that keeps the identity CPU view of VRAM live while the
kernel has a de-tiled surface window open.

Evidence labels:

* **MEASURED**: hardware probe on 2026-09-15, target `192.168.1.56`, with the
  `m9x_drm` module loaded.  The probes are in `tools/m9-aperture-slot-probe.py`
  (modes `coexist`, `extent`, `layout`).  They use traceable data, and every
  run restored all VRAM bytes and slots it touched.
* **STATIC**: disassembly of the vendor `KernelLibraries.o_shipped` (m9xdriver
  1.4.3-80847, `objdump -dr`).
* **CAPTURED**: vendor `m9x_drv.so` xinit MMIO trace
  `captures/m9x-cold-init-20260915-v1/xinit-mmio*.jsonl`.

## 1. Slot encoding

Slot `n` has four dwords at MMIO `16n + {0x0, 0x4, 0x8, 0xc}`.

| dword | meaning |
|-------|---------|
| d0 `+0x0` | BAR0 page range: `start_page \| end_page << 16`, 4 KiB pages, **end inclusive**.  There is no valid bit.  `0x0000ffff` (start > end) is the empty slot. |
| d1 `+0x4` | VRAM (GPU) address shown at BAR0 `start_page << 12` |
| d2 `+0x8` | view descriptor: `0xf0000000` = raw (tiled) bytes 1:1; a surface descriptor (e.g. `0xcc210140`, `0xcc210400`) = hardware de-tiled linear rows of that surface |
| d3 `+0xc` | `layers << 16 \| height` (`0x00010001` for the raw view) |

### STATIC: m9x115 / m9x716

`m9x115` (`0x20620`) republishes all 20 slots, and `m9x716` (`0x212b0`)
publishes one.  Both write `+4`, `+8` and `+0xc` first, then `+0` last.

* `m9x115`: `d1 = [rec+0x34] & ~3`.  Type `[rec+0x34] & 3 == 1` takes a
  translated address instead.  `d2 = [rec+0x38]` and `d3 = [rec+0x3c]`.
  `base = [alloc+0x200] + [[alloc+0x1c8]+0x38]` and `size = [alloc+0x1a0]`.
  Then `d0 = (u16)(base >> 12) | (u16)((base + size - 1) >> 12) << 16`
  (`0x20837..0x20873`).  An absent record writes only `d0 = 0x0000ffff`
  (`0x208cb..0x208d1`).
* `m9x716`: the same field order.  The aperture base comes from argument 4
  (`%rcx`, kept in `%r14`) and the size from `[desc+0x188]`.  It computes
  `d0 = (u16)(r14 >> 12) | ((r14 + size - 1) >> 12) << 16`
  (`0x2148e..0x214ac`).

A 16-bit page number addresses exactly the 256 MiB BAR.  The value
`0xa0000000` that the kernel uses today therefore means pages `0..0xa000`,
i.e. BAR0 `[0, 0xa001000)`, about 160 MiB.  It is not a valid bit and not
32 MiB.

### CAPTURED

During xinit the vendor X driver writes to `0x000..0x13f` only in the cold
self-test.  Slot 0 gets `4=0, 8=f0000000, c=00010001, 0=a0000000` (seq
105552..105555).  Then the value `i` is written to MMIO `4*i` for
`i = 0..0x4f` (seq 105557..105636, caller `m9x_drv.so` `0x2a6173`).  That is a
register scratch pass over the whole bank.  `xinit-mmio-part2.jsonl` has no
slot writes, so no surface window was captured.  Live slots 1..19 read
`d0=0000ffff` with garbage in d1..d3 (MEASURED).

## 2. Decode rules (MEASURED)

1. **The end page is inclusive.**  VRAM holds the traceable pattern
   `0x6b000000 ^ addr`.
   * Slot 0 identity with `d0=bfff0000`: BAR0 `0xbfff000` reads `60fff000`
     (correct).
   * With `d0=c0000000`: BAR0 `0xc000000` reads `67000000` (correct), and
     `0xc001000` does not.
   * With `d0=ffff0000`: `0xc001000=67001000`, `0xfff0000=64ff0000` and
     `0xffffffc=64fffffc`, all correct.  **Identity can cover the whole
     256 MiB BAR.**
2. **Pages outside every slot do not fault and do not read `0xffffffff`.**
   They alias to a recently translated page.  With `d0=a0000000`, BAR0
   `0xa001000`, `0xbfff000`, `0xc000000` and `0xfff0000` all returned
   `64ffff80`, the last word read through the previous window.  Consequence:
   with today's `d0=a0000000`, CPU access to a TTM BO above BAR0 `0xa001000`
   silently reads and writes the wrong memory.
3. **Within d0 but past the descriptor's storage extent, reads return
   `0xffffffff`.**  A 320x240 `cc210140/000100f0` window with d0 end `0xc0ff`
   read `ffffffff` at +0x50000, +0x60000 and +0xff000.  Storage for this
   surface is 0x50000 bytes (320x256, section 3).  This explains the earlier
   finding that "all other BAR0 offsets read `0xffffffff`" while slot 0 held
   a surface.  That slot's d0 `a0000000` covered them, but the surface extent
   did not.
4. **d0 bounds a surface window too.**  With end `0xc00f`, BAR0 `0xc00fffc`
   read pixel (63,51) correctly and `0xc010000` read an alias (`a50e8000`).
   Rows past `height` inside the storage extent read other pixels (row 240
   returned pixel (0,232)), so they are undefined.
5. **Ranges overlap by priority: the lowest slot index wins.**
   * Slot 0 identity `a0000000` plus slot 1 `910f9100` window: BAR0
     `0x9100000` shows identity.
   * The same two swapped: it shows the window (`60ff8000`).
   * Slot 0 invalid, slot 1 identity, slot 2 window: it shows identity.
   * This is why every earlier second-slot attempt failed.  Each tried d0
     (`13001000`, `11000000`, `81000000`, `a1000000`, `01000000`) overlapped
     slot 0 at the page it read.  For example, slot 0 `10000000` covers page
     `0x1000` inclusive.
6. **All slots work, and so does the last page.**  Slot 19 at `ffffe000`
   and slot 1 at `fffffff0` showed their VRAM correctly (`0xffffffc` =
   `67007ffc`).
7. **The window maps linearly.**  BAR0 `(start_page << 12) + x` shows VRAM
   `d1 + x`.  Slot 1 `ffffc000 / d1=0x9000000 / raw` matched a 512 KiB
   pattern exactly, with slot 0 at `7fff0000`.

## 3. De-tiled surface windows (MEASURED)

These windows are writable and readable in linear row order, and the hardware
stores the data tiled.  The pixel tag is `tag | y << 12 | x`.

| descriptor d2 / d3 | window data | raw storage |
|--------------------|-------------|-------------|
| `cc210140 / 000100f0` (320x240 XRGB) | rw exact, 76800 px | all px found in `[0, 0x50000)`; (1,0)@0x4, (8,0)@0x80, (0,1)@0x20, (0,4)@0x300, (319,239)@0x4fefc |
| `cc210400 / 00010300` (1024x768) | rw exact, 786432 px | all px in `[0, 0x300000)`; (7,0)@0x1c, (8,0)@0x80, (0,1)@0x20, (0,4)@0x300, (1023,767)@0x2ff2fc |

The raw order is the 8x4 tile order already known
(`m9-color-surface-tiled`).  Size a window as at least the surface's storage
pages.  A 320x240 surface needs 0x50000, more than `w*h*4`.  Past-extent reads
give `0xffffffff` (rule 3).

## 4. Verified coexisting layout (MEASURED, `layout` probe)

```
slot 0  d1=0x00000000  d2=0xf0000000  d3=0x00010001  d0=0xdfff0000   identity  BAR0 [0, 224 MiB)
slot 1  d1=<surface>   d2=<descriptor> d3=<layers<<16|height>  d0=0xffffe000   window  BAR0 [224, 256 MiB)
```

Publish order is `d0=0`, then d1, d2 and d3, then d0, then read d0 back.
Release by writing `d0=0x0000ffff` to slot 1.

Result with slot 1 = `d1=0x9000000, cc210400, 00010300`:

* The 1024x768 tagged image written at BAR0 `0xe000000` read back exact.
* At the same time a 1 MiB traceable pattern was written and read through
  identity BAR0 `0x9f00000`, and it matched.
* After releasing slot 1, raw VRAM `0x9000000` held all 786432 tags in tiled
  order.
* The run was repeated through the committed tool, with every byte restored.

## 5. Recommended kernel usage

* **Slot 0 = permanent identity, `d0 = 0xdfff0000`**, BAR0 `[0, 224 MiB)`.
  Never retarget slot 0; `m9x_address_cpu_window()` currently does.  Restrict
  the CPU-mappable VRAM range given to `drmm_vram_helper_init()` / TTM to
  those 224 MiB.  The current `d0=0xa0000000` covers only
  `[0, 0xa001000)`, so BOs above about 160 MiB already alias (rule 2).
* **Slot 1 = kernel surface-transfer window, `d0 = 0xffffe000`**, BAR0
  `[224, 256 MiB)`, 32 MiB.
  * Program `d1` = surface VRAM offset, `d2` = the surface's own descriptor,
    `d3 = layers << 16 | height`.
  * Copy through `bar0 + 0xe000000`, then invalidate with `0x0000ffff`.
  * Serialize users with a kernel lock.  Because slot 0 never changes,
    concurrent TTM mmaps and vmaps stay valid.
* Slots 2..19 stay `0x0000ffff`.  Keep them clear of pages `0..0xdfff`: a
  lower slot index would win on overlap.  Slot 0 already takes priority over
  the whole range, but a future layout must not rely on higher slots
  overlapping lower ones.

## 6. Open

* No measurement exists for surfaces whose storage exceeds 32 MiB (for
  example 4096x4096 XRGB).  Neither splitting a tiled surface across slots
  nor using a larger window has been tried.
* It is not measured whether d2 descriptors other than `f0000000`,
  `cc210140` and `cc210400` de-tile (16-bit, small-category, `c8010020`
  FBOs, textures).
* Type-1 (`[rec+0x34] & 3 == 1`) d1 translation in `m9x115`/`m9x716` has not
  been followed.
