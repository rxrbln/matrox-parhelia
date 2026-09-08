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

## Attempt 2026-09-08: empty traces, and the gotcha the method text is missing

Ran the documented method to capture a **textured** vs an **untextured** draw on
one boot, so the diff would show anything the kernel writes inline that the DMA
ring capture cannot see. `tools/mmio-tex-capture.sh` follows every ceiling here:
16384 kB buffer, `mtx` loaded after the tracer, X started with `tracing_on = 0`,
trace copied before the tracer is reset.

**Both traces came back with the two markers and nothing between them.**

One real gotcha this file did not record, and it is now fixed in the script: the
distro loads `mtx` at boot, so `modprobe mtx` after arming the tracer is a
**no-op** and hooks nothing. `pkill -x X; rmmod mtx` first, and the reload is
genuinely traced (the script fails loudly if the module stays resident).

That was not enough. A control - arming `tracing_on` around `modprobe mtx`
itself - also recorded **zero** writes, so the module does no MMIO at init; the
`ioremap` evidently happens later, when the X server opens the device.

### RESOLVED: mmiotrace cannot see this driver at all

The follow-up experiment settles it. Arming `tracing_on` **across X startup**
with the 16384 kB buffer recorded `0 writes, 0 maps` - not merely no writes, but
**no ioremap was hooked at all** - while X came up normally. The reason is in
the X server's own address space:

```
2ba1642c4000-2ba1642c6000 rw-s febfe000 .../0000:06:00.0/resource1   BAR1, 8 KB registers
2ba1642c6000-2ba1742c6000 rw-s d0000000 .../0000:06:00.0/resource0   BAR0, 256 MB framebuffer
```

**The vendor X driver mmaps the PCI BARs directly through sysfs.** It never
asks the kernel to `ioremap` them - consistent with `lsmod` showing `mtx` at
refcount **0** while X is running. mmiotrace works by page-faulting
kernel `ioremap`'d regions, so it can see **nothing** of this driver's register
traffic, and every attempt to use it on a draw was structurally doomed. That
also explains why the DMA ring capture has always been the only instrument that
worked.

The right instrument for this stack is therefore a **userspace** equivalent:
`LD_PRELOAD` into the X server, intercept the `mmap` of `resource1`, hand back a
`PROT_NONE` alias, and log each faulting store from a `SIGSEGV` handler before
single-stepping it. That is a real piece of work - x86-64 store decoding for the
forms `mtx_drv.so` emits - but it is the only way to get a
textured-vs-untextured register diff, and it would supersede the ring capture
for every future question.

What `captures/mmio-draw-0528.txt` (105 writes) actually recorded is therefore
**unexplained** and should not be relied on until reproduced; it cannot have
been this driver's own BAR1 stores.

---

Superseded reasoning, kept for the record: with the
tracer current across `modprobe`, X startup and the GL client, a GL draw
produced no kernel MMIO at all, while `captures/mmio-draw-0528.txt` from the
original session holds 105 writes including `0x10b0`, `0x12b8` and `0x0308`.
Either the draw path is entirely userspace MMIO on this configuration (the
vendor stack does map registers into the client - see
`parhelia-mesa-re-targets`), or the earlier capture was taken under conditions
this run did not reproduce. **Do not treat either explanation as established.**
Resolving it is the prerequisite for using mmiotrace on the texture question at
all, and it is cheap: arm `tracing_on` across X startup with the 16384 kB buffer
and see whether the ioremap and its traffic appear.

## The userspace tracer (tools/mtxtrace.c) — mechanism proven, not yet complete

Since mmiotrace is structurally blind here, `tools/mtxtrace.c` does the same job
from userspace: an `LD_PRELOAD` that intercepts `mmap`, recognises the
`.../resource1` mapping, and on demand makes it `PROT_NONE` so every register
access traps. It needs **no instruction decoding**: on `SIGSEGV` it records the
address and the write bit from the page-fault error code, makes the region
accessible and sets the x86 trap flag; on the following `SIGTRAP` the access has
retired, so simply reading the location recovers the value.
`tools/mtxtrace-capture.sh` drives it for a textured and an untextured draw.

Two findings that were not obvious and are worth keeping:

1. **The GL client maps nothing.** `/proc/<gltest>/maps` has no `resource0`,
   `resource1` or `/dev/mem`, so every register write is the **X server's**.
   The tracer belongs in X, not in the client.
2. **Xorg installs its own `SIGSEGV` handler during init**, long after
   `LD_PRELOAD` runs, silently replacing one installed at load time - the first
   trap then looks to Xorg like a crash and it exits. Install the fault
   handlers when **arming**, not at load.

State: with that fixed the handler fires correctly and the fault is in the
register window -

```
  # MAP resource1
  # ARM
  #S 2b6915baec4c AI      <- armed, In region: X really is writing BAR1
  #S 000015baec4c Ao      <- the re-executed access, address truncated to 32 bits
```

so the trap side is proven. The **completion** side is not, and one plausible
cause has already been eliminated: unblocking `SIGTRAP` and `SIGSEGV` in the
resuming `uc_sigmask` (Xorg blocks signals around critical sections, and a
*synchronous* trap delivered while blocked is fatal rather than deferred)
changes nothing - the same two-fault pattern repeats, with the second address
being the first truncated to 32 bits and no `SIGTRAP` in between.

**The faulting instruction is now identified.** Logging `REG_RIP` gives
`mtx_drv.so + 0x4a785` for the in-region fault, which disassembles to

```
  4a77b:  89 db              mov    %ebx,%ebx          ; zero-extend the offset
  4a77d:  48 8b 40 20        mov    0x20(%rax),%rax
  4a781:  48 8b 40 40        mov    0x40(%rax),%rax    ; -> the BAR1 pointer
  4a785:  8b 04 18           mov    (%rax,%rbx,1),%eax ; the register READ
```

so the trap genuinely catches the driver's register access, and the access form
is a plain 32-bit load with the offset in `%rbx` and the mapping pointer reached
through two indirections. The second, out-of-region fault has a `REG_RIP` in a
**different module** entirely, so it is not this instruction restarting.

That truncation is the thread to pull. It is reproducible across boots with
different mapping bases, so it is not noise: the re-executed access computes a
32-bit address. Either the trap flag is not taking effect and X then faults
genuinely for an unrelated reason, or the store is being restarted with a
clobbered pointer. The next step is to log `REG_RIP` in both handlers and
disassemble the faulting instruction in `mtx_drv.so` - that says immediately
which of the two it is, and it needs no hardware run beyond one capture.

Also closed off on the way: **the 3D register file is not readable**, so a
before/after register snapshot cannot substitute for a trace. `--readreg 0x1300
0x1310` returns `000020c0` for every register in the range - a single latched
value, not per-register state.
