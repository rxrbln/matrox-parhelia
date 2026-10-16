Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

# Can a locked-up Parhelia or P690 be reset at run time? (review, 2026-10-16)

Tags: **MEASURED** on hardware, **PROVEN** read out of a vendor binary,
**INFERRED** consistent reading, not forced. Builds on
`parhelia-p690-engine-reset-re.md` (vendor reset, P690) and
`parhelia-cold-post-bringup-re.md` (vendor cold bring-up, Parhelia family);
this review closes the gaps both left open and checks them against our kernel.

## Answer

**Yes, for both families, but only as a chip reset followed by a complete
clock, memory-controller and engine re-initialisation.**  There is no
engine-only reset in any vendor binary (PROVEN, negative scan); the vendor's
answer to a busy engine is to wait forever.  The chip reset is the PCI PM
D3hot -> D0 strobe on config `0xE0`, and every piece of the re-initialisation
is now known for both families:

| | Parhelia-512/8X (0527/0528) | P690 (2539) |
|---|---|---|
| chip reset | `HSLPARHardwareReset`: 7 config dwords, `0x0308 = 3`, `0x0300` bit 8 across the strobe | `HSLSNXHardwareReset`: all 64 config dwords (skip `0x04/0x44/0x48`, COMMAND last), `0x0300` recompute, 10 ms per half |
| PMCSR at `0xE0` | MEASURED (`parhelia_drv.c`) | **MEASURED today**: PM capability at `0xdc`, PMCSR `0xe0`, NoSoftReset = 0, so D3hot -> D0 resets the function; no FLR (DevCap bit 28 = 0) |
| clocks | `HWPARInitializeSystemClock`, implemented | `HWS90InitializeSystemClock`, **decoded today** (below) |
| memory controller | `HWPARInitializeMemoryController`, implemented | `HWS90InitializeMemoryController`: 44 PInS registers, decoded in the P690 note; **`ctrl` settled today** |
| board data needed | PInS 6.0a: clock select `0x84`, PLL records `0x24`, MC records `0xa4/0xd8/0x10c/0x140` | PInS 7.0: `0x200..0x20c`, PLL record `0x24`, `0x210..0x2bc` |
| present in our board ROMs | **yes**, all (Alpha board ROM `vbios-matrox-parhelia`, PInS at `0x7800`, len `0x200`) | **yes**, all (`tools/p690.rom`, PInS at `0x76c0`, len `0x400`; 29 of the 44 MC dwords non-zero) |
| our kernel | `parhelia_post_cold_bringup()` (commit b16e200) has the whole sequence **but is called from nowhere**: it has never run | nothing yet |

The reset itself is known to work: **MEASURED on the P690**, a module load with
`reset_on_probe=1` (`pci_reset_function_locked()`, the same PM strobe) cleared
the latched "WARP store busy" state that blocks a reload.  What failed was
everything after it: no memory-controller re-initialisation, so every VRAM
access was corrupt until a reboot re-POSTed the card (HANDOVER.md).

## What was settled today

### 1. `HWS90InitializeSystemClock` (`mtx_drv.so` `0xe60d0`), PROVEN

```
pins = HWPARGetPINS(dev)
sel[0..2] = pins[0x200], pins[0x204], pins[0x208]
for r in 0xe00, 0xe04, 0xe08: write(r, read(r) | 1)    /* park */
for r in 0xe00, 0xe04, 0xe08: write(r, 1)
for r in 0xe00, 0xe04, 0xe08: write(r, 0)
HWDelay(1000)
for fs in 0..3, register 0xe30 + 4 * fs, PInS record 0x24 + 0x14 * fs:
    if IsScUseFS_SUN90(sel[0], sel[1], sel[2], fs):
        if record.divider:                     /* +0x04 */
            write(reg, divider | 0x80000000); HWDelay(100); write(reg, divider)
        else:
            HWS90InitialiseFSController(dev, fs, record.target)   /* +0x00, kHz */
HWDelay(10000)
for r in 0xe00, 0xe04, 0xe08: write(r, 1)
for i in 0..2: write(0xe00 + 4i, sel[i] | 1)
for i in 0..2: write(0xe00 + 4i, sel[i])
write(0xe24, pins[0x20c])
```

