Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# P690 local-VRAM vertex ring

The P690 class-0 (`0x1000 = 0`) vertex frontend was verified with the known
good 3-dword `0x97` range draw. The production render ABI exposes this
zero-copy route alongside the existing PCIe DMA32 upload path.

`DRM_IOCTL_PARHELIA_CREATE_VERTEX` creates a linear, local-VRAM GEM object for
one render file. The kernel pins it in VRAM for that file's lifetime and
returns only a handle plus a render-node mmap offset. It never returns a GPU
address. The allocation is P690-only, page-aligned, and capped at 1 MiB.

Mesa maps one persistent ring per Gallium context. Before a synchronous draw,
it copies the packed post-VS stream into that mapping and turns each normal
`PARHELIA_RELOC_DRAW` into `PARHELIA_RELOC_DRAW_VRAM`. This removes the old
user-to-kernel copy into the coherent DMA upload. The normal DMA32 path is the
fallback when the capability or allocation is absent.

For every VRAM draw relocation, the kernel verifies the P690 device, the exact
creating render file and vertex BO, the validated three-dword `0x97` packet,
and the complete aligned byte range within both that BO and BAR0. It rejects a
mixed host/VRAM submit. It changes only the final recovered `0x1000 = 3` bind
to class 0, leaving all loader state untouched.

The synchronous fence completes before Mesa reuses the ring. `postclose`
unpins and releases it. A timed-out engine retains it until driver teardown,
matching the existing quarantine policy for every potentially active BO.

`src/parhelia3d/p3d_vram_vertex_probe.c` is a direct render-node diagnostic.
It creates one colour target and one `CREATE_VERTEX` BO, submits a known
five-float vertex triangle, and checks the full tiled result after the fence:

```
make -C xf86-video-parhelia/src/parhelia3d p3d_vram_vertex_probe
./xf86-video-parhelia/src/parhelia3d/p3d_vram_vertex_probe \
  --execute /dev/dri/renderD128
```

This does not imply a PCIe or AGP GART. P690's host route remains raw 32-bit
PCIe DMA class 3 and its local-VRAM route is class 0; no GART translation is
invented or exposed.
