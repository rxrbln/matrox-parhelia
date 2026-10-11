# P690 vblank, IRQ, and Present audit

## Scope and result

This audit covers the current P690 (`102b:2539`) DRM KMS driver, the patched
non-glamor modesetting Present path, and the saved vendor Linux/Windows
artifacts.  It deliberately does not modify source or enable an unproven engine
IRQ bit.

The display IRQ and head-0 vblank path work on the target.  They are not the
cause of Q3's approximately 1 FPS.  The render submit ioctl waits for both a
DMA writeback and a WARP fence before it returns, while holding the device-wide
submit mutex.  Each render submission therefore serializes the game regardless
of its GL swap interval.

Disabling swap synchronization can expose a *presentation* race (a 2D copy
into the scanout buffer while it is being scanned), but it cannot account for
wrong 3D contents already present in the rendered DRI3 pixmap.  For the current
windowed 640x480 Q3 route, it is expected to cause tearing or an incoherent
capture at most.  It must not be used as a render-performance workaround.

## Live target evidence

On 2026-09-10, while the target ran Xorg with
`/tmp/xorg-parhelia-pageflip.conf` and Q3, IRQ 18 advanced from 585,916 to
586,036 in two seconds: exactly 60 interrupts per second.  The same shared
line also serves three OHCI devices, so the per-CPU value rather than a
zero/nonzero first column must be read.  The kernel lists `parhelia` as a
handler on that line.

The active KMS CRTC reported a 1024x768 mode at 60 Hz.  Xorg reported
non-glamor DRI3 PRIME presentation and `PageFlip=true`.  The running Q3 command
line included `+set r_swapInterval 1 +set com_maxfps 120`, yet Q3 still showed
the known approximately 1 FPS behavior.  Thus a 60 Hz vblank wait is not
limiting that run.

The scanout plane remained an Xorg-owned 1024x768 framebuffer.  Q3 is a
smaller DRI3 client pixmap, so it cannot be a direct full-screen page flip in
this configuration; it uses the Present copy route below.

## Current open-driver paths

### Vblank IRQ

`parhelia_pci_probe()` initializes one vblank counter, masks all IRQ sources,
acknowledges W1C pending bits, and installs one shared handler:
`linux-7.2/drivers/gpu/drm/parhelia/parhelia_drv.c:269-280`.

`parhelia_pipe_enable_vblank()` acknowledges and enables bit 1 in `0x8fc`, and
`parhelia_pipe_disable_vblank()` clears and acknowledges it:
`parhelia_kms.c:409-429`.  `parhelia_irq_handler()` reads `0x8f4`, writes the
same status to W1C `0x8f8`, and calls `drm_crtc_handle_vblank()` when head-0
bit 1 is set: `parhelia_kms.c:431-442`.  The established definitions are
`PARHELIA_MMIO_IRQ_{STATUS,ACK,MASK}` at `parhelia_drv.h:62-66`.

This matches the saved vendor routing.  `OsSetVSyncInterruptState` uses mask
`0x2` for head 0; the vendor fast ISR reads `0x8f4` and writes its observed
status back to `0x8f8`.  See `parhelia-engine-and-context-init-complete.md`
§4, especially its recovered `0x8fc |= 0x2` sequence, and
`parhelia-kernel-abi.md` §8.

The driver correctly keeps unidentified sources masked.  The saved vendor
arm path used `0x8fc = 0x00010000`, but the engine-completion bit is still not
identified on P690.  Enabling bit 16 merely because it appears in that capture
would introduce a shared-IRQ source without a completion protocol and is not a
safe optimization.

### Atomic flip

`parhelia_pipe_update()` programs `DC_START` (`0x1824`) and transfers an
atomic event to DRM's vblank queue (`parhelia_kms.c:377-407`).  `DC_START`
latches at retrace.  This is the correct primitive for the current single-slot
KMS implementation and relies on the working IRQ path above.

The vendor has a richer, separate flip queue: it waits until the preceding
render tag at `0x41c` retires, then writes a selected entry in a four-slot
START bank and changes the pending slot in `DC_CTRL` from vblank.  This is
decoded in `parhelia-rendertarget.md` §§6.2-6.5.  Only slot 0 has direct local
hardware confirmation.  Replacing the current atomic update with the vendor's
four-slot scheme would be speculative and is not needed to fix Q3 throughput.

The current event pattern is the normal DRM pattern: write the
vblank-synchronized state, take a vblank reference, then arm the event under
`event_lock`.  It is used by small upstream drivers too.  It has one inherent
limitation: `drm_crtc_arm_vblank_event()` cannot itself make a
vblank-latched register update and its interrupt atomic.  The DRM core
documents that neither simply arming before nor simply arming after the
register write closes this race (`drm_vblank.c:1103-1139`).  A missed boundary
can make the completion event one frame late; it cannot alter the selected
framebuffer or corrupt 3D pixels.

Therefore there is no safe one-line ordering change to make here.  Exact
"event for the latch that consumed this START" semantics would need a separate
pending-flip state and a controller-visible pending/active-slot completion
test in the IRQ handler.  The vendor uses just such a scheme, but its four-slot
bank is only partly hardware-proven locally.  Do not introduce it until a
target capture proves the `DC_CTRL` pending/active bits and slot transitions.

### Non-glamor DRI3 Present copy

The target config selects `AccelMethod "none"` and `PageFlip "true"`.  The
patched modesetting driver accepts a DRI3-imported pixmap for a full-screen
flip only when the pixmap is eligible (`present.c:232-388`).  Otherwise its
`ms_present_copy()` submits one recovered 2D blit from the DRI3 pixmap into the
Xorg front buffer (`present.c:391-486`).  That call sets
`PARHELIA_SUBMIT_FENCE`, but the current ioctl is already synchronous, so the
blit has completed before Present receives success.

