# M9148 presentation path: m9x GL output to the screen under X11

This note works out how frames rendered by the clean-room Mesa `m9x` Gallium
driver reach the monitor on the clean-room `m9x_drm` kernel driver: first for
a direct-rendering `glxgears`, later for glamor.  It lists the stack as it is
today, compares four presentation routes, recommends an order, and specifies
the kernel surface-access ioctl those routes need.

Labels: **[SRC]** read in source (file:line), **[LOG]** read on the target
(192.168.1.56, read-only inspection on 2026-09-15), **[DOC]** stated by an
existing measured document, **[INF]** inference from the cited facts,
**[OPEN]** not known and needing a measurement or decision.  Size estimates
are estimates.

Trees referenced:

* kernel `linux-7.2/drivers/gpu/drm/m9x/` (abbreviated `m9x/`)
* Mesa `mesa-26.2.2/src/` (abbreviated `mesa/`), driver `gallium/drivers/m9x/`
* X server `xorg-server-parhelia/hw/xfree86/drivers/modesetting/` (`ms/`)
* DDX `xf86-video-parhelia/src/`

## 1. Facts about the current stack

### 1.1 Kernel scanout

* KMS allows only the warm 1024x768 mode, XRGB8888, pitch 4096, offset 0,
  VRAM GEM object [SRC `m9x/m9_kms.c:220-251` `m9_pipe_check`,
  `m9x/m9_kms.c:463-488` `m9_fb_create` (also requires size >= `0x301000`)].
* The display controller only ever fetches a pinned kernel-owned scanout BO
  (VRAM `0x1422000` on the target) [SRC `m9x/m9_kms.c:575-605`; LOG dmesg
  "M9 tiled scanout surface at VRAM 0x1422000"].
* `m9_scanout_publish` vmaps the framebuffer BO through its *raw* TTM mapping.
  It copies all `0x300000` bytes into a kvmalloc bounce buffer, takes
  `transport.lock`, retargets host-aperture slot 0 at the scanout BO with
  `cc210400/00010300`, runs `memcpy_toio` over a 3 MiB WC ioremap of BAR0,
  restores the identity window and unlocks [SRC `m9x/m9_kms.c:258-292`,
  ioremap at `m9x/m9_kms.c:580-582`].  It runs on every plane update
  [SRC `:381-386`] and on every `DIRTYFB` [SRC `:388-401`].  The DIRTYFB
  clip rectangles are ignored, so every call copies the full frame.
* So the kernel assumes that every framebuffer's raw storage is **linear**.
  That is true for dumb BOs written by the CPU (modetest, Xorg with
  `AccelMethod none`), which were verified on scanout [DOC `m9x/m9-bringup.rst`
  section "Tiled KMS scanout"].
* Slot 0 is reprogrammed by `m9x_address_cpu_window` (MMIO `0x000..0x00c`,
  valid bit `0xa0000000`).  The identity window is `d1=0, d2=f0000000,
  d3=00010001` [SRC `m9x/m9_address.c:37-40, 199-215`].  Slot 0 maps 32 MiB
  of VRAM at BAR0 offset 0 [DOC `m9x/m9-bringup.rst` "Address layout"; memory
  note measured 2026-09-15].
* UAPI has exactly three driver ioctls: `CREATE_BO` 0x09, `SUBMIT` 0x0a and
  `GET_BO_INFO` 0x0b [SRC `m9x/m9x_drv.c:28-35`, `m9x/m9x_drm.h:8-21`].
  There is **no** readback, write-through-window, copy or present ioctl.
  VRAM BO mmap is the raw TTM BAR0 mapping [SRC `m9x/m9_gem.c:62-68, 161-194`].
* The driver exposes `DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET |
  DRIVER_ATOMIC` with a vblank IRQ [SRC `m9x/m9x_drv.c:53-60, 177-192`].
