# glxgears on m9x under X11 (non-glamor DRI3/Present)

Direct-rendering GLX clients render on the clean-room Mesa `m9x` driver
into tiled VRAM back buffers.  The X server (modesetting from
`xorg-server-parhelia`, no glamor) imports them GPU-only through DRI3 and
de-tiles every Present copy rectangle into the CPU-mapped window pixmap with
`DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS`.  The kernel publishes the damaged
front through DIRTYFB into the tiled scanout.  No software rasterizer
produces client frames; the server's swrast GLX provider only supplies GLX
visuals and indirect contexts.

Labels: **MEASURED** on the target (192.168.1.56), **STATIC** from source.

## Components

* X server: `xorg-server-parhelia` 21.1.24, `build-present-copy/`
  (autotools, `x86_64-t2-linux-gnu-gcc`), modesetting changes:
  * `m9x_drm_uapi.h`: GET_BO_INFO / SURFACE_ACCESS subset of the kernel UAPI.
  * `driver.c`: `ms->m9x` from `drmGetVersion`; DRI3 `pixmap_from_fds` keeps
    `PARHELIA_M9_LAYOUT_TILED` imports GPU-only (stride must be
    `align(width, 32) * 4`); Parhelia GET_PARAM probe skipped on m9x.
  * `present.c`: `ms_m9x_present_copy` reads each clipped update box of the
    tiled pixmap (`cc210000|align(w,32)` / `00010000|h`) straight into the
    window pixmap, then `DamageDamageRegion`.
* Mesa `m9x` (`m9_screen.c`):
  * SCANOUT/SHARED colour images of any size up to 2048, B8G8R8X8 or
    B8G8R8A8 (DRI3 back buffers are window-sized).
  * PIPE_BUFFER accepts `PIPE_BIND_VERTEX_STATE` (display-list storage,
    `mesa/main/bufferobj.c:328`).
  * Gallium buffers are heap memory, not kernel BOs: only the CPU draw module
    reads them, and a 1 MiB contiguous DMA32 `dma_alloc_wc` failed on the
    target.

## Why not the installed server (STATIC)

A modesetting module built from this tree reports video ABI 25.2 like the
installed 1.21.1.22 server, so it would load.  The Present `copy` hook is a
local addition to the server core (`present/present.h:126-145`,
`present/present_execute.c:82`).  The stock core never calls it and would
run `present_copy_region` on a GPU-only pixmap with no CPU storage.  The
tree's own `Xorg` binary therefore runs from a private prefix.

## Build and stage (host)

```sh
cd xorg-server-parhelia/build-present-copy/hw/xfree86/drivers/modesetting && make
/home/ai/tmp/m9x-ninja.sh
tools/m9-stage-mesa-runtime.sh <mesa>/objs-m9x ~/tmp/m9x-stage
```

Private X prefix `/root/parhelia-m9x-xorg` on the target: `bin/Xorg` from
`build-present-copy/hw/xfree86/Xorg`, `modules/` with
`drivers/modesetting_drv.so`, `extensions/libglx.so` and `libshadow`,
`libshadowfb`, `libwfb`, `libexa`, `libfbdevhw`, `libvgahw`, `libint10`,
`libglamoregl` from the same build dir (all stripped).  `lib/` carries
`libunwind.so.8` and `libmd.so.0`, which the build host links but the
target lacks (MEASURED).  Mesa goes to `/root/parhelia-m9x-mesa/{dri,gbm,lib}`
by temp name + `mv`.  Tools go to `/root/parhelia-m9x-tools`:
`xorg-m9x-dri3.conf`, `m9x-x-glxgears.sh`, `m9-x-scanout-compare`.

## Run (target)

```sh
cd /root/parhelia-m9x-tools
gcc -O2 -I/usr/X11/include -o m9-x-scanout-compare m9-x-scanout-compare.c \
    -L/usr/X11/lib64 -Wl,-rpath,/usr/X11/lib64 -lX11
flock -w 1200 /tmp/m9-hw.lock ./m9x-x-glxgears.sh
```

The script starts

```sh
LD_LIBRARY_PATH=/root/parhelia-m9x-xorg/lib /root/parhelia-m9x-xorg/bin/Xorg :1 \
    -nolisten tcp -noreset \
    -modulepath /root/parhelia-m9x-xorg/modules,/usr/X11R7/lib64/xorg/modules \
    -config /root/parhelia-m9x-tools/xorg-m9x-dri3.conf
```

and runs the clients with

```sh
DISPLAY=:1 LIBGL_DRIVERS_PATH=/root/parhelia-m9x-mesa/dri \
LD_LIBRARY_PATH=/root/parhelia-m9x-mesa/lib:/root/parhelia-m9x-mesa/dri \
__GLX_VENDOR_LIBRARY_NAME=mesa glxgears -info -geometry 300x300+100+100
```

It records libgallium/m9x_dri/libGLX_mesa from `/proc/<pid>/maps`, stops
glxgears with SIGSTOP after `M9X_SECONDS` (default 20), runs
`m9-x-scanout-compare :1 300x300 100 100`, then stops glxgears and X.
Output: `/root/m9x-glxgears/{Xorg.log,glxinfo.txt,glxgears.txt,glxgears-maps.txt,compare.txt}`.
The compare tool programs host-aperture slot 1 itself, so it runs only
while glxgears is stopped and X is idle.

## Results

MEASURED 2026-09-15 (module sha de704649…, standard parameters):

* X log: "Initialized non-glamor DRI3 PRIME presentation.", Present, DRI3
  and GLX extensions, "GLX: Initialized DRISWRAST GL provider for screen 0".
  No `(EE)` lines and no "m9x Present" errors.
* `glxinfo -B`: "direct rendering: Yes", "OpenGL renderer string: Matrox M9
  (clean-room bring-up)", "OpenGL version string: 1.5 Mesa 26.2.2".
* glxgears maps `/root/parhelia-m9x-mesa/dri/libgallium-26.2.2.so` and
  `/root/parhelia-m9x-mesa/lib/libGLX_mesa.so.0`, synchronized to vblank.
  It prints 1.47-1.95 FPS in a 300x300 window.
* Stopped on a presented frame: root window vs de-tiled scanout
  (DC_START 0x1422000, 1024x768) has 0 mismatches.  The 300x300+100+100
  scanout region has 25630 pixels differing from the black clear colour:
  17227 red-dominant, 4763 green-dominant, 3640 blue-dominant (the three
  gears).
* Earlier failure, fixed by the two buffer changes above: glxgears
  segfaulted in `_mesa_bufferobj_subdata` (`save_Normal3f` ->
  `vbo_save_SaveFlushVertices` -> `compile_vertex_list`).  First the
  VERTEX_STATE bind was rejected.  Then `CREATE_BO` returned ENOMEM for the
  1 MiB DMA32 BO: target MemFree was 13.6 MiB, with no free DMA32 block
  above order 6.

## Gaps

* About 1.5 FPS.  Where the frame time goes (CPU T&L, two-pass submits,
  SURFACE_ACCESS reads, full-frame DIRTYFB publish) is not yet measured.
* Every presented frame costs a de-tile read plus a 3 MiB publish, because
  the kernel ignores DIRTYFB clips.
* Surfaces of 64x64 or smaller are refused by SURFACE_ACCESS; such windows
  drop frames (logged once).
* Redirected (composited) windows are de-tiled into their window pixmap
  but untested.

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