With a nonzero swap interval, Present schedules the copy at an MSC.  With an
immediate/no-vblank request, the same 2D command may write the scanout buffer
during an active scan.  That is the source of visible tearing.  It is not a
loss of render ordering: the render ioctl has already completed before the
copy starts, and the copy ioctl completes before its return.

A windowed client should therefore keep `r_swapInterval 1` for image checks.
For maximum FPS measurements, use `vblank_mode=0` only with a full-screen
direct-flip-capable workload or treat the output as deliberately tearable.
Do not infer GPU rendering correctness from a screenshot taken while a copy is
in progress.

## Why Q3 is slow

The UAPI explicitly defines `PARHELIA_SUBMIT` as synchronous because the
driver owns one primary buffer, one writeback page, one fallback vertex upload,
and shared secondary DMA blocks:
`include/uapi/drm/parhelia_drm.h:145-168`.

In the normal coalesced path, `parhelia_submit_ioctl()` publishes all calls
once, then spins on `writeback_cpu[8]` and subsequently polls `0x41c` with
`udelay(1)` (`parhelia_submit.c:892-915`; the latter helper is at
`330-345`).  It retains `pdev->submit_lock` from line 614 through the waits and
unpins every relocated BO only after completion (`1018-1026`).  Q3 therefore
pays these waits for every Gallium submit.  This is consistent with the
measured high kernel-time fraction and ioctl rate in
`parhelia-q3-submit-performance-audit.md`.

Neither `vblank_mode=0` nor `r_swapInterval 0` bypasses that render completion
contract.  They only alter scheduling after the render ioctl returns.

## Safe minimal work proposal

1. Make no functional vblank/IRQ change for the Q3 performance issue.  Keep
   IRQ source bit 1, W1C acknowledgement, masking, and the existing atomic
   event path unchanged.  The hardware lacks a proven mechanism to make the
   event arm and a retrace-latched start-address update atomic.

2. Keep Q3 on the Present copy route at `r_swapInterval 1` while validating
   rendering.  Do not advertise asynchronous page flips: the KMS driver does
   not implement `DRM_CAP_ASYNC_PAGE_FLIP`, and a partial window cannot safely
   flip the scanout buffer anyway.

3. Treat render submission as a separate project.  The smallest safe
   performance design is an explicit asynchronous submit ABI with at least two
   independently owned slots.  Each slot needs its own primary span, secondary
   DMA blocks, writeback word, fence tag, and pinned BO references.  A completed
   slot releases pins and wakes waiters; a busy slot makes the next submit wait
   or use the other slot.  The existing single global buffers cannot be reused
   before the engine finishes.  The detailed rationale and an indexed-draw
   follow-up are in `parhelia-q3-submit-performance-audit.md`.

4. Before an asynchronous render ABI reaches KMS/direct flip, attach its
   completion fence to every written scanout-capable GEM object's reservation.
   The atomic plane update must wait for that fence before exposing a BO to
   `DC_START`.  This preserves the vendor's key ordering rule—render tag first,
   vblank flip second—without claiming a four-slot display bank that has not
   been proven on this board.

Do not implement the tempting shortcut of returning from the current ioctl
before its two waits.  The next ioctl would overwrite `primary_cpu`,
`dma_blocks[]`, or `writeback_cpu`, and unpin a VRAM object while hardware can
still read it.  That is a direct corruption and wedge path.

## Test plan

All kernel experiments use only the staged module and
`modprobe -d /tmp/parhelia-stage parhelia`; never copy `parhelia.ko` to the
normal `/lib/modules` tree.

1. **IRQ baseline:** with X active, sample the `parhelia` shared line in
   `/proc/interrupts` for ten seconds.  Expect the total delta to equal the
   selected CRTC refresh within normal timer error (about 600 at 60 Hz).
   Check `/sys/kernel/debug/dri/0/state` for an active CRTC and the selected
   mode.

2. **Vblank event baseline:** run repeated atomic page flips through a small
   DRM test or X Present.  Record vblank MSC/event timestamps.  Events must be
   monotonic and none may be lost across 10,000 flips.  Allow an occasional
   one-frame completion skew until a controller-active-slot test has been
   independently proven; it is a timing-reporting issue, not a framebuffer
   corruption criterion.

3. **Present image split:** capture a fixed Q3 frame at `r_swapInterval 1`.
   Repeat with `r_swapInterval 0` and `vblank_mode=0`.  A difference limited to
   a horizontal tear proves a scanout-copy issue; a persistent wrong world or
   HUD region in both captures belongs to Mesa rendering/state, not vblank.

4. **Performance split:** for each swap policy, sample Q3 `utime` and `stime`,
   `strace -f -c -e ioctl`, and frame rate for at least ten seconds.  If the
   application remains in kernel time with thousands of submit ioctls, preserve
   the synchronous-submit conclusion.  `glxgears` with `vblank_mode=0` is a
   presentation-unlimited control, not evidence that Q3 is asynchronously
   rendered.

5. **Future async-submit gate:** first pass single-triangle, texture/depth,
   vertex-alpha, X Present copy, and Q3 image comparisons with each slot.  Then
   run alternating render and atomic flip stress for 10,000 frames, checking
   event sequence monotonicity, no DMA/writeback timeout, no pin leak, and no
   scanout of an unfinished BO.  Only after that is an async direct-flip path
   eligible for a refresh-unlimited benchmark.
