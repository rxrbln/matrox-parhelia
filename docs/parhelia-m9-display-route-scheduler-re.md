# M9128 display-route scheduler: bounded original-source check

The physical M9128 `102b:0540` fullscreen interval-0 path currently copies
each presented frame. Removing that copy would matter, but enabling async flip
requires a qualified latch and old-scanout-owner retirement contract. The
original `PhxSetDisplayStart` leaf and complete
`H2OCeSetDisplayStartAddress` API establish write grammar only.

`tools/m9-display-route-scheduler-check.py` executes a separate original
route-transition routine, `m9x11412` at `0x2b63d0`, in the pinned M9 Linux
driver image (SHA256 `3878475450ffad5cd648cc037cc65c29fcb07b5e86caf41ccf5983ef8e570273`).
The CPU descriptor and three 28-byte route records are synthetic owned data.
The current route ID is 0; the requested ID is 1. When row 1 has required
status 3, the original code enters `m9x25201` at `0x2b5650`, which calls
`m9x17687` with zero. On the M9128 branch, `m9x8988` at `0x2f80a0` calls the
actual read service at `0x2b8ad0`, requesting offset `0x318` with a stack
output. The emulator **stops at that service**. The setter `m9x7332` at
`0x2ed6d0`, the `m9x6104` family dispatcher, and the write service have not
been reached. No read value is invented. Changing only row 1 status to 2
returns a refusal before a read, setter or write.

The source instruction after the read takes the high byte of the returned
32-bit value, clears bit 6 when the argument is zero (sets it otherwise), and
writes the resulting word through the original write service. This is an
actual read-modify-write dependency; a write-only trace of the display-start
setter cannot stand in for the full transition. The direct-call inventory
includes the route setter's calls into the same `m9x6104` dispatcher used by
the complete display-start API, but this bounded run intentionally ends
before that point.

## The `0x318` bit is not an owner-latch receipt

The original `0540` getter `m9x25509` selects `m9x2331` at `0x2f8070`.
That leaf reads `0x318`, shifts high-byte bit 6 into a Boolean, and returns
it: the observed field is full-word bit 30. In the same binary, `m9x8988`
at `0x2f80a0` accepts a caller Boolean and explicitly clears or sets that
same bit through a read-modify-write. The pinned `.text` has two identified
direct calls of the getter and six of the setter. At `0x2de6e3`,
`m9x7701` tests the bit, records a CPU request field at `+0x30` when set,
then calls the setter with zero. At `0x2dec45`, `m9x30415` tests the bit,
calls the setter with zero on one path, and calls it with one on the other
path after its display work. Exact original instruction bytes, the call
inventory and four negative source mutations are checked by the emulator
script.

These source-controlled changes mean a bit-30 read alone cannot identify
which START backing is actually being fetched. No getter path here compares
the active START address with the new owner's live address or acknowledges
a vblank event. This does not prove the hardware never changes the bit; it
limits what this source evidence establishes. In particular, the route's
pre-setter CPU status stores and a returned bit-30 value cannot authorize
release of the old pinned framebuffer.

## Actual source caller and status ordering

The pinned ELF `.text` has one direct call into `m9x11412`: `0x2b0ce0`
inside `m9x21210` at `0x2b0ca0`. Its relocated `m9x28149` virtual table
places that method at slot `+0x40`. It first calls `m9x4532` at `0x2b0a40`
to prepare a route ID. Failure after the transition calls `m9x28080` at
`0x2b5f40`; the successful path calls the same object's virtual slot `+0x48`
with arguments taken from the request. The pinned slot resolves to
`m9x6634` at `0x2b0640`, which enters `m9x6002` at `0x2b6020`. Its later
`m9x221` branch explicitly selects the `0540` routine `m9x27667` at
`0x2f2b30`. Thus the success follow-up is further original display control,
not an identified Xserver Present-completion callback.

Inside `m9x11412`, after the still-unanswered `0x318` read chain, source
stores requested route ID at descriptor `+8`, new-row status 1, old-row ID
at descriptor `+0xc`, old-row status 2, and prior-row status `0xff`. **All
these CPU status stores precede** `m9x7332` (the display setter), followed by
`m9x28260` and `m9x15769`. In particular, old-row status 2 cannot itself
mean hardware retired the old scanout: the setter has not run yet. The
checker pins the sole direct caller, virtual slots, M9128 dispatch and store
ordering against exact original bytes.

This virtual method has not been joined to a Linux Present/DRI swap callback
or proven to be the active KMS update path. Its `0x318` read is **not** a measured
scanout latch, completion or vblank acknowledgment. The next source experiment
is to trace the virtual method's external invocation and identify any swap
or Present relationship. The complete M9128 transition then needs source
read values and an observable event that permits release of the old scanout
owner. Any eventual native update must derive start addresses from live
pinned backing. Async capabilities remain disabled until latch, event and
lifetime are qualified on the target under the established lock.

## Why the apparent Linux Present names do not close the link

The same ELF contains readable `LinuxServices::Present` strings at `0xe9e900`
and `0xe9e980`. Their only identified code references here are
`m9x13211`/`m9x20228` at `0x34cec0`/`0x34cef0`: each builds arguments for
`__assert_fail`, with no presentation work. The checker pins those references
and the actual call target. The Xorg `xf86CrtcCreate` registration at
`0x288eac` installs a CRTC function table at `0x1218620`; this is not itself
a Present/pageflip callback either. The active physical Q3 session uses the
new modesetting DDX, not this old vendor Xorg CRTC table. Thus matching a
string or an old CRTC slot to a display-route routine would conflate two
different call paths.

This is a bounded negative, not proof that no proprietary swap path exists.
Without a source join from the real swap caller and a target-observed latch
or event that protects the old pinned BO, async flip remains gated. A locked
two-live-owner scanout/event probe is the next hardware experiment once FAA
allows a target slot; it must observe old-owner safety after the event, rather
than equating the MMIO write or CPU route status with completion.

Run the CPU-only proof with:

```sh
/home/ai/tmp/faaemu/venv/bin/python tools/m9-display-route-scheduler-check.py \
  --out /tmp/m9-display-route-scheduler.json
```

The remaining target experiment needs two simultaneously pinned, distinguishable
M9128 scanout owners. A synchronous page-flip control should record the
requested live START value, actual display status across vblank, the DRM
event and pixels before and after. Only an observed latch tied to the new
owner can justify a later old-owner release test. No async flip capability
or release rule follows from this CPU-only source proof.
