# M9 async completion and presentation contract

2026-10-06; M9128/M9 only. Native reference before this change: Q3 GL1,
1024x768 fullscreen, vsync 0, 387 frames / 6.8 seconds = 56.6 FPS, about
55 jobs/frame, zero uniform splits and about 7.5 ms/frame ioctl time. Those
numbers were measured by the target owner on the earlier uniform-run build;
they are not measurements of this completion change.

## Actual correctness gap

Kernel M9 `async_submit=Y` posts one job and returns its *issued* writeback
target. The next submit drains that job before rewriting the one shared
secondary command image. This is a single pending job, despite two primary
buffer slots. Existing Mesa `m9_context_flush` constructed an already-signaled
fence for every returned point, and `m9_fence_finish` returned that Boolean
without hardware access. Its comment incorrectly asserted that submission
ioctls were synchronous. Therefore a GL flush fence/GLFinish could report
completion while the final posted job still ran. Pixel readback and resource
access ioctls drain separately, so ordinary rendering/readback tests concealed
this gap.

The kernel direct tiled scanout path programs the display address directly.
It relies on the ordinary DRM framebuffer reservation dependency, but user
render jobs currently do not publish a completion fence to `dma_resv`. Correct
Mesa fence waiting alone does not establish that presentation dependency.

## First coherent fix: observed completion timeline

`WAIT_POINT` is a dedicated 32-byte render ioctl, command number `0x0d`:
`point`, `timeout_ns`, `completed_point` (three u64), `flags`, `pad` (two u32).
It never posts a command. `flags`, `pad`, and input `completed_point` must be
zero. A request for an unaligned or future point is rejected. Point zero is
a capability query returning the last observed completion. Timeout zero
polls. A finite wait is bounded by the requested timeout and the driver
watchdog; U64_MAX uses the watchdog bound. Expiration returns `-ETIME` and
leaves pending work and its ownership intact. Transport-lock contention counts
against that same wait budget; a zero-time poll cannot block behind a submit
drain. Device failure returns `-EIO`;
an interrupted wait returns `-ERESTARTSYS`.

The transport now keeps separate submitted/completed 64-bit points using the
existing 32-bit target wrap extension. Async posting advances only submitted
progress. An actual matching hardware writeback advances completed progress
when the existing owner is drained. Synchronous posting advances both after
its real writeback wait. The transport lock prevents a different job replacing
the owner while a wait observes it. A status nibble is masked as in the real
vendor/writeback comparison; observing a later or different raw target does
not invent completion of the pending owner.

Mesa marks a nonzero submitted point unsignaled, probes this ioctl without
posting, and uses it in `fence_finish`. Successful completion is cached with
atomic accesses. Kernel errors, early timeouts and short completion progress
remain unsignaled. On older kernels a zero-time poll returns false; an explicit
positive wait attempts the existing synchronous NOOP_FENCE drain. This requires
`m9_render_diag=Y`; with the target's normal old `N` configuration it returns
EACCES and the fence remains unsignaled. It is a conditional compatibility
fallback, not general completion support for old kernels. That older ABI
cannot enforce an arbitrary finite caller timeout, so this compatibility path
can run up to its existing watchdog. The new ioctl does enforce its bounded
wait. Empty fences and NULL fences remain immediately complete.

This is a correctness prerequisite, not a demonstrated FPS improvement. The
single shared secondary/BO ownership protocol is unchanged.

## Implicit buffer dependencies and automatic completion

The next kernel slice reserves all user BOs using the standard DRM_EXEC WW
locking helper, with READ/WRITE usage from the validated BO reference list.
It waits foreign implicit hazards and propagates their errors. Own M9 timeline
dependencies remain in the ordered primary stream; the existing single-slot
drain establishes their retirement. A per-job fence is published into every
participating reservation before the actual doorbell. Unposted errors signal
an error fence immediately. A completed synchronous job signals normally.
Posted but unfinished work transfers its fence, pins, BO references, and a
preallocated retirement record under the same transport lock as posting.

A completion worker observes the exact real hardware writeback even if no
next submit/readback occurs. It drops transport locking while sleeping and
never takes a BO reservation. Only real matching completion signals a job
and schedules ownership release. Its watchdog failure preserves an unsignaled
fence and all outstanding refs/pins, marks the transport wedged, and stops
polling. Teardown stops/cancels the worker before disabling host DMA; only
then can it signal a failure fence and release quarantine. Failure signaling
precedes BO unpin/last-put: TTM's failed reservation-copy/OOM fallback can wait
30*HZ on an object's reservation fences during last-ref release, so putting
the object before signaling its own fence could stall teardown. The retirement
record is allocated before posting, avoiding an allocation failure during
ordinary user-job retirement.

