# Vendor capture tooling for the P690 context compiler

Copyright 2026 René Rebe <rene@exactco.de>  
SPDX-License-Identifier: GPL-2.0

## Scope

This is an inventory and a capture plan for the missing P690
post-`mtx950` context/dynamic-map evidence. It deliberately does not deploy,
replace, load, unload, trace with `mmiotrace`, replay, or otherwise change a
target driver. A capture is useful only on a machine already booted into a
known-good matching vendor Linux stack with direct rendering already available.
It is not a procedure for the current open-driver target at `192.168.1.111`.

The evidence needed is a vendor delta between an opaque RGBA texture and an
otherwise identical zero-alpha cutout. It must include the key-`0x0e` record,
the compiler input/output buffers, the per-submit command buffers, and the
dynamic destinations used by `mtx713`. See
`parhelia-p690-texture-alpha-context-re.md` for why that is more useful than a
register sweep.

## Saved material

| Item | Location | Use and limits |
|---|---|---|
| Linux 2009 vendor package | `matroxdriver-x86_64-cvs-20090511/` | The relevant P690-family reference: `kernel/mtx_parhelia.o`, `opengl6/lib64/libGL.so.1.3.0`, and matching `xserver/*/{mtx_drv.so,libglx.so}`. The kernel object retains symbols. |
| Linux kernel source and object disassembly | `matroxdriver-x86_64-cvs-20090511/kernel/src/` and `scratchpad/{mtx_parhelia_ko.asm,mtxk.asm,mtxk_r.asm}` | Static ABI recovery. `mtx950` is symbol/offset `0x14be0`; `mtx713` is `0xaaaf0`; `mtx3136` is `0x1b680`; `mtx558` is `0x44ef0`. These are object-relative, not runtime addresses. |
| Later M-series package | `m9xdriver-x86_64-1.4.3-80847-20140908-build_98/` | Useful for later display/DDX comparisons. It is not a substitute for a P690 capture. |
| Windows XPDM package | `xddm32_210_00_105_se_u_nowhql/` | Static cross-check: `MtxPar/MTXOGL.dll`, `MtxPar/MTXPARM.sys`, plus PCI-Express-server variants in `MtxPser/`. It is not runnable on the Linux capture host. |
| Static binary listings | `scratchpad/{ogl.asm,libGL.asm,libGL64.asm,pard.asm,mtx_drv.asm}` | Existing disassembly of both Linux GL ICDs, Windows OpenGL, and vendor DDX. |
| Working P690 Q3 record extract | `captures/ioq3-p690-working-draw.txt` | 36 KiB retained extract from the original 349 MiB vendor trace; it proves the record grammar and key `0x0e` image, but is insufficient to recover the internal `mtx950` destination map. |
| Synthetic vendor traces | `captures/{mtxtrace-5draws.txt,mtxtrace-textured-draw.txt,mtxtrace-textured-submits.txt,gltri*-0528-trace.txt}` | Known-good examples for checking new trace shape and `SUBMIT` framing. |
| State matrix and same-boot ring archives | `captures/gl-matrix/` and `captures/live-ring-0528/` | Historical differential inputs and offline decoding material. They are not P690 runtime evidence. |

## Existing capture executables and their safe use

### `tools/parhelia-mtxtrace.c` — the required instrument

Build this interposer on the already-vendor-booted capture machine:

```sh
gcc -O2 -fPIC -shared -o /root/tmp/parhelia-mtxtrace.so \
    tools/parhelia-mtxtrace.c -ldl
```

Run the vendor GL client with:

```sh
LD_PRELOAD=/root/tmp/parhelia-mtxtrace.so \
MTXTRACE_LOG=/root/tmp/<case>.trace \
MTXTRACE_BUFS=1 MTXTRACE_CPTR=1 \
<vendor-linked-client>
```

It interposes `open`, `open64`, `ioctl`, `mmap`, and `mmap64` for
`/dev/matrox`. With `MTXTRACE_BUFS=1`, it makes a pre-doorbell snapshot and
then records every changed mapped dword as `SUBMIT` data, preserving whether
the client, kernel, or both authored it. `MTXTRACE_CPTR=1` additionally dumps
the reachable request/pointer graph (`HEAPREQ` and `CPTR` records), which is
needed for in-band allocations. This is the only existing tool that captures
the data handed to the vendor context compiler without modifying that driver.

Do **not** set `MTXTRACE_MMIO=1` for the first P690 capture. It protects and
single-steps the small BAR mapping, which is useful for a targeted register
question but adds avoidable timing and signal-handler risk. The regular
direct-render path is expected to be fully represented by buffer/doorbell
traffic. `MTXTRACE_LOG` selects the output file; absent it defaults to
`/root/tmp/mtxtrace.log`.

