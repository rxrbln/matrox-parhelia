# M9 initialization findings and tracing harness

Status as of 2026-09-15: the clean-room driver still has no completed M9
triangle. A raw-DMA class-2 secondary retired; the equivalent translated
class-1 secondary stalls with secondary writeback `0x10` and primary
writeback `0x21`. Matching CALL framing alone has not resolved translation.
The current task is to capture vendor initialization before its first GPU
submission, compare the actual sequence, and fix the demonstrated differences.

## Established findings

* The low-VRAM root is **not a live fixed MMIO window**. `m9x565` sets
  request flags `0x16`; `m9x1102` bypasses both `m9x919` and `m9x716`, leaves
  the fixed slot at `-1`, and uses its dynamic list. The fixed-table
  `m9x115` passes cannot republish that root. See the
  [exact branch analysis](parhelia-m9-cold-transition-submit-decode.md).
* Cold `m9x421` transitions use MMIO. Its command-emitting branch depends on
  a nonzero requester reference count and must not be injected into a fresh
  service. Bus mastering is available before service initialization.
* `m9x926` has transitive PTE/MMIO effects. Applied `System\\MmFlags`
  controls mode-0 and mode-1 publication; the separate hardware capability
  word is not that property. The four mode-0 pools publish identity PTEs,
  each followed by `31c=f,1,0`, after requester arm. They do not use the
  mode-1 helper's extra `314` publication. These corrections are implemented
  and were tested, but the class-1 secondary still did not retire. See the
  [pool test](../captures/m9x-mode0-pools-20260915/README.md).
* The class-1 primary CALL is exactly `45452d2c`,
  `(BmBuffer.GPUVA + 8 + start)|1`, and the clamped exclusive end `|2`.
  The raw cookie at BmBuffer `+0x48` is a distinct field. The actual class,
  extent and GPUVA are at `+0x34/+0x40/+0x44`. See the
  [start-tag evidence](parhelia-m9-secondary-start-tag-re.md) and
  [address-domain audit](parhelia-m9-secondary-address-domain-audit.md).
* Writeback ownership is secondary group B at `106c` (wb0), primary
  token A at `1068` (wb1). Both begin at `0x10`, with initial target `0x20`.
  The primary CALL precedes the `4545421a` fence; observing wb1 advance to
  `0x21` does not prove that the secondary retired.
* Address decoding must retain the observed address domains. Root register
  `360 & 0xfffff000` gives its byte base; preserve control bits `360 & 0xc`.
  The `f74/f78` leaf base is used without an invented offset. Generic PTE
  physical addresses decode as `(pte & 0xfffff000) | (((pte >> 4) & 0xff) << 32)`;
  validity is bit 3. The tested compact mapping for GPUVA `0x40024000`
  uses root slot `0x863404`, root value `0x901008`, and leaf `0x901090`.
  These are observations for that allocation, not universal physical addresses.
* `0x8f0[24]` already toggles before the vendor module or X starts. This
  directly refutes using that observation alone as evidence of class-1
  dispatch or a translation fault. The pre-X high IRQ word at **8f4** was
  `00007100`; the preceding failed clean-room boot was observed at
  `02007100`. Windows explicitly includes the differing bit 25 in its
  high-pending clear mask, but no recovered handler identifies it as a
  translation fault. Compare the same lifecycle boundaries before drawing
  conclusions. See the [bit-25 audit](parhelia-m9-high-irq-bit25-audit.md).
* `132c/1330/1334` are recovered translation configuration writes, not
  identified fault-address/status registers. Zero readback is insufficient
  evidence that their earlier writes were lost.

The [pre-X baseline](../captures/m9x-cold-init-20260915-v1/README.md) preserves
boot ID, PCI state, configuration, sampler source/binary and vendor hashes.
PCI bus mastering is already enabled at this baseline. This is a
post-firmware service-start observation, not a trace of power-on firmware.

## Actual old-host capabilities

The preserved target is `root@192.168.1.56`, running `2.6.30-dist` and Xorg
1.5.2 with the vendor X 7.4 driver/GLX pair. Its boot ID for this capture is
`aab643c8-e10f-49bb-848a-09c4846d34d1`.

Both `/boot/kconfig_2.6.30-dist` and `/boot/kconfig_2.6.34.1-dist` disable
`CONFIG_MMIOTRACE`, `CONFIG_KPROBES` and `CONFIG_KGDB`. The saved
`/root/tmp/mmiotrace.txt` contains only a `nop` tracer header. The historical
`mtxtrace` preload uses userspace page protection and single stepping; it
does not trace kernel MMIO, and its write readback cannot reliably recover
values sent to write-only registers.