* Existing lock nesting is COLOR RESV -> TRANSPORT
  [SRC `m9x/m9_submit.c:395-399`].  User submits pin every referenced VRAM BO
  [SRC `m9x/m9_submit.c:778-813`].

### 1.2 Render targets are tiled

* GPU render targets are stored as an opaque tiling of 8x4 pixel blocks.  A
  slot carrying the surface's own descriptor makes BAR0 show linear top-down
  rows [DOC `m9x/m9-bringup.rst` "First-triangle status"; the 320x240 target
  uses `cc210140/000100f0`].
* Descriptor word 0 is `format(31:25) | bit23 | bit22 | bit21 |
  layout(18:16) | row_span(15:0)`.  Word 1 is `layers(27:16) | height(15:0)`
  [DOC `parhelia-m9-render-surfaces-clear.md`; SRC Mesa
  `gallium/drivers/m9x/m9_batch.c:163-177`].
* The captured window render target `cc210080` (format `0x66`, span 128,
  layout 1, bit21) and `DC0_PITCH=cc210400` belong to the same `cc21` family.
  The display controller is programmed with the same descriptor word that
  names a 1024-span render target [DOC
  `parhelia-m9-vram-render-target-join.md` "Linear color descriptor shared
  with scanout"].  An FBO target `c8010020` (bit21 clear) is a different
  layout [same DOC].
* The kernel present diagnostic copies a 320x240 target into a 1024-wide
  layout-0x21 surface with the vendor texture-rectangle stream: source view
  `f0210140/000100f0`, destination `f0210400`, rectangle `0x00f00140`
  [SRC `m9x/m9_triangle.c:1555-1622`].  This is a GPU copy.  The rectangle
  and descriptors are fixed to the captured sizes.

**[INF]** A GPU-rendered 1024x768 XRGB target has the descriptor
`cc210400/00010300`, identical to the scanout surface.  The kernel's current
publish therefore reads *tiled* bytes as if they were linear and tiles them
again: a GPU-rendered framebuffer would scan out scrambled.  If tiling depends
only on the descriptor, such a BO could also be scanned out directly with no
copy at all.  That is **[OPEN]** until measured (section 5, Q1).

### 1.3 Mesa m9x driver

* Scanout and display-target resources are allowed only for exactly
  1024x768 B8G8R8X8.  Every other template with `PIPE_BIND_SCANOUT` or
  `DISPLAY_TARGET` fails [SRC `m9x/m9_screen.c:59-75, 270-272`].
* Resources are VRAM BOs mapped raw with `mmap` [SRC `m9_screen.c:310-330`].
  `transfer_map` returns `resource->map + y*row_span*cpp + x*cpp`, i.e. raw
  storage [SRC `m9_context.c:43-87`].  For GPU-written color targets that is
  tiled data **[INF from 1.2]**.
* `flush_frontbuffer` assumes "Scanout is the same linear VRAM BO ... no
  shadow, resolve, or Present-side copy" [SRC `m9_screen.c:688-707`].  That
  predates the kernel's tiled scanout copy (kernel `af8f69b` "drm/m9x: scan out
  through a tiled kernel surface").
* DRI3/PRIME import and export of VRAM BOs exist, with stride
  `row_span*cpp`, modifier LINEAR, and caps `dmabuf = IMPORT|EXPORT`
  [SRC `m9_screen.c:352-554, 760`].
* Context creation returns NULL while the executable-submit or
  linked-program gate is closed [DOC Mesa `m9x/M9_BRINGUP.md`].  Another agent
  is implementing the context.
* Render layout: `row_span = align(width, 32)`, height quantum 32 (small
  surfaces 8/4), 8-element tail pad, 4 KiB byte quantum; 1024x768 gives
  `0x301000` [SRC `m9_internal.h:104-137`].

### 1.4 Mesa GLX and loader

* Mesa 26.2 has no DRI2 GLX client: `src/glx/` has `dri3_glx.c` and
  `drisw_glx.c` but no `dri2_glx.c` [SRC directory listing].  The patched
  server's `present.c:76-81` comment states the same.  GLX screen creation
  tries DRI3, then drisw, then indirect [SRC `mesa/glx/glxext.c:868-899`].
* DRI3 window back buffers are allocated with `USE_SHARE | USE_SCANOUT |
  USE_BACKBUFFER` [SRC `mesa/gallium/frontends/dri/loader_dri3_helper.c:
  1528-1533`], which becomes `PIPE_BIND_SCANOUT | PIPE_BIND_SHARED` [SRC
  `frontends/dri/dri2.c:984-987`].  With 1.3, **any `glxgears` window that is
  not exactly 1024x768 fails to allocate its back buffer** [INF].
* drisw probes only software winsys (`pipe_loader_sw_probe_kms` or
  `pipe_loader_sw_probe_dri`).  No DRM hardware driver can be loaded into
  drisw without new frontend code [SRC `frontends/dri/drisw.c:598-620`].

### 1.5 X server on the target and in the tree

* The installed `/usr/X11R7/lib64/xorg/modules/drivers/modesetting_drv.so`
  (May 15) is the stock T2 build.  `strings` finds no Parhelia/M9 text.  The
  server reports "compiled for 1.21.1.22" [LOG].
* The glamor attempt fails with "glamor: Failed to create GL or GLES2
  contexts" [LOG `/root/Xm9-glamor-262.log:75-76`], which matches the NULL
  context gate.  GLX then has no provider: "AIGLX: Screen 0 is not DRI2
  capable", `dlopen /root/m9-runtime/dri/swrast_dri.so` failed, "no usable GL
  providers found for screen 0" [LOG `:122-126`].  The redirected driver
  directory contains only `m9x_dri.so` and `libgallium-26.2.2.so` [LOG].
  `/usr/lib64/dri/swrast_dri.so` exists [LOG].
* `/root/Xm9-dri-gated.log` shows glamor refusing llvmpipe, then a segfault
  at address 0x8 inside stock `modesetting_drv.so` right after
  "Initializing extension DRI2" [LOG `:116-118, 178-196`].  Cause **[OPEN]**.
* Kernel module parameters on the target: `m9_kms=Y m9_kms_scanout=Y
  m9_user_submit=Y m9_triangle_present=N` [LOG sysfs].
* `xorg-server-parhelia` (21.1.24, not a git repository) carries these local
  modesetting changes, written for the original Parhelia kernel ABI:
  * non-glamor DRI3: `open_client` authenticates the primary node,
    `pixmap_from_fds` imports via PRIME and maps with `MAP_DUMB`, and
    `fd_from_pixmap` [SRC `ms/driver.c:87-240`].  Registered when glamor is
    off [SRC `ms/driver.c:2240-2252`].
  * Present `copy` hook `ms_present_copy` [SRC `ms/present.c:82-260`,
    registered `:661`], called from the locally added
    `present/present_execute.c:82-83`.  It issues Parhelia
    `DRM_IOCTL_PARHELIA_GET_BO_INFO` (nr 0x05) and native BITBLT submits
    (`ms/parhelia_submit.h`).
  * native Parhelia DRI2 buffers [SRC `ms/dri2.c:236-330`] (unused now that
    Mesa has no DRI2 GLX client).
  * **[INF]** On m9x, ioctl nr 0x05 is undefined (1.1), so GET_BO_INFO fails
    and `ms_present_copy` returns `handled = TRUE` [SRC `ms/present.c:
    170-179`].  Every Present copy would be dropped silently.  The tree cannot
    be used on M9 unchanged.
  * Built modules exist in `build-present-copy/` (2026-09-12) and three other
    build directories.  None is installed on this target.

### 1.6 xf86-video-parhelia DDX

The DDX is a UMS driver.  It maps BAR0/MMIO itself through pciaccess
[SRC `parhelia_driver.c:687-708`] and drives the M9 engine directly
(`parhelia_m9_ring.c`, `parhelia_m9_exa.c`, `parhelia_m9_xaa.c`).  It
contains no DRM, DRI2 or DRI3 code (`grep -rn "DRI2\|dri2\|drmOpen"` over
`src/` is empty) [SRC].  Its M9 ring and address programming would conflict
with `m9x_drm`, which owns the requester, address windows and slot 0
[SRC `m9x/m9_address.c`, `m9x/m9_requester.c`].  For the original Parhelia,
GL presentation went through the patched modesetting server described in
1.5, not through this DDX [DOC `HANDOVER.md` "Current state";
`parhelia-p690-page-flipping.md`].

### 1.7 Concurrency hazard in the existing slot-0 retarget

While `m9_scanout_publish` has slot 0 retargeted, BAR0 `[0, 32 MiB)` decodes
the scanout surface instead of VRAM `[0, 32 MiB)`.  The scanout BO itself sits
at 20 MiB (1.1), and user VRAM BOs are allocated from the same helper range
(relocations to `0x1725000..0x183f000` in the target dmesg) [LOG].  **[INF]**
Any other process that touches a raw BAR0 mapping of such a BO during the
retarget reads or writes the wrong memory.  The verified X configuration was
single-threaded (Xorg writes, then calls DIRTYFB itself), so nothing
concurrent ran.  GL clients with mapped BOs break that assumption.  How BAR0
above 32 MiB decodes, and whether a different slot can be dedicated to kernel
transfers, is **[OPEN]** (Q2).

## 2. Presentation options without glamor

### (a) xf86-video-parhelia DDX with DRI2

Not viable.  The DDX has no DRI2 at all (1.6), it would fight `m9x_drm` for
the engine, and Mesa 26 has no DRI2 GLX client (1.4).  Adding DRI2 would still
leave no client able to use it.  **Rejected.**

### (b) modesetting + non-glamor DRI3/Present with a kernel copy

Flow: the `glxgears` DRI3 client renders into a tiled VRAM back buffer
(PRIME fd) -> `PresentPixmap` -> the server's `ms_present_copy` M9 branch
asks the kernel to de-tile the damaged rectangles into the linear X front
dumb BO -> `DamageDamageRegion` -> modesetting `drmModeDirtyFB` -> kernel
publish into the tiled scanout.

Required changes:

* **Kernel:** `SURFACE_ACCESS` ioctl (section 4), about 250 lines with
  validation and selftest.  A BO storage-layout flag with a `GET_BO_INFO`
  extension (4.1), about 60 lines.  Honouring DIRTYFB clips in
  `m9_fb_dirty` is optional (full-frame copies today), about 40 lines.
* **Mesa m9x** (driver owner): mark GPU color targets `TILED` at
  `CREATE_BO`.  Accept `SHARED|SCANOUT` color images of any size up to 2048
  as tiled render targets instead of only the 1024x768 presentation template.
  Make `transfer_map`/`unmap` of tiled resources use a staging buffer filled
  and flushed through `SURFACE_ACCESS`, so glReadPixels and CPU fallbacks
  work.  About 150-200 lines.  The context itself is in progress.
* **X server** (`xorg-server-parhelia` modesetting): detect `m9x` with
  `drmGetVersion`.  In `pixmap_from_fds`, keep tiled imports GPU-only
  (`devPrivate.ptr = NULL`, as the Parhelia branch already does at
  `ms/driver.c:159-180`).  In `ms_present_copy`, add an M9 branch that calls
  `SURFACE_ACCESS(READ)` per clip rectangle with `data_ptr` pointing into the
  mapped front dumb BO, then damages the region.  About 150 lines.
  Build against 21.1.24 and install alongside the 1.21.1.22 server; ABI
  compatibility is **[OPEN]** (Q5).
* **GLX server provider:** put `swrast_dri.so` (with a matching
  libgallium) in the server's DRI search path, so the GLX extension exposes
  visuals.  Clients still render directly through DRI3.  This is
  configuration only (1.5).

Cost per frame, full 1024x768: one de-tile read (~3 MiB), one CPU copy into
the front BO, one full publish (~3 MiB write).  A 300x300 gears window reads
about 0.36 MiB but still publishes 3 MiB until clips are honoured.  Bus
throughput is **[OPEN]**.

### (c) drisw-style readback and XPutImage with m9x

Flow: the client renders on m9x, reads the back buffer back
(`SURFACE_ACCESS READ`), then `XPutImage`/MIT-SHM to the server, which runs an
fb copy into the front BO and publishes through DIRTYFB.

Required changes:

* **Kernel:** `SURFACE_ACCESS` (same as b).
* **Mesa:** new frontend mode that loads a DRM hardware screen into drisw.
  That touches `glx/drisw_glx.c` and `frontends/dri/drisw.c` (today probe-sw
  only, 1.4) and pipe-loader.  drisw also adds `PIPE_BIND_DISPLAY_TARGET` to
  every color attachment [SRC `frontends/dri/drisw.c:489-490`], which m9x
  rejects for non-1024x768.  Estimate 300-400 lines outside the driver,
  unlikely to be upstreamable, plus the m9x transfer work from (b).
* **X server:** stock modesetting `AccelMethod none` is enough (already
  verified on scanout), plus the swrast GLX provider.

Four copies per frame and no route to glamor.  **Usable only as a diagnostic
fallback; not recommended.**

### (d) EGL on GBM/KMS without X

Flow: a kmscube-style client on `/dev/dri/card0` renders to a GBM surface ->
`gbm_surface_lock_front_buffer` -> `drmModeAddFB2` (1024x768 XRGB8888, pitch
4096) -> atomic commit or page flip -> `m9_pipe_update`.

* The loader and allocation are proven: `tools/m9-gbm-smoke.c` allocates the
  exact 1024x768 `RENDERING|SCANOUT|FRONT_RENDERING` BO through libgbm and
  `m9x_dri.so`.  The target has `/root/m9-runtime/{dri,gbm,lib}` [SRC, LOG].
* **Kernel:** the publish must handle a tiled framebuffer (4.1).  Either
  (T1) scan out a tiled BO directly (pin, `DC_START` = BO offset, no copy),
  pending Q1, about 40 lines.  Or (T2) read through a source window into the
  bounce buffer and write through the scanout window, using only proven
  mechanisms, about 60 lines.  Plus the layout flag.
* **Mesa m9x:** the `TILED` flag at `CREATE_BO`.  The working context is
  still needed (in progress).
* **Test client:** about 200 lines (GBM device and surface, EGL,
  `eglSwapBuffers`, add FB, flip on vblank events).  No X involved.

This is the shortest route to a GPU-rendered frame on the monitor, and it
exercises exactly the tiled-framebuffer KMS path that glamor needs later.

## 3. Recommendation

1. **First light: option (d).**  Add the BO layout flag and the tiled-FB
   publish (T2 first, since it uses only proven window mechanics).  Measure Q1
   with a coordinate-coded frame (every texel identifies its x,y).  If raw
   tiled storage equals DC fetch order, switch to T1 direct scanout.  Verify
   on the monitor and with a window readback of the scanout BO
   (`tools/m9-scanout-readback.py`), never from logs.
2. **glxgears under X: option (b).**  `SURFACE_ACCESS`, the Mesa transfer
   and DRI3 size relaxation, the M9 branch in the patched modesetting Present
   copy, and swrast as the server GLX provider.  Everything it adds (DRI3
   import, Present hook, layout flag, de-tile transfer) stays in use on the
   glamor path.
3. **Glamor later.**  glamor needs a GL 2.1/GLSL 1.20 or GLES2 context;
   today m9x returns no context and advertises no GLSL [DOC
   `M9_BRINGUP.md`; LOG 1.5].  With glamor, modesetting's front is a GBM BO,
   i.e. a tiled render target.  So scanout must use T1 or T2 on every damage
   (glamor's `drmModeDirtyFB` path).  glamor's CPU fallbacks
   (`prepare_access`, glReadPixels, texture uploads) reach Mesa
   `transfer_map` and therefore `SURFACE_ACCESS`.  DRI3 client Present
   becomes a GPU copy (texture-rectangle stream generalised from
   `m9_triangle_present_locked` to arbitrary rectangles) or a flip of a tiled
   client BO once T1 is proven.  The patched `ms_present_copy` already
   branches on glamor through `gbm_bo_from_pixmap` [SRC `ms/present.c:
   132-145`].

Order of work per component:

| component | step 1 (GBM) | step 2 (X glxgears) | step 3 (glamor) |
|---|---|---|---|
| kernel | BO layout flag, tiled FB publish T2 (then T1) | `SURFACE_ACCESS`, DIRTYFB clips | GPU rectangle copy ioctl, dedicated transfer slot (Q2) |
| Mesa m9x | `TILED` create flag, context (in progress) | tiled `transfer_map` via ioctl, any-size shared back buffers | GLSL/GLES2, blit |
| X server | none | M9 Present copy and import branch, swrast GLX provider | glamor on m9x, flips |
| DDX xf86-video-parhelia | not used | not used | not used |

## 4. Kernel UAPI specification

### 4.1 Storage layout of VRAM BOs

The kernel must know whether a VRAM BO's raw bytes are linear (CPU-written
dumb or staging images) or tiled (GPU render targets).  Nothing can infer it
from the bytes.

```c
/* CREATE_BO: GPU render/scanout-family color storage; implies BO_VRAM. */
#define PARHELIA_M9_BO_TILED		0x00000010u

#define PARHELIA_M9_LAYOUT_LINEAR	0u
#define PARHELIA_M9_LAYOUT_TILED	1u

struct drm_parhelia_m9_get_bo_info {
	__u32 handle;
	__u32 memory_class;	/* out */
	__u64 size;		/* out */
	__u64 mmap_offset;	/* out */
	__u32 layout;		/* out: PARHELIA_M9_LAYOUT_* (appended) */
	__u32 pad;		/* must be zero */
};
```

* The flag is stored in `struct m9x_vram_bo` and is immutable.  Dumb BOs
  (`m9_dumb_create`) are LINEAR.
* `GET_BO_INFO` grows from 24 to 32 bytes.  DRM core zero-extends and
  truncates ioctl arguments, so old userspace keeps working.
* `m9_fb_create` accepts both layouts.  `m9_scanout_publish` keeps its
  current code for LINEAR.  For TILED it uses T2 (window read of the source
  with `cc210400/00010300` into `scanout_bounce`, then the existing scanout
  window write), or T1 once Q1 is closed.
* Raw `mmap` of a TILED BO stays allowed (Mesa maps it today), but its
  contents are opaque.  Linear CPU access goes through 4.2.

### 4.2 `DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS`

```c
#define DRM_PARHELIA_M9_SURFACE_ACCESS	0x0c
#define DRM_IOCTL_PARHELIA_M9_SURFACE_ACCESS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_PARHELIA_M9_SURFACE_ACCESS, \
		 struct drm_parhelia_m9_surface_access)

#define PARHELIA_M9_ACCESS_READ		1u	/* surface -> user, de-tiled */
#define PARHELIA_M9_ACCESS_WRITE	2u	/* user -> surface, tiled */

struct drm_parhelia_m9_surface_access {
	__u32 handle;		/* VRAM GEM, layout TILED */
	__u32 op;		/* exactly one PARHELIA_M9_ACCESS_* */
	__u32 descriptor0;	/* selector-8 word: format|flags|layout|row_span */
	__u32 descriptor1;	/* layers << 16 | height */
	__u32 surface_width;	/* visible width, row_span == align(width, 32) */
	__u32 x;		/* rectangle in surface pixels */
	__u32 y;
	__u32 width;
	__u32 height;
	__u32 data_stride;	/* bytes per row in the user buffer */
	__u64 data_ptr;		/* user buffer, top-down rows */
	__u64 data_size;	/* bytes available at data_ptr */
	__u32 flags;		/* must be zero */
	__u32 pad;		/* must be zero */
};				/* 64 bytes, no implicit padding */
```

`DRM_RENDER_ALLOW`, so Mesa can use it on the render node and the X server on
the primary node.

**Validation**, in order.  Return `-EINVAL` unless stated otherwise.

1. `flags`, `pad` are zero.  `op` is READ or WRITE.  `width`, `height` and
   `surface_width` are nonzero.
2. `descriptor0`: format `(d0 >> 25) == 0x66` (cpp 4; `0x4e`/cpp 2 only
   after Q3).  Layout `(d0 >> 16) & 7 == 1`.  Bits 20:19, 22 and 24 are zero.
   Bit 23 is zero.  Bit 21 is **one** (captured `cc21` family; bit21-clear FBO
   layouts only after Q3).  `row_span = d0 & 0xffff` equals
   `ALIGN(surface_width, 32)` and is at most 2048.
3. `descriptor1`: `layers = (d1 >> 16) & 0xfff == 1`.  `surface_height =
   d1 & 0xffff` is between 1 and 2048.  Bits 31:28 are zero.
4. Rectangle: `x + width <= surface_width`, `y + height <= surface_height`,
   checked with `check_add_overflow`.
5. User buffer: `data_stride >= width * 4`, and
   `(height - 1) * data_stride + width * 4 <= data_size`, overflow-checked.
   `access_ok`.
6. The handle resolves to a VRAM GEM (`m9x_gem_is_vram`, else `-EINVAL`;
   missing gives `-ENOENT`) whose layout is TILED.
7. BO size is at least the kernel copy of the Mesa render layout
   (`m9_render_surface_layout(surface_width, surface_height, 4)`, Mesa
   `m9_internal.h:111-137`).  Non-small category only (`width > 64 ||
   height > 64`) until Q3.
8. The de-tiled window view needed, `row_span * 4 * surface_height`, is at
   most the kernel's BAR0 transfer ioremap and at most the 32 MiB slot-0
   window.  2048x2048x4 = 16 MiB fits.  The current 3 MiB `bar0_window`
   must grow to that bound or get its own ioremap.
9. `offset = drm_gem_vram_offset()` passes `m9_vram_gpu_address(vram_size,
   offset, size - 1, ...)` [SRC `m9x/m9_submit.c:328-339`] (`-EOVERFLOW`).

The kernel never accepts a raw `d1`, address or descriptor bits beyond those
enumerated.  The window geometry is therefore bounded by a validated BO of
sufficient size.

**Execution and locking:**

```
kbuf = kvmalloc(width * height * 4)          /* or row chunks <= 4 MiB */
if WRITE: copy rows from user into kbuf      /* before any reservation */
m9x_vram_pin(bo)                             /* same order as submit */
ttm_bo_reserve(bo, interruptible)            /* COLOR RESV */
dma_resv_wait_timeout(resv, READ ? DMA_RESV_USAGE_WRITE
                                 : DMA_RESV_USAGE_READ,
                      interruptible, bounded by m9_fence_timeout_ms)
mutex_lock(transport.lock)                   /* COLOR RESV -> TRANSPORT */
  if transport.wedged: -EIO
  m9x_address_cpu_window(offset, d0, d1)
  READ : memcpy_fromio(kbuf row, win + (y+r)*row_span*4 + x*4, width*4)
  WRITE: memcpy_toio  (win + ..., kbuf row, width*4); wmb()
  m9x_address_cpu_identity()
mutex_unlock(transport.lock)
ttm_bo_unreserve(bo); m9x_vram_unpin(bo)
if READ: copy rows from kbuf to user         /* after all locks dropped */
kvfree(kbuf)
```

* User copies never run under `transport.lock` or the BO reservation.
  `data_ptr` may legitimately point into another VRAM BO's TTM mmap (the X
  front buffer in option b), and its fault handler reserves that BO.