Before accepting a trace, verify direct rendering in that process:

```sh
grep -c matrox /proc/<client-pid>/maps
```

The count must be seven rather than zero. A zero-mapping GLX client sends no
device calls itself, so an empty interposer trace is a topology failure, not a
negative hardware result.

`tools/parhelia-recdec.py <trace>` and `tools/parhelia-replay.c` are offline
consumers of this format. Use them only on copied output; do not run a replay
against the live vendor context. Existing documentation records that a replay
can poison subsequent vendor captures until reboot.

### Other tools: inventory and exclusions

| Tool | What it does | Status for this capture |
|---|---|---|
| `tools/mtxtrace.c` + `tools/mtxtrace-capture.sh` | Preloads into X and uses `SIGUSR1` to guard BAR1 accesses; `MTXTRACE_OUT` selects output and `MTXTRACE_WRITES=1` suppresses status reads. | Useful only for a narrow X-server MMIO question. It does not provide the vendor kernel's post-`mtx950` object and should not be combined with a Q3 collection. |
| `tools/gltest.c` | Small vendor GL stimulus with texture, depth, blend, alpha-test, and controlled texture/vertex-alpha inputs. | `GLTEST_CAPTURE_ALPHA=1` makes the one-unit, `GL_MODULATE`, fixed-clear, `GL_GREATER,.5` alpha-test draw required below. It never changes a driver. |
| `tools/capture-gl-matrix.sh` | Runs many `gltest` variants and dumps ring/secondary memory. | Historical 0528 harness. It runs `modprobe mtx`, starts X when needed, and defaults to `.56`; excluded from a no-driver-change P690 capture. |
| `tools/mmio-tex-capture.sh` and `tools/trace-gltri-start.sh` | Kernel `mmiotrace` workflows. | Excluded: they stop X and/or reload `mtx`; `mmiotrace` is structurally blind to the vendor DDX BAR mapping and has wedged prior systems. |
| `tools/same-boot-replay.sh`, `p3d-run.sh`, and `hw-run.sh` | Freeze/replay hardware test workflows. | Excluded. They are for open-driver experiments, not vendor observation. |

## What `mtx950` can be observed without patching the vendor driver

The saved module establishes a precise non-invasive dynamic-map observation
point:

```
mtx3136(ctx, device)
    source = mtx580(device, 0)       // copied 0xa0-byte stage record
    mtx950(ctx, source)
```

At entry, `mtx950` receives `rdi = ctx` and `rsi = source`. It invokes
`mtx713` eight times, with output subcontexts at `ctx + 0x78 + 0x60*n` and
source slices advancing by `0x74*n`. At `mtx713+0x40`, directly before its
four-byte copy, the normal x86-64 argument registers are:

```
rdi = dynamically selected destination
rsi = source + 4 * input_ID
rdx = 4
```

Therefore a tracepoint at that instruction yields the actual dynamic map:
`input_ID = (rsi - source_for_subcontext) / 4` and the target is `rdi` relative
to its `ctx`. The source record already contains the alpha fields at IDs 4/5/6,
so this produces the post-`mtx950` alpha routing exactly, without assuming that
an opaque destination is MMIO state.

The saved module has symbols, so a vendor-host operator can first establish
the runtime relocation without changing it:

```sh
grep -w 'mtx950\|mtx713\|mtx3136' /proc/kallsyms
```

If and only if tracefs is already available and the running module's symbol
addresses match this saved build, a short-lived dynamic kprobe may record the
entry arguments above. It is an observer, not a replacement module. The exact
tracefs event syntax must be checked on that capture kernel before use; do not
copy object-relative `0x14be0` or `0xaaaf0` into a probe definition. Remove the
events immediately after the single controlled client run.

This yields the dynamic-map snapshot. The workspace has **no existing
observer that dumps arbitrary vendor kernel memory** after `mtx950` returns.
`parhelia-mtxtrace.so` deliberately sees userspace mapped buffers and request
graphs, not the vendor module's private 0x858-byte context. A raw 0x858-byte
post-return snapshot would require a separate read-only kernel probe/BPF
observer, validated first on the known vendor capture host. It must not be
claimed from the present tooling or fabricated from the static offsets.

## Concrete safe capture sequence

1. Use a disposable P690 vendor-capture boot, never the open-driver test
   target. Confirm the vendor module, matching vendor X DDX, and matching
   vendor `libGL` are already loaded/running. Do not run package installers,
   `modprobe`, `rmmod`, `depmod`, `mmiotrace`, or any replay.
