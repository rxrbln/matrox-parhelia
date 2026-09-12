# Parhelia FAA command and fragment-buffer follow-up

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

This is a static follow-up to `parhelia-antialiasing.md`.  It examines only the
saved Windows `MTXPARD.dll`; it does not load a vendor driver or issue a target
probe.  The findings narrow the interface that an eventual FAA implementation
must reproduce, but do not make FAA safe to enable.

Evidence labels: **[BIN]** is direct disassembly of the saved binary.  **[OPEN]**
is intentionally not guessed.

| input | SHA-256 |
|---|---|
| `xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll` | `9072c6473f8f9c35ab7ea49cffd4fde6375502f37629c738b16abf7ff4ffb8bf` |

All Windows addresses below are image virtual addresses (image base `0x10000`).

## 1. DWGCTL FAA command: exact packet form

`sub_9DCC0` at `0x9dcc0` makes the command, subject to
`pDev+0xab50 & 0x0007fc00` [BIN].  On capable silicon it first emits the
following synchronization packet and submits it through `sub_2bef0`:

```
count = 3, packed indices = 0x21afafc0
  index 0xc0  (MMIO 0x1300) = 0x00000207
  index 0xaf  (MMIO 0x12bc) = tag | 1
  index 0xaf  (MMIO 0x12bc) = tag
```

`tag` is the per-device sequence value after it has been advanced by `0x10`
at `0x9dd13..0x9dd1f`.  The code then computes a mode word as
`0x00a | (arg1 ? 0x200 : 0) | (arg2 ? 0x800 : 0)` and skips its packet when it
equals the process-global cached word at `0x1d1f2c` [BIN].  The emitted form is:

```
count = 2, packed indices = 0x2121c0c0
  index 0xc0  (MMIO 0x1300) = mode
  index 0xc0  (MMIO 0x1300) = saved normal DWGCTL
```

Thus the directly observed cases are:

| call | mode written to DWGCTL | established meaning |
|---|---:|---|
| `sub_9DCC0(cmd, 1, 0)` | `0x20a` | enter fragment accumulation |
| `sub_9DCC0(cmd, 0, 0)` | `0x00a` | leave accumulation; the target-change path uses this before retiring the old target |
| `sub_9DCC0(cmd, 0, 1)` | `0x80a` | **[OPEN]** alternate leave/control form |
| `sub_9DCC0(cmd, 1, 1)` | `0xa0a` | **[OPEN]** alternate enter/control form |

The immediate restoration of the normal DWGCTL proves that `0x20a` is a
one-shot control command, rather than a draw-state replacement.  It does **not**
program a fragment-buffer address, pitch, or stride.  A driver must preserve the
sync packet, caching behavior, and restore write if it ever reproduces this
operation.  Issuing only `0x20a` has none of those prerequisites and remains an
unsafe experiment.

## 2. Fragment allocation: base location and what it does not reveal

`sub_9e570` constructs an allocator descriptor for the fragment blob [BIN]:

```
fragbuf+0x04 = 8             allocator memory-class selector
fragbuf+0x10 = 0x00010004
fragbuf+0x18 = 0             \
fragbuf+0x1c = bytes          | allocation dimensions/size input
fragbuf+0x90 &= ~3           / descriptor handle/control before allocation
sub_38760(fragbuf)
fragbuf+0x78 -> fragbuf+0x90  allocated offset published after success
```

The allocation request is then zeroed in CPU-visible memory by `sub_9e690`
(`sub_28cb0`, followed by `rep stos`).  The actual size is inserted into the
FAA shadow word at `render_state+0xa0`: bits `[31:14]` receive `bytes >> 8` and
the low fourteen bits are preserved (`0x9e7e3..0x9e809`) [BIN].  This is the
same field already documented as the fragment size divided by 256.

These facts establish a flat private allocation and a 4-byte aligned allocator
handle.  They do **not** establish a hardware base register or its address-unit:
the value at `fragbuf+0x90` is never directly paired with a literal DMA index
by the FAA allocation/control functions.  Nor do they establish a pitch,
fragment-record size, tiling mode, or a consumer-visible stride.  Calling the
allocation a normal 2-D surface would be unsupported by this evidence.

## 3. Resolve boundary and attachment restoration

The target-retirement route at `0x9e860..0x9eb0b` first calls the generic state
flush, restores the old colour/depth attachments with two ordinary DMA packets,
then dispatches the lower resolve helper:

```
indices 88 89 8c 8d -> MMIO 0x1220 0x1224 0x1230 0x1234
indices 8a 8b       -> MMIO 0x1228 0x122c
```

On the `caps & 0x7fc00` route it calls `sub_2e560`; the other route calls
`sub_2e360` [BIN].  It subsequently emits `DWGCTL=9` with a normal fence and
uses `sub_9d6e0` to stamp both attachments.  The per-target object is cleared
only after `sub_2ce60` returns.  This is enough to establish that target
retirement is a multi-step transaction, not merely the `0x00a` write.

The disassembly still does not expose which record or register passes
`fragbuf+0x90` to `sub_2e560`/`sub_2e360`, so the fragment base remains
**[OPEN]**.  The attachment packets above restore ordinary surfaces; treating
one of their six registers as the FAA base would contradict their values and
call placement.

## 4. P690 applicability

The P690 (`102b:2539`) belongs to the vendor's LX/P-series capability class.
The same `caps & 0x7fc00` predicate controls the FAA command, the FAA setup
pre-pass, and the P690-specific resolve-helper argument at `0x9e9b9` and
`0x9eaeb` [BIN].  Therefore the saved Windows driver considers P690-class
silicon eligible for its FAA path.

That is a software eligibility result only.  The archive contains no FAA-on
command capture, target readback, or recovery path for the current P690.  The
known P690-safe driver must continue to advertise one sample and no FAA until a
matched Windows FAA-on capture identifies the control-word and base-address
emission, followed by isolated validation on hardware.

## 5. Reproduction

```sh
objdump -d -Mintel --start-address=0x9dcc0 --stop-address=0x9de00 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
objdump -d -Mintel --start-address=0x9e570 --stop-address=0x9e858 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
objdump -d -Mintel --start-address=0x9e860 --stop-address=0x9eb10 \
  xddm32_210_00_105_se_u_nowhql/MtxPar/MTXPARD.dll
```