Private triangle resources finish allocation before user WW reservations are
acquired, so TTM reservations during first resource construction cannot nest
under the user reservation set. `DRM_M9X` now selects `DRM_EXEC`. The full
module compiles and its dependency metadata includes `drm_exec`; deployment
must load/install the matching module if it is not already available.

The M9 simple-pipe funcs omit `prepare_fb`, so the standard simple KMS helper
calls `drm_gem_plane_helper_prepare_fb`. That helper supplies the framebuffer
implicit dependency. The newly published render fence therefore reaches the
existing KMS dependency path; no unproven display command is added. Native
presentation qualification still requires the target owner's new-module run.

The integrated-source review also removed the inherited custom fence release.
It called kfree directly, which violated the dma_fence RCU-free contract; it
also kept driver ops attached after signaling, allowing a dma_resv-held fence
to reference module callbacks after unloading. `struct m9x_fence` has its base
as the allocation's first member, so the core default `dma_fence_free` safely
uses kfree_rcu. Omitting custom release/wait lets Linux 7.2 detach ops at signal.
This follows actual `include/linux/dma-fence.h` release/lifetime documentation
and `dma_fence_signal_timestamp_locked`/`dma_fence_free` implementation, rather
than the adapter harness inventing callback lifetime rules.

## Focused verification

`tools/m9-completion-check.py KERNEL_M9X MESA_M9X` extracts the actual kernel
timeline/wait functions and Mesa fence create/finish functions, compiles them
with controlled writeback, clock, drain and ioctl adapters, then exercises
75 cases under AddressSanitizer and UndefinedBehaviorSanitizer. It checks ABI
layout equality, async unsignaled posting, zero/finite/infinite-bounded waits,
exact writeback matching, delayed completion, lock contention, invalid ioctl
inputs, unplug/disabled-device admission, interrupts/device errors,
64-bit wrap, cached completion, older-kernel fallback, and actual failed-drain
BO/pin quarantine versus real retirement, and actual user-submit ownership
transfer across unposted errors, sync timeouts, async posts, sync completion
and an error after real completion. It also covers implicit publication while
reservations are held, completion without a next submit, watchdog quarantine,
worker stop/unplug, foreign-vs-own hazards, foreign errors and reservation
failure before posting, and the actual fini function's stop-DMA / signal-failure /
last-BO-release order with a TTM self-wait adapter. WW implementation and hardware access are adapters,
not a claimed emulation of DRM_EXEC's kernel implementation. LeakSanitizer is
disabled because the workspace sandbox uses ptrace; every allocated test
fence is freed explicitly. Actual modified kernel objects/full module compile, Mesa batch
suite and context/screen objects compile. Native validation remains pending
the target owner's next controlled kernel deployment. The read-only
`tools/m9-wait-point.c` checks the new ioctl's capability query and five invalid
inputs without submitting any command; it compiles with Wall/Wextra/Werror.

## Next required slices, still open

1. Native-qualify the implemented `dma_resv` publication/automatic completion
   slice, including the last frame's dependency and read-only WAIT_POINT probe.
   It preserves f587 same-posting-lock pin ownership; no fence is signaled from
   a merely returned target.
2. Timed-out async jobs now retain their pending owner, BO references and pins;
   new submits honor a drain error, and fini releases quarantine only after
   clearing PCI bus mastering. This is tested against the actual drain function.
   User-stream framing now reports the true doorbell result: synchronous
   failure after posting also transfers the vectors to quarantine while an
   unposted failure or a retired job's guard error leaves them with the caller
   for ordinary cleanup. The actual submit function is covered offline.
3. Provide at least four independent secondary command slots and retirement
   ownership vectors. The current shared 32 KiB secondary cannot be rewritten
   with a previous reader in flight. Primary append capacity alone does not
   solve this.
4. Add retirement-aware Mesa source/index/system/CPU-written arena banks and
   backpressure. Current double buffering permits only one outstanding job;
   posting a third job without bank retirement would overwrite the first.
   Include CPU BE clears, inter-pass scratch, state/resource destruction and
   translation-table changes in this ownership audit.
5. Capture which real full-screen presentation path is active. Direct scanout
   is already enabled; non-addressable tiled windows use a drain/copy path.
   Avoid attributing Q3 ioctl time to a presentation copy without that evidence.

The real Linux vendor ExecuteDma captures in
`parhelia-m9-cold-transition-submit-decode.md` and
`parhelia-m9-requester-drm-contract.md` establish primary ring appends and
switch/ownership sequencing without an ordinary per-submit no-op drain.
They support a queue design, but do not authorize skipping reader retirement
of the clean-room driver's current shared images. No speculative FAA packet,
opaque auxiliary-bank interpretation, or vendor success return is introduced.
