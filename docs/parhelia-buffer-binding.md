# Buffer binding and address resolution — there are no client relocations

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

**This answers an open question that earlier notes recorded as undecoded, and it
retracts a claim.** `docs/parhelia-mesa-driver.md` said the kernel's replay
engine `mtx2153` "patches base-address registers at replay time,
`value = surface_base + entry_offset − entry_delta`". `mtx2153` does no such
thing — it is a fence-and-state emitter, and that arithmetic appears nowhere on
the path. The real mechanism is a **handle lookup at bind time**, and the client
never puts a GPU address in the command stream at all.

## The binding record — key 0x08, 32 bytes

Consumer `mtx2530` (`.text+0x4bf10`), reached from `mtx558`'s 82-entry jump
table at `.rodata+0x1b08`, entry 8. Per item:

    +0x00  u32  slot index, bounds-checked <= 7 (eight bindable slots)
    +0x08  u64  buffer HANDLE - or a direct kernel object pointer, see +0x18
    +0x10  u32  offset; also latched into hwstate[0x118]
    +0x18  u32  0xffffffff selects the direct-pointer path; any other value is a
                second key compared against the slot's shadow

Two paths, and only one crosses a trust boundary:

* `+0x18 != 0xffffffff` — `+0x08` is a **32-bit handle**. The kernel walks a
  red-black tree (a `std::map`) rooted at `hwstate[0x20]+0x78+0x28`, comparing
  the key against `node[0x20]`, and takes `node[0x28]` as the resolved object.
  Then `mtx2220(object, ctxblock, item)`.
* `+0x18 == 0xffffffff` — `+0x08` is taken as a **kernel pointer** and stored
  straight into `hwstate[0x110]`, then `mtx1450`. This path is only safe because
  the vendor's caller is the X server, and it is exactly the hole an open driver
  must not reproduce.

## Where a binding lands

`mtx2220` / `mtx1450` write a per-slot shadow inside the context block at
`hwstate[0x30] + 0x78`:

    +0x2978            dirty bitmask, |= 1 << slot
    +0x2979            second dirty flag, |= 4
    +0x2986 + slot*20  20-byte slot descriptor {kind, objid, offset, flag, ...}
    +0x2a3a + slot*4   per-slot second key
    +0x2a5a + slot*4   per-slot valid word
    +0x2a7a            group flag

Eight slots, from the `cmp $0x7; ja` bound in both functions.

## Where the address is finally produced

Entirely kernel-side, three copies removed from the client:

1. the resolved object carries precomputed base and pitch in
   `surf[0xb28 + i*4]` and `surf[0xb38 + i*4]`, `i` = 0..3;
2. `mtx2372` (`.text+0xcc30`) copies those into the draw context as
   `ctx[0x144 + i*4]` (base) and `ctx[0x154 + i*4]` (pitch);
3. `mtx2954` (`.text+0xf8e0`) emits the register packet:

```
[4][0x8d8c8988][ctx+0x144][ctx+0x154][ctx+0x14c][ctx+0x15c]
      -> 0x1220 base0, 0x1224 pitch0, 0x1230 base1, 0x1234 pitch1
[2][0x21218b8a][ctx+0x148][ctx+0x158]
      -> 0x1228 base2, 0x122c pitch2
```

`mtx2153` additionally overrides the `i = 1` entries of both arrays from a
global and from `ctx[0x14c]`/`ctx[0x15c]` before emitting.

## Textures resolve the same way

`mtx792` (`.text+0x697c5`) emits the per-unit texture block, header byte 0
computed as `((0x200 + unit*4) >> 2) & 0xff`:

```
[0x72717000 | (0x80+unit)]  -> 0x1200+unit*4, 0x11c0, 0x11c4, 0x11c8
[0x76757473]                -> 0x11cc, 0x11d0, 0x11d4, 0x11d8
[0x20797877]                -> 0x11dc, 0x11e0, 0x11e4, pad
```

Values come from a kernel-side per-unit struct (`src[0x00..0x2c]`), so the
texture base at `0x11dc` is `src[0x24]` — again computed by the kernel, never
supplied by the client.

## What this means for the open driver

**Do not design a `drm_parhelia_reloc` array for surfaces or textures.** The
proposal in `parhelia-drm-design.md` §3.4 invented a relocation ABI that the
hardware and the vendor stack do not need. The correct shape is the one the
vendor already uses and that DRM has a standard idiom for:

* the submit ioctl carries a **BO handle array**;
* a bind record names `(slot, bo_handle, offset)`;
* the kernel resolves handles to pinned BOs, computes base and pitch, and
  **emits the base-address register packets itself**;
* client command buffers are then free of addresses entirely, which removes the
  arbitrary-DMA hole rather than validating around it.

The whitelist rows in `parhelia-drm-design.md` §3.4 that read "allowed only with
a matching relocation entry" for indices `0x88`/`0x8a`/`0x8c`/`0x8e` and `0x77`
should read **rejected outright** — those registers are kernel-emitted, exactly
like `0x12bc` and `0x10a0`/`0x10a4`.

## Still open on this path

* ~~The address-space selector in the low bits of the emitted base values.~~
  **SOLVED**, and by two independent readings of the same function. `mtx1132`
  (`.text+0x4c730`) is the address resolver every emitter goes through:

  ```c
  addr = offset + obj[0x18];
  switch (obj[0x20]) {          /* the region's memory type */
      case 1:  return addr | 2;         /* system heap 1 */
      case 2:  return addr | 3;         /* system heap 2 */
      default: return addr;             /* 0 = local framebuffer */
  }
  ```

  **The low two bits of every GPU address are the memory-space tag**, and it is
  the same tag that appears in `SECADDRESS`/`SECEND`'s low bits and in register
  `0x1000`. This also explains the PRIMEND measurement in
  `parhelia-drm-design.md` §10: a ring in DMA32 system memory needs `|3` because
  3 *is* its space tag, not because two independent enables happen to be
  required.
* Which of the eight bind slots is which target. Four colour/depth surfaces are
  visible in the `i = 0..3` arrays; the other four are presumably the texture
  units, but that is inference.
* `surf[0xb48 + i*4]` -> `ctx[0x164 + i*4]` is copied alongside base and pitch
  and its register destination is not identified.
