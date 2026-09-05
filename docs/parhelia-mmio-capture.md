# Parhelia live MMIO capture (mmiotrace) — method + first captures

Captured on a working original Parhelia (0x0528) box running the vendor 2009 mtx
stack on kernel 2.6.30. This is the definitive way to capture ALL GPU register
access the vendor driver makes, for the open-source driver.

**Capture platform, exactly** (matters for every address below): `06:00.0 Matrox
Graphics Parhelia (rev 06)`, `Subsystem: Parhelia PCI 256MB`, BAR0 256MB
prefetchable @0xd0000000, BAR1 8KB @0xfebfe000, capabilities: Power Management
only. This is a **PCI/PCI-X board — no AGP, no PCIe**. Consequently all bus
addresses recorded here (0x404 payload, the 0x0000-0x0130 aperture bank) are
plain PCI bus addresses with no GART translation. AGP and PCIe boards use the
same register protocol but produce those addresses differently and are
**uncaptured** — see docs/parhelia-3d-driving.md "Address-space caveat".

## Method (works; the gotchas cost real time)

The 2.6.30 kernel has **mmiotrace** (`available_tracers: mmiotrace nop`). It
page-faults every access to `ioremap`'d regions and logs each MMIO read/write.

Steps (all as root):
1. `mount -t debugfs none /sys/kernel/debug` (not mounted by default).
2. `echo <kb> > .../tracing/buffer_size_kb` — use a MODEST size (16384). 256MB
   hung the box (OOM/thrash), and **32768 hung it too** (2026-09-05). 16384 is
   not a starting point to tune upward from; treat it as the ceiling.
3. `echo 0 > .../tracing/tracing_on` (record off), `echo mmiotrace > current_tracer`.
4. **`modprobe mtx` AFTER mmiotrace is on** — mmiotrace only hooks ioremaps done
   while it is active; a pre-loaded mtx is NOT traced (this cost a lot of time —
   empty traces). modprobe itself is fast.
5. Start X (`/usr/X11/bin/startx` — full path; PATH in a detached script lacks it).
   **Do NOT have tracing on while X starts.** mmiotrace page-faults every MMIO
   access and single-steps it; X's init does enough traffic to wedge the machine
   on its own. Arm tracing only for the narrow window you actually need, and
   never across a phase you are not measuring (2026-09-05: tracing armed across
   X startup with a 32 MB buffer took the box down).
6. Arm the WARP with a GL client (`glxgears` 3s) while `tracing_on=0`.
7. `echo 1 > tracing_on`; `echo MARK_START > trace_marker`; run the GL op to
   capture (a MINIMAL client: context + glBegin/glEnd/**glFlush** — NOT glFinish,
   which waits on the mmiotrace-crippled GPU and hangs; also skip glClear/swap).
8. `echo MARK_END > trace_marker`; `echo 0 > tracing_on`.
9. **`cp .../tracing/trace <file>` BEFORE `echo nop > current_tracer`** — switching
   the tracer RESETS the buffer (another time sink: empty traces).

Decode: base is the BAR1 MMIO map address from a `MAP` line; `reg = addr - base`.
Line fmt: `W 4 <ts> <mapid> <addr> <val> ...` (addr is field 5, val field 6).

## First capture: a single triangle draw (captures/mmio-draw-0528.txt)

Decoded per-draw sequence (reg = off from BAR1 base 0xfebfe000):

**Engine arm** (done twice - per the two draw calls):
```
R 0x0450            ; fence read
W 0x10b0 0x00000010
W 0x12b8 0x00070601 ; <-- the engine-arm value we RE'd, CONFIRMED live
R 0x0300 ; W 0x0300 0x1f0b0140
W 0x0384 0x0        ; DMAPAD (write-combining pad), NOT a soft-trap
W 0x0308 0x00000083
```

**Draw submission = the DMA RING (NOT direct 0x91c!):**
```
W 0x0314 0x1 (xN)          ; kick/trigger
R 0x08f0 ; R 0x0450        ; status + fence
W 0x0404 0x14600000        ; PRIMADDRESS = command-buffer bus addr
W 0x0400 0x177ff000        ; (a second ring ptr)
... DMA descriptor / pull-aperture setup at regs 0x0000-0x0130 ...
W 0x0920 0x1
W 0x0bc0 0x4087
W 0x0408 0x14600023        ; PRIMEND = end | 3  <-- the low-2-bits=3 ring trigger
W 0x0408 0x14600043        ; ... repeated, end advances by 0x20 (32B) per packet
W 0x0408 ... 0x146003c3    ; each write submits one command packet
```

**KEY findings for the driver:**
- The per-draw geometry+state travels in a **DMA ring command buffer** (bus addr
  0x14600000), submitted by writing **0x404 = start** then **0x408 = end|3**
  repeatedly (one write per ~0x20-byte packet). This MATCHES our RE'd DMA ring
  ([[parhelia-dma-ring-working]]: 0x408 = end|3). The 0x91c port is used only for
  the MICROCODE load at init, NOT per-draw geometry — correcting the earlier
  "geometry streams to 0x91c" reading.
- So the open driver's WARP draw = arm (0x12b8/0x10b0/0x300/0x384/0x308) + build
  the tokenised command buffer in a ring buffer + submit via 0x404/0x408 (the
  path we already have working for 2D fills), NOT the --warpstream 0x91c path.
- ~~STILL TO CAPTURE: the command-buffer CONTENTS~~ **CAPTURED (2026-09-05).**
  `tools/parhelia-mtxtrace.c` snapshots the mapped regions around every
  doorbell, which is where the ICD writes those packets, and `tools/gltri.c`
  reduces a whole 3D draw to 936 KB of trace. Both the ring packet and the
  record stream for a single triangle are decoded in
  `docs/parhelia-minimal-triangle.md`. The register side of the same draw is
  `captures/mmio-gltri-0528.txt`: 33 writes and 19 reads, no `0x404` and no
  `0x91c` — a 3D draw is one doorbell.

## Next captures to build the full GPU-access map
- Modeset (record modprobe+startx) = display bring-up sequence.
- The command-buffer contents for a draw (the tokenised packet stream).
- 2D accel ops, DDC, dual-head - each via the same method.

## Correction: 0x384 (2026-09-05)

`0x384` is **DMAPAD**, a write-combining erratum pad — NOT a soft-trap (corrected 2026-09-05 from the kernel disassembly). Rule: write `0x384 = 0` whenever the previous register write offset was within 128 bytes *below* the new one; a register *read* resets the tracker and suppresses the next pad.