* Reserving and waiting for fences keeps the ioctl from reading a target that
  a user submit is still rendering.  The kernel adds no new fence.
* `transport.lock` serialises with `m9_scanout_publish`, the triangle
  diagnostics and submits, which are all slot-0 users and all under this lock
  [SRC 1.1].
* Pixel order is little-endian BGRX dwords, exactly as in the window view
  (`tools/m9-scanout-readback.py` reads `& 0xffffff`).  The kernel does no
  format conversion.
* Returns 0, or `-EINVAL`, `-ENOENT`, `-EFAULT`, `-EOVERFLOW`, `-EINTR`,
  `-ETIMEDOUT` (fence wait), `-EIO` (wedged), `-ENOMEM`.
* Selftest: pack and validate the captured `cc210140/000100f0` and
  `cc210400/00010300` pairs.  Reject `c8010020` and `f0210140` while Q3 is
  open, and reject `row_span != ALIGN(width, 32)` and an undersized BO.

**Remaining hazard:** the retarget still changes BAR0 `[0, 32 MiB)` for
concurrent raw user mappings (1.7).  Until Q2 is answered, the specification
documents rather than removes it.  Once a second slot's BAR0 placement is
measured, `m9x_address_cpu_window` should take a slot index and kernel
transfers should use a slot that never aliases user-mapped BOs.