Host `/usr/bin/gdb` lacks Python. A private GDB 17.2 was built from the
cached `/srv/t2/download/mirror/g/gdb-17.2.tar.xz`, installed into
`/tmp/m9-gdb`, and verified with `python import gdb`. Target gdbserver is
7.1; use its TCP interface rather than assuming newer `--once` or stdio
options. The prepared kernel source tree is pruned, so the module observer
does not depend on building a new kernel or external module from its headers.

## Three complementary observers

| Observer | Captured evidence | Important limit |
| --- | --- | --- |
| [Private vendor-module observer](../tools/m9-cold-kprobe/README.md) | Kernel `m9x1019` writes, service phases, publication inputs, four pool returns, native CALLs, table/MMIO snapshots | Image-specific instrumentation; bounded arrays and snapshots |
| [X MMIO/PCI observer](../tools/m9x-xinit-mmio-gdb.py) | Actual dword/byte accessor arguments, both register spaces, PCI writes, hardware-enable/init phases | Exact vendor X 7.4 image and dynamically resolved accessors |
| [X port-I/O observer](../tools/m9x-xinit-pio-gdb.py) | OUT instructions in pinned Xorg/int10/vgahw, emulated port-write handlers and BIOS phase | Does not claim coverage of other DSOs or firmware executed before Linux |

The older [KGDB observer](parhelia-m9-cold-service-capture.md) remains a
validated alternative for a target kernel with KGDB. Its 12 offline tests
pass, including native CALL ownership, nonidentity roots and stopping before
the selected doorbell. It cannot run through KGDB on the current old kernel.

### Reproducing the X observer setup

Build a sysroot containing the exact target Xorg, loader, required libraries
and the pinned `m9x_drv.so`, `libint10.so` and `libvgahw.so`. Preserve the
`/usr/X11R7` alias used by the target. Start target X under gdbserver before
connecting; do not continue the inferior until both observers are armed:

```sh
# Target; X must not already be running.
gdbserver 127.0.0.1:2345 /usr/X11/bin/Xorg :0 -noreset -nolisten tcp -ac vt7
```

Forward host port 22345 to target loopback port 2345 over SSH. Start the
Python-enabled host GDB, substituting actual host paths and NEW output files:

```gdb
set pagination off
set sysroot /tmp/m9-vendor-sysroot
file /tmp/m9-vendor-sysroot/usr/X11/bin/Xorg
target remote 127.0.0.1:22345
set $m9_xinit_image = "/HOST/xf86-video-parhelia/m9xdriver-x86_64-1.4.3-80847-20140908-build_98/xserver/7_4_0/m9x_drv.so"
set $m9_xinit_output = "/HOST/NEW-CAPTURE/xinit-mmio.jsonl"
set $m9_xinit_limit = 200000
source /HOST/xf86-video-parhelia/tools/m9x-xinit-mmio-gdb.py
set $m9_xinit_sysroot = "/tmp/m9-vendor-sysroot"
set $m9_xinit_pio_output = "/HOST/NEW-CAPTURE/xinit-pio.jsonl"
set $m9_xinit_pio_limit = 100000
source /HOST/xf86-video-parhelia/tools/m9x-xinit-pio-gdb.py
```

After the temporary observed module is loaded, use `continue`. Image hashes
and live instruction checks must pass. Each script stops the inferior and
disables its observers on errors or its configured limit; inspect the log
before continuing. Do not re-source a running observer to reset its state
or overwrite its output. Per-write remote breakpoints make initialization
slow; a slow-running trace alone is not evidence of a wedged GPU.

### Kernel observer invariants

The original installed module SHA-256 is
`f922f867c59dda24678110e111f2bc1d5920af2ea494cef76ab280a5fc3fc31a`.
The instrumenter refuses another image or mismatched hook instructions.
Assembly thunks preserve all 15 general registers and RFLAGS, align their
temporary C-call stack, replay displaced instructions, and resume the
original vendor code. The actual MMIO store remains the vendor instruction.
Original relocations are retained; newer `R_X86_64_PLT32` call relocations
are converted to equivalent `PC32` entries accepted by the old loader.

No extra VRAM mapping is created. `m9x386` calls
`KernelClientMapLocalLinMemory` and places the resulting mapping at accessor
`+0x30`; accessor `+0x28` is MMIO. `m9x798` initially sets `+0x30` to `-1`.
The observer records the mapping request/length and reuses the valid vendor
pointer. This avoids a second uncached alias conflicting with the vendor's
write-combining setup.