2. Compile only `parhelia-mtxtrace.so` in `/root/tmp`; this does not touch the
   driver. First run a single known-good `gltest` texture case under
   `MTXTRACE_BUFS=1`, and require seven `/dev/matrox` mappings plus non-empty
   `IOCTL`, `MMAP`, and `SUBMIT` records. Delete that trial trace only after
   copying it off-host.
3. Build `tools/gltest.c` on the capture host before the real collection:

   ```sh
   gcc -O2 -o /root/tmp/gltest tools/gltest.c -I/usr/X11/include \
       -L/usr/X11/lib64 -L/usr/lib64 -lGL -lX11 -lm \
       -Wl,--allow-shlib-undefined
   ```

   It now supplies the required minimal client. `GLTEST_CAPTURE_ALPHA=1`
   forces one texture unit, `GL_MODULATE`, a black color clear, and
   `GL_ALPHA_TEST` with `GL_GREATER, 0.5`; it also forces a texture and the
   `glColor4f` path. `GLTEST_TEXALPHA` and `GLTEST_VERTEX_ALPHA` accept only
   `0` or `255`. Run one fresh process for each of these otherwise identical
   cases (under the interposer in step 4):

   ```sh
   # RGB control: alpha test remains enabled; RGB texture alpha is implicitly 1.
   env GLTEST_CAPTURE_ALPHA=1 GLTEST_TEXFMT=1 GLTEST_TEXALPHA=255 \
       GLTEST_VERTEX_ALPHA=255 GLTEST_HOLD_US=200000 /root/tmp/gltest

   # Opaque RGBA texels.
   env GLTEST_CAPTURE_ALPHA=1 GLTEST_TEXFMT=0 GLTEST_TEXALPHA=255 \
       GLTEST_VERTEX_ALPHA=255 GLTEST_HOLD_US=200000 /root/tmp/gltest

   # Zero-alpha RGBA cutout texels.
   env GLTEST_CAPTURE_ALPHA=1 GLTEST_TEXFMT=0 GLTEST_TEXALPHA=0 \
       GLTEST_VERTEX_ALPHA=255 GLTEST_HOLD_US=200000 /root/tmp/gltest

   # Opaque texture with zero vertex alpha.
   env GLTEST_CAPTURE_ALPHA=1 GLTEST_TEXFMT=0 GLTEST_TEXALPHA=255 \
       GLTEST_VERTEX_ALPHA=0 GLTEST_HOLD_US=200000 /root/tmp/gltest
   ```

   Each draw calls `glFlush`, never `glFinish`, and retains the context for
   `GLTEST_HOLD_US` after the final draw. `GLTEST_CAPTURE_ALPHA=1` rejects
   `GLTEST_MULTITEX=1` and LUMINANCE input so the capture cannot silently
   become a paired-unit or non-alpha experiment. Existing `gltest` modes keep
   their previous controls.
4. Run one fresh process per case under `MTXTRACE_BUFS=1 MTXTRACE_CPTR=1`.
   Store the traces outside the live capture directory before inspecting them.
   Do not take screenshots as proof: retain the raw trace and record the
   visual result separately.
5. Decode the complete key-`0x0e` record in each trace, then compare: stage-0
   fields 1--6; output record offsets `+0x10/+0x14/+0x18`; every key `0x46`,
   sampler, and alpha-test record emitted in the same submission; and the
   `HEAPREQ`/`CPTR` inputs around the related doorbell.
6. If the driver-host tracepoint validation succeeds, repeat **one** opaque
   and **one** A=0 run while collecting only `mtx950` entry and `mtx713+0x40`
   destination/source pairs. Join them by context pointer and submission
   chronology to create the dynamic-map delta. This is enough to decide
   whether input IDs 4--6 route differently. Do not begin a raw-memory
   snapshot implementation until this lower-risk map is known.
7. Analyse offline. A Mesa or kernel experiment may only be proposed when the
   opaque/cutout delta both explains the visual difference and has an exact
   P690-safe packet/context form. Nothing in this sequence authorizes a
   normal-driver change or installation.

## Expected outcomes

* If fields 4--6 and the dynamic destinations differ while key `0x46`, sampler
  state, and `0x12e4` do not, the missing route is a context-side condition.
  The next work is a private diagnostic that reproduces the complete observed
  compiler output, not a global Mesa state-bit substitution.
* If key `0x46` or sampler state changes, that final packet delta is the
  preferred implementation target and must be decoded first.
* If all captured software-visible state is identical, retain the dynamic map
  and evaluate a read-only post-return context observer. The current tools do
  not prove a hidden register or validate the unsafe paired sampler mode.