### 4.3 Later: GPU rectangle copy

For glamor-less Present at full speed, and for glamor itself, generalise
`m9_present_build` [SRC `m9x/m9_triangle.c:1563-1587`] into an ioctl that
copies validated rectangles from one tiled BO to another.  Source view is
`f021 | row_span` with `00010000 | height`, the destination view is
`f021 | row_span`, and the kernel owns the fence.  It is not specified here:
the rectangle, origin and trigger encodings are captured only for
`0x00f00140` at origin 0, so arbitrary rectangles need a coordinate-coded
capture first.

## 5. Open questions

* **Q1:** Is the raw storage of a GPU render target with `cc210400/00010300`
  byte-identical to what the display controller fetches for
  `DC0_PITCH=cc210400`?  Test: render a coordinate-coded frame, point
  `DC_START` at the render BO (T1) and compare the scanout window readback
  with the T2 copy.
* **Q2:** How is BAR0 at 32 MiB and above decoded, and can host-aperture
  slots 1..19 be placed at a BAR0 range free of user BO mappings for kernel
  transfers?  Where do the X front dumb BO and Mesa BOs land in VRAM?
* **Q3:** Do descriptor windows de-tile small-category (<= 64x64)
  surfaces, bit21-clear FBO layouts (`c8010020`) and 16-bit format `0x4e`
  the same way?  Are textures (`f021` sampler family) stored tiled too, which
  would decide whether Mesa texture uploads also need `SURFACE_ACCESS
  WRITE`?
* **Q4:** What PCIe link width and throughput does the M9148 host aperture
  give?  It bounds option (b)'s per-frame copies.
* **Q5:** Does a modesetting module built from `xorg-server-parhelia`
  21.1.24 load into the installed 1.21.1.22 server on this target (video ABI
  25.2 in both)?  Or should the whole server be deployed?
* **Q6:** What caused the stock modesetting segfault at 0x8 in
  `/root/Xm9-dri-gated.log`?  It is not reproduced here, since X was not
  started.
* **Q7:** Should `m9_fb_dirty` honour clip rectangles (with 8x4 tile
  alignment) instead of republishing 3 MiB for every damage?

<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
