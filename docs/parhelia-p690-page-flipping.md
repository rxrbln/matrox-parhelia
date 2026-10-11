# Page flipping on the Matrox P690

Copyright 2026 René Rebe <rene@exactco.de>

What the hardware does, what the kernel driver implements, why a full-screen
GL client used to stop dead with `Option "PageFlip" "true"`, and why page
flipping is not the default for this chip.

## What the hardware does

The display controller fetches from the address in `DC_START`
(`PARHELIA_DC_START`, bank-encoded like the engine's `SURF` operands).  The
register is **latched at the next retrace**: a write takes effect for the
following frame, never in the middle of one.  There is no flip queue, no
"flip pending" status bit and no flip-done interrupt; the only completion
signal available is the vblank interrupt, which the driver already uses for
`DRM_IOCTL_WAIT_VBLANK`.

That single property decides what the two DRM flip flavours can mean here.

### Vblank-synchronised flip (`drmModePageFlip` without `_ASYNC`)

`parhelia_pipe_update()` writes `DC_START` in the atomic commit tail and hands
the commit's event to `drm_crtc_arm_vblank_event()`.  The hardware latches the
new address at the next retrace and the vblank interrupt for that very retrace
completes the event.  The reported completion is therefore the frame on which
the new buffer became visible, and the old buffer is released to the client
only after it has stopped being scanned out.  No tearing, and the client is
paced at the refresh rate.

There is no flip-pending status bit and no flip-done interrupt on this chip,
so the completion is the armed vblank rather than an observed latch: a
`DC_START` write that lands inside the blanking interval after the hardware
has already sampled the address would become visible one frame later than the
event says.  The commit tail normally runs in the active picture, well before
the sampling point, and the measured 13.33 ms cadence at 75 Hz shows no
dropped frame, but the one-frame uncertainty is inherent without a flip-done
signal.

### Asynchronous flip (`DRM_MODE_PAGE_FLIP_ASYNC`)

`parhelia_plane_async_update()` writes `DC_START` and sends the event
immediately.  On hardware that can latch a new address mid-scanline this is
where tearing comes from; **on the P690 the image still changes only at the
next retrace**.  What the asynchronous flip actually buys, and costs, here is
the completion report: the client is told the flip is done while the previous
buffer is still on screen for up to one refresh, so it starts drawing into
that buffer straight away.  The tearing is produced by the client's own
rendering into the visible buffer, not by the address latch.  In exchange the
client is not throttled to the refresh rate, which is what a `vblank_mode=0`
benchmark or a timedemo asks for.

Both flavours are counted: `async_flips` in
`/sys/kernel/debug/dri/0/parhelia_submit_stats` counts asynchronous flips
that reached the hardware.

## What the kernel had to implement

`drm_atomic_helper_page_flip()` and `drm_mode_atomic_ioctl()` only record
`drm_crtc_state.async_flip`.  The atomic helpers act on
`drm_atomic_commit.async_update`, which nothing but a legacy cursor update
sets, and `drm_atomic_helper_commit_tail()` ignores `async_flip` entirely —
drivers that support asynchronous flips act on it in their own commit tail.
With the stock helpers an asynchronous flip is therefore committed like any
other one: programmed in the commit tail, completed at the next vblank, and
`.atomic_async_update` is never called.  That is why installing the plane
asynchronous hooks alone left `async_flips` at 0.

`parhelia_atomic_commit()` closes that gap: for a commit whose CRTC carries
`async_flip`, it runs `drm_atomic_helper_async_check()` and, if the plane
accepts the update, sets `async_update` so `drm_atomic_helper_commit()` takes
the asynchronous path (prepare planes, `.atomic_async_update`, unprepare) with
no commit worker and no vblank wait.

`parhelia_plane_async_check()` rejects anything that is not a pure buffer
change: a different CRTC, an inactive CRTC, any geometry, rotation, format or
pitch change, a different BO tile pitch, or a BO without a valid display
address.  A rejected asynchronous flip simply falls back to the normal
vblank-synchronised commit.

Unpinning the old framebuffer without waiting for a vblank is safe on this
chip because P690 VRAM BOs are pinned resident — TTM cannot move them (there
is no working VRAM copy engine path, see the handover) — so the address the
CRTC keeps fetching until the retrace stays valid.

### Measured (P690, 1024x768@75, `tools/parhelia-kms-flip-test`)

| flip type | flips | per flip | rate |
|---|---|---|---|
| vblank-synchronised | 150 | 13.33 ms | 75.0 /s |
| asynchronous | 400 | 0.005 ms | ~200000 /s |

`async_flips` went from 300 to 700 across the 400 asynchronous flips, i.e.
every flip reached `.atomic_async_update`; a second run after a module reload
repeated 150 synchronous flips at 13.328 ms and 400 asynchronous ones at
0.005 ms with `async_flips` at exactly 400.  `parhelia-drm-test
--scanout-dump` after `--final 0` and after `--final 1` returned exactly the
two painted test images (green bar over red, and white bar over blue), so the
flip really does select the buffer it reports.

Dumb framebuffers are limited by the 16 MiB host aperture: the flip test can
allocate two of them up to 1024x768 (a 1280x1024 dumb BO needs an 8 MiB
aperture window, two do not fit).  That is a dumb-buffer property, not a flip
property; GBM/native class buffers used by X and GL have no aperture window.

## Why a full-screen GL client stopped with `PageFlip true`

With `Option "PageFlip" "true"` a full-screen ioquake3 stalled a fraction of a
second after the video mode was set: the client asleep in `poll()`, the X
server idle in `epoll_wait()`, and `async_flips` at 0.

Traced end to end:

* the X server issued exactly **one** `DRM_IOCTL_MODE_ADDFB` +
  `DRM_IOCTL_MODE_PAGE_FLIP`, the kernel completed it, X delivered one
  `PresentCompleteNotify` and removed the old FB — the flip itself worked;
* `gdb` on the client: `dri3_find_back()` →
  `dri3_wait_for_event_locked()` → `xcb_wait_for_special_event()`, i.e. the
  GL loader was waiting for a `PresentIdleNotify` for a buffer to draw into.

The cause is Mesa's Parhelia-specific `single_back_buffer`
(`src/gallium/frontends/dri/loader_dri3_helper.c`, selected by the
"Matrox P690" renderer name), which pins a window drawable to **one** back
buffer so that one colour and one depth BO fit next to the pinned X front.
Present keeps the pixmap it flipped to busy until the *next* flip completes
and only then idles the previous one, so with a single back buffer the client
waits for an idle notify that only its own next present could produce, and
that present cannot be made.  A copy present has no such property — X copies
and idles the pixmap at once — which is why the same client runs with
`PageFlip false`.

`DRM_CAP_ASYNC_PAGE_FLIP` does reach the X server (`drmGetCap` returns 1 on
the target; `DRM_CAP_PAGE_FLIP_TARGET` is 0, correctly, because the simple
KMS CRTC has no `.page_flip_target`), so `ms->drmmode.can_async_flip` is set.
The one flip the stalled client produces is still a vblank-synchronised one:
`present_vblank_create()` only drops `sync_flip` when the requested target
MSC is not in the future, which the first present of a drawable does not
satisfy.  The asynchronous flip path from X is therefore only reachable once
the client reaches its steady state, i.e. only after the Mesa fix below; the
driver side of it is validated directly against KMS by
`tools/parhelia-kms-flip-test`.

Page flipping therefore needs at least two client back buffers.  The fix
belongs in the Mesa loader and is saved as
`mesa-loader-dri3-flip-two-back-buffers.patch`: honour `single_back_buffer`
only while Present is not flipping, and re-evaluate the limit inside
`dri3_find_back()` so the first `PresentCompleteModeFlip` completion lifts it.

## Should `PageFlip` be on by default?  No.

The modesetting driver in this tree defaults `Option "PageFlip"` to **off**
(upstream defaults it on) and logs the choice as
`(II) modeset(0): PageFlip: disabled`.  Reasons:

1. **A flipped scanout is a client colour buffer.**  The driver places the
   display image in a VRAM band of its own precisely so that display refresh
   does not compete with colour, depth and texture traffic; refreshing
   1920x1200 out of the colour bank cost textured fill 30 % (see
   `parhelia-p690-q3-performance-analysis.md` and
   `xserver-modesetting-front-rendering-scanout.patch`).  Flipping gives that
   up by construction: the scanned-out buffer is whichever client back buffer
   was presented last, and those are colour class.  At 1280x1024@75 refresh
   reads 393 MB/s, at 1920x1200@60 553 MB/s, so the penalty scales with the
   mode but does not go away.
2. **It costs a second client colour buffer**, which is the reason the Mesa
   driver pinned window drawables to one back buffer on this 128 MiB part in
   the first place.
3. **What it can save is bounded by the Present copy**, which is one native
   BITBLT of the window, and the P690 is memory-bandwidth bound: the copy
   reads and writes the same colour band the flip would move the refresh into.
4. Without the Mesa loader fix above, flips do not merely perform worse —
   a full-screen GL client deadlocks.

### What the copy path costs today, and what could not be measured

The Q3 `bench` timedemo at 1280x1024 on an X screen of the same size, with
the Present BITBLT copy, runs **32.6 fps** (791 frames in 24.3 s, frame times
14.0/30.7/54.0/5.9 ms).  Full-screen and windowed are the same to within
0.1 fps because a 1280x1024 window on a 1280x1024 screen presents the same
region either way.

The flip figure to compare against could **not** be measured: the deployed
Mesa deadlocks on the first flip (above), and the Mesa tree cannot currently
be rebuilt for this measurement — a build of the shared tree in its present
state produced a library that wedged the engine during the Q3 level load.
Running the X server with `PARHELIA_PRESENT_NOOP=1`, which drops the blit
submission, is not a substitute either: with nothing consuming its buffers
the client stops being throttled and the timedemo reports 998 fps of CPU time
while the GPU is still working.  The honest statement is therefore that the
flip-versus-copy frame rate is open, and that it has to beat both the copy it
removes and the colour-bank refresh it adds.

So flips stay an explicit `Option "PageFlip" "true"`, they are correct when
asked for (once Mesa can hand out two back buffers), and the shipped default
keeps the display band and the BITBLT present.