`IsScUseFS_SUN90` (`0xe5f90`): synthesiser `fs` is in use when any select's
field `(sel >> 1) & 0xf` names it (FS0: 1, 8, 9; FS1: 2, 10, 11; ...).
`HWS90InitialiseFSController` (`0xe5bb0`) builds the limits from the record
(`+0x08..+0x12`, MHz -> kHz) and calls `sun90pllInit`, the same SUN90 PLL code
`parhelia-p690-output-path-re.md` §4 decoded for the pixel clock: our
`parhelia_sun90_clock_solve()` (parhelia_kms.c) already produces that divider
word for FS2, and `sun90pllHwOscOn` powers the oscillator on `0xe24` first.

The P690 board ROM selects `0x20902 / 0x2 / 0x2` (every domain on FS0), FS0 at
**350000 kHz with no ready divider** (so the solver runs), limits VCO
800-1600 MHz, output 54-108 MHz (the solver's own constants), and
`0xe24 = 0x60000000` at the end.  This is the same `0xe24` the vendor's
liveness test reads (`0 or 0x80000000` = dead).

### 2. `ctrl` in `HWS90InitializeMemoryController`, PROVEN undefined

Both builds start the controller with `WriteS90MC(0x0ff4, bank 3, ctrl | 1)`,
and in both `ctrl` is an **uninitialised variable**:

* `mtx_drv.so` `0xe5a2d`: `or $1, %r15d`; `%r15` is never written in this
  function, nor in its caller `HSLS90InitMemClk` (`0xdcef0`).
* `mtxpserm.sys` `0x53bbb`: `mov 0x18(%esp), %ebx; or $1, %ebx`.  At that point
  `0x18(%esp)` is the function's own argument (the device pointer); the config
  dword read just before lands in `0x10(%esp)` and is never used.

Two compilers made two different garbage values out of it and the vendor driver
works on every board, so only bit 0 (start) is significant.  **We write 1.**
INFERRED (that the other bits are ignored), PROVEN (that they are garbage).

### 3. Which hang classes need what

The lock-up report's `BM=` is `0x08f0`.  Measured signatures:

| hang | `0x08f0` | stuck unit | 0x0920 strobe could help |
|---|---|---|---|
| 2026-10-15 q3 copy mode (0528) | `00000000` | primary DMA not idle | no |
| 2026-10-15 glxgears immediate flips (0528) | `02000000` | WARP busy (bit 25) | maybe |
| 2026-10-16 engine-copy bench, twice (0528) | `00000000` | 2-D blit never drained (`DWGCTL 0x307` read back) | no |
| P690 multi-primitive packets | `03020000` (HANDOVER) | WARP store latched | maybe |

The `0x0920` bit-31 strobe is the only WARP-local gesture in any binary (the
Windows miniport brackets its WARP self-test with it); it cannot touch a
stalled primary DMA or 2-D unit.  A module reload fails in every class
because the WARP gate requires `0x08f0` bit 17 (DMA idle).  **The chip reset
is the only recovery that covers all of them.**

## What is lost and must be rebuilt after the strobe

Everything on the chip except the restored PCI configuration:
clocks and DRAM timings (rebuilt above), the engine control words
(`0x1c00 |= 0x00809020`; our engine init uses `0x00808020`, bit 12 unexplained),
the ring and writeback, the host-aperture descriptor bank, the block-state and
constant files, DWGCTL, the watermark (`0x12b8 = 0x00070601`, `0x12b0 = 0`),
the whole WARP instruction store (every resident program and every client's
slot), the display controllers and pixel PLLs, and on the P690 `0x1c50 =
PInS[0x19c]` (written by both vendor inits, never by ours).

**VRAM contents.**  The memory controller is reset and retrained (the 0528
training run writes the banks, `0x0f5c = 0x130` for 21 ms), so VRAM must be
treated as lost (INFERRED; nothing measured either way).  The memory
controller stays alive while the engine is hung, so the CPU can still read VRAM
through the host-aperture descriptors before the reset (INFERRED from the
architecture: the stalls never stopped CPU readback).

## Two ways to use it

**A. Recovery by module reload** (small, testable now).  At probe, if the
engine is latched (`0x08f0` bit 17 clear, or bits 24/25 set, with no client),
or on request (`recover=1`), run chip reset -> clocks -> memory controller ->
the normal probe.  VRAM loss does not matter at probe.  This turns "reboot the
Alpha" into "rmmod; insmod".  0528: wire up `parhelia_post_cold_bringup()`.
P690: add the SNX reset and the S90 clock/memory init above.

**B. Live recovery in the driver** (larger).  On a lock-up: stop scanout, fail
every queued fence (`-EIO`), save the pinned scanout and the other live VRAM
objects through the host aperture, reset and rebuild as in A, restore the
saved objects, re-upload the resident WARP programs, invalidate every client
WARP slot and the kernel state shadow, bump a reset counter that GET_PARAM
reports, and let Mesa re-emit its full state when the counter changes (it
assumes register state survives today).  Clients would see one lost frame.

## Risks

* `parhelia_post_cold_bringup()` has never run.  A wrong memory-controller
  write leaves VRAM corrupt (a reboot fixes it); an access to an unclocked
  chip can raise a machine check on the Alpha (the whole host goes down, and
  the user has to reset it).  The function never touches BAR0 before the
  controller is up, as the vendor requires.
* The 0528 DVI path above 154 MHz needed VBE boot state on x86 (memory note);
  the Alpha's DVI is fully programmed by our KMS (CH7301 over I2C), so the
  Alpha is the better first target.
* The P690 in nano does not survive a *warm reboot* (MMIO reads all ones,
  memory note parhelia-on-nano); that is the platform's bus reset and the
  VBIOS POST, not the PM strobe, which worked in the `reset_on_probe` run.

## Test plan, in order of risk

We now have a deterministic lock-up generator on the 0528: the engine-copy
bench (`BENCH=20 PARHELIA_ENGINE_COPY=1 copy-test 1280 720`, 2 of 2 runs).

1. **Read-only, before any reset:** after the next lock-up, dump `0x08f0`,
   `0x0e00`, `0x0e20`, `0x0e2c`, `0x0f24` per bank, and read back a VRAM
   pattern through the host aperture: proves the controller is alive while
   the engine is hung.
2. **The 0x0920 strobe** on a wedged board (two writes; no DMA, no memory
   access), then `0x08f0` again.  Expected not to help this hang class, but it
   is free on a board that needs a reset anyway.
3. **A on the 0528 (Alpha):** `recover=1` reload on the wedged board; check the
   `cold bring-up completed` line, the MC phase status, then
   `parhelia-drm-test --render/--texture`, copy-test, rtt-test, the scanout.
   Stop at the first failure and ask for the reset.
4. **A on the P690 (nano)**, after the S90 path is written, same checks plus
   `--native-transfer-probe` and `--masked-clear-probe` (the two that showed
   the `reset_on_probe` corruption).
5. **B**, once A is solid on both.

## Reproduction

```sh
so=/srv/t2/xf86-video-parhelia/matroxdriver-x86_64-cvs-20090511/xserver/7_4_0/mtx_drv.so
sys=/srv/t2/xf86-video-parhelia/xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserm.sys
objdump -d --no-show-raw-insn -C --start-address=0xe59d0 --stop-address=0xe6470 $so  # MC init, FS controller, IsScUseFS, system clock
objdump -d --no-show-raw-insn -C --start-address=0xdcef0 --stop-address=0xdd040 $so  # HSLS90InitMemClk (r15 untouched)
objdump -d --no-show-raw-insn $sys | sed -n '/53b72:/,/53bd2:/p'                       # Windows ctrl = [esp+0x18] | 1
```

PInS fields and the P690 PCI capabilities: `xf86-video-parhelia/tools/p690.rom`,
`vbios-matrox-parhelia`, `captures/p690-identity/config.bin` (the Python
used is in the 2026-10-16 session; offsets as in the tables above).
