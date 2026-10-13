# parhelia: Matrox Parhelia open-source driver

Copyright 2026 René Rebe <rene@exactco.de>

A Linux DRM kernel driver and a Mesa Gallium driver for the Matrox
Parhelia P-series (Parhelia-512, Parhelia8x, Parhelia-LX and P690). This
repository holds only the driver sources, with their full development
history, the driver documentation, and the few patches that the
surrounding Linux and Mesa trees need.

## Layout

| Path | Contents | Goes to |
|---|---|---|
| `driver/gpu/drm/parhelia/` | Linux DRM/KMS driver, module `parhelia` | `drivers/gpu/drm/parhelia/` in a Linux 7.2 tree |
| `src/gallium/drivers/parhelia/` | Mesa Gallium driver, `parhelia_dri` | `src/gallium/drivers/parhelia/` in a Mesa 26.2.2 tree |
| `docs/` | Parhelia reverse-engineering and design notes; `gpu-capture-replay-policy.md` | — |
| `linux-*.patch` | Changes to the Linux tree outside the driver directory | applied with `-p1` |
| `mesa-*.patch` | Changes to the Mesa tree outside the driver directory | applied with `-p1`, in name order |

The Matrox M9 code that once lived in these driver directories is not part
of this repository; it is the separate m9x driver.

## Patches

The patches are made against the T2 SDE package trees (Linux 7.2 and Mesa
26.2.2 with the T2 package patches applied). They apply in name order.

- `linux-01-parhelia-integration.patch`: Kconfig and Makefile hooks, plus
  `drivers/gpu/drm/matrox_common/matrox_kms_common.h`, which
  `parhelia_kms.c` includes.
- `mesa-01-parhelia-integration.patch`: registers the driver in meson, the
  pipe loader, and the DRI and DRIL targets.
- `mesa-02-loader-dri3-single-back-buffer-on-p690.patch`: one DRI3 back
  buffer on the P690, whose render-target origins fit only one colour and
  one depth buffer next to the pinned X front.
- `mesa-03` … `mesa-16`: generic Gallium, NIR and Mesa fixes (lowered-IO
  aaline/aapoint/stipple lowering, fog, polygon offset, wide points,
  no-integer gallivm lowering and others). Each keeps its original commit
  message.

## Building

Kernel: copy `driver/gpu/drm/parhelia` to `drivers/gpu/drm/parhelia`, apply
the `linux-*.patch` files, and enable `CONFIG_DRM_PARHELIA=m`. The driver
needs the resident firmware files `parhelia/mtx*.bin`, extracted locally
from the vendor driver, in `/lib/firmware` (and in the initramfs for early
loading).

Mesa: copy `src/gallium/drivers/parhelia` to `src/gallium/drivers/parhelia`,
apply the `mesa-*.patch` files, and configure with
`-Dgallium-drivers=parhelia` (plus any other drivers you need).

## Licences

- Kernel driver: GPL-2.0-only. The UAPI header `parhelia_drm.h` is MIT.
- Mesa driver: mixed, as marked per file. The screen, context and state
  code is MIT. The shader compilers (`parhelia_fs_*`, `parhelia_vs_*`) and
  the command builder (`parhelia_batch.*`, `parhelia_reference.c`) are
  GPL-2.0-only. `reference/`, taken from the xf86-video-parhelia DDX, keeps
  its GPL-2.0 licence.