Snapshots preserve the root before the first `31c`, compact tables/MMIO
before the first `404`, and a separate table/MMIO image before the first
`404` from the builder that constructed a class-1 CALL. The native CALL hook
is `m9x830+0x290`, after its third primary dword was stored. RAX points to
that record, RBX identifies the builder, and `*(rbp-0x58)` is its BmBuffer.
The snapshot does not prove that the subsequent GPU submission completed.

### Dumping the retained buffer

Only experimental `/tmp/*.ko` files may be loaded. Never install the
instrumented vendor copy or the clean-room module in `/lib/modules`.
The observer keeps the module name `m9x`; start with the ordinary vendor
module unloaded and X stopped. Preserve the module until its buffer is dumped.

The current capture object is `0x11ce138` bytes. Resolve the live
`m9cold_capture` symbol from `/proc/kallsyms` and confirm its size against
the actual instrumented object before reading it:

```sh
m9x-kcore-read VERIFIED_CAPTURE_ADDRESS 0x11ce138 NEW_CAPTURE.bin
```

[m9x-kcore-read.c](../tools/m9x-kcore-read.c) derives the file offset from
the live `/proc/kcore` ELF program headers. It bounds reads to 32 MiB,
requires a new output file and fsyncs the result. It was tested on this
target against 16 bytes of `sys_read` at `ffffffff80293911`; the bytes
matched the saved `vmlinux` exactly. Ordinary `/dev/kmem` reads rejected
this high address with `EINVAL`, despite successful seeking.

## Running and validating a capture

1. Preserve the clean boot, PCI/configuration state, module hashes and
   read-only raw IRQ/MMIO baseline before loading the vendor module or X.
2. Build and inspect the private instrumented module; verify displaced
   instructions, relocations, mapping bounds and capture layout.
3. Start X under gdbserver, initially stopped at loader entry. Connect the
   private host GDB through the SSH tunnel, using exact target X/loader
   files in `/tmp/m9-vendor-sysroot`. Source both X observers before the
   first continue. Their source headers contain the required settings.
4. Load only the reviewed `/tmp/m9x-cold-observer.ko`, then allow X to
   initialize under the observers. Retain the X log and debugger logs.
5. Capture one vendor flat-triangle submission with the existing
   [ioctl/command tracer](../tools/m9x-exec-trace.c). Verify completion and
   rendered output separately from the pre-doorbell snapshots.
6. Dump the kernel buffer while quiescent. Check header/version/size,
   snapshot state, counts, drop indicators and record sequence numbers.
   A dropped record or absent snapshot is a gap, not successful coverage.
7. Join actual CALL addresses to the corresponding root/leaf contents,
   compare each ordered initialization write with the clean-room driver,
   and document differences before changing driver behavior. Keep kernel
   TSC and host debugger timestamps distinct; they are not a calibrated
   global clock.

## Live checkpoint, 2026-09-15

The private module is now **loaded**, from `/tmp/m9x-cold-observer.ko`, with
SHA-256 `ebc095b9262dee3b68feb863dc7689e1182d1382d3a6f50882ccb469e8697ea3`.
The installed vendor module remains unchanged. On the recorded boot,
`m9cold_capture=ffffffffa05138e0` and its size is `0x11ce138`; resolve them
again after any reload. X PID 5032 is running under the observers and has
produced more than 30,000 MMIO/PCI JSONL records, including repeated `304`
writes and subsequent `318`/`1c4c` writes. This is an in-progress trace,
not a completed initialization, decoded kernel dump, or rendering proof.

The initial observer hit a Python logging error before the first observed
PCI write: `emit() got multiple values for argument 'kind'`. GDB stopped the
inferior before the accessor executed. The field was corrected to
`access_kind`; only `bind_accessor` was redefined in the paused debugger,
preserving the existing state and output. Observers were re-enabled and
execution continued. The JSONL retains `capture-stop`, `capture-resume`
(with `writes=0`) and `accessor-bound-recovered`; do not remove that history.
No write was skipped during this paused recovery. The regression test
executes the real callback code under a fake GDB, covers all six vtable
bindings and concrete accessors, and checks disabling at the write limit:

```sh
python3 tools/test-m9x-xinit-mmio-gdb.py
python3 tools/test_m9_cold_service_observer.py
```

The PIO stream at this checkpoint contains setup plus the VBE `4f03`
get-current-mode phase, with no physical OUT event observed yet. Absence
of events does not establish complete port-I/O coverage. X startup has
reached vendor framebuffer mapping; first vendor triangle and retained
kernel-buffer validation are still pending.

Remaining work is to finish the capture, validate bounded-record coverage
and snapshot completion, join the first native class-1 CALL to actual page
tables and doorbell, then compare the ordered vendor writes with the
clean-room driver. The fixed-window-root theory is refuted; the exact
remaining cause of translated fetch failure is **not established**.
