Parhelia predicted register shadow
==================================

Status
------

``state_shadow_cache=1`` enables an experimental, device-wide cache of a small
set of indexed 3D registers.  It defaults to false.  The implementation is a
kernel-side analogue of the vendor driver's HWREG dirty cache, but deliberately
does less until each additional hardware group has a direct correctness test.

The filter runs after packet validation, every relocation lookup and patch,
GEM lookup, VRAM pin, and mode-specific contract check.  It leaves userspace's
secondary packet storage unchanged.  When a complete opcode-zero packet is
relocation-free, contains only registers in the allowlist below, and every
value equals the predicted device state, its primary-ring CALL is omitted.
All object references and pins still live until the submission fence retires.

The device's ``submit_lock`` serializes filtering and producer updates.  A
transaction starts with the predicted state after the last published primary
span, walks the new packets in execution order, and records the resulting
state privately.  It commits only after the complete span is exposed through
``PRIMEND`` and the front-end strobe.  This lets a later asynchronous submit
compare against queued work: FIFO order guarantees that its predecessor's
state writes execute first.  A failure before publication discards the
transaction.  A timeout invalidates the cache and wedges the device.

Exact allowlist
---------------

Each index names ``0x1000 + index * 4``.

======= ====== ==========================================
index   MMIO   recovered use
======= ====== ==========================================
``b0``  12c0   colour/depth mask high
``b1``  12c4   colour/depth mask low
``b2``  12c8   depth enable and compare function
``b3``  12cc   depth mask
``b4``  12d0   depth write enable
``b5``  12d4   depth range minimum
``b6``  12d8   depth range maximum
``b7``  12dc   depth range scale
``b9``  12e4   alpha test
``ba``  12e8   blend control
``be``  12f8   antialias threshold
``bf``  12fc   raster mode
``c9``  1324   colour write mask
``ca``  1328   scissor minimum
``cb``  132c   scissor maximum
======= ====== ==========================================

The evidence for treating these as ordinary idempotent values is the vendor
Linux driver's two-bank HWREG file: state validation compares computed values,
sets a register dirty bit only on change, and its emitter walks that dirty map.
The open reference builder independently identifies the listed meanings.

Hard exclusions and limitations
-------------------------------

The filter never omits a packet containing a relocation.  It therefore cannot
hide a render target, depth target, texture, or vertex BO from validation,
pinning, reservation fencing, or lifetime tracking.

It never omits vertex fetch, WARP program/bind/stage, texture selection or the
windowed texture file, surface descriptors, fragment combiner or FIFO state,
draw control/arming/geometry, EOB, fence, watermark, strobe, or opcode ``0x41``
constant-group packets.  A packet mixing one allowlisted value with any such
register executes intact.  The shadow observes the allowlisted write so a
future comparison is accurate, but it does not rewrite, split, or reorder the
mixed packet.

Engine initialization and WARP upload are writers outside ordinary submission,
so initialization invalidates every valid bit.  Synchronous and asynchronous
timeouts, wedge handling, and teardown invalidate it again.  No recovery is
attempted after a wedge.

The optimization is packet-granular.  It saves one secondary CALL only when a
whole register-list packet is redundant; it does not remove individual dwords
from mixed packets.  This keeps packet offsets and relocation operands stable.
``trace_state_shadow=1`` reports per-submit and cumulative eligible packets,
omitted packets, and omitted register writes.  A pure static transition test
runs from ``parhelia_submit_init()`` and rejects module initialization if first
write, repeat write, changed value, relocation exclusion, unsafe-register
exclusion, or invalidation semantics regress.

This path needs isolated P690 image, depth-order, blend/alpha, scissor, Quake 3,
and uncapped performance validation before it can become the default.
