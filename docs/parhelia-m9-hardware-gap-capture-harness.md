# M9 bounded hardware-gap capture harness

This is the executable capture plan for the M9/Phoenix facts that cannot be
closed from the saved Linux and Windows binaries alone. It covers:

1. index-`78`, `request+0x90`, page-table-root, and physical-page ownership;
2. RT1, RT2, and RT3 fragment export plus resource/context retirement;
3. the physical occlusion-counter wrap rule and multisample scaling;
4. one auxiliary requester-page wrap and the hardware-owned
   `0x50c..0x524` transitions;
5. requester-address and 3-D decode fault IRQ identification.

No case runs automatically. `tools/m9x-hw-gap-capture.sh` runs exactly one
named case against the saved vendor GL stack. It has a fixed timeout, creates
a fresh artifact directory, samples MMIO read-only, and hashes its results.
The MRT-retirement, long-counter, and fault cases require distinct literal
arming phrases. The tools were built and checked offline; this document does
not claim that any of these new cases has run on hardware.

The harness sources are:

| file | role |
|---|---|
| `tools/m9x-hw-gap-probe.c` | public-GL stimuli and durable phase markers |
| `tools/m9x-exec-trace.c` | allocation, ExecuteDma, BM, mapping, and selected VRAM images |
| `tools/m9x-hw-gap-mmio.c` | bounded read-only requester, raw IRQ, fence, and memory-controller samples |
| `tools/m9x-one-shot-fault.c` | separately armed, single-ExecuteDma mutation |
| `tools/m9x-root-page-kgdb.py` | bounded kernel-side `request+0x90`, PTE, and physical-page observation |
| `tools/m9-hw-gap-capture-evidence.py` | offline integrity, phase, packet, MMIO, IRQ, and PTE validator |

## Common setup and capture invariants

Boot the M9148 host into the preserved vendor Linux installation and start its
working direct-rendering X server. Use the `xserver/7_4_0` vendor GLX pair
recorded in `parhelia-m9-linux-vendor-capture.md`. Obtain the current authority
path from the live X or xinit command line. On the known host the setup is of
this form:

```sh
export DISPLAY=:0
export XAUTHORITY=/root/.serverauth.CURRENT
export M9_GAP_MMIO_RESOURCE=/sys/bus/pci/devices/0000:03:00.0/resource2
```

Copy the listed C/Python sources and runner to one directory on the target and
set `M9_GAP_SOURCE_DIR` to it if the runner is elsewhere. Save captures on a
persistent filesystem, not `/tmp`, because `/tmp` may disappear on a recovery
reboot.

The MMIO observer opens the PCI resource `O_RDONLY|O_SYNC` and maps it
`PROT_READ`. Its raw IRQ inputs are low `0x8e0` and high `0x8f4`; `0x8e4` and
`0x8f8` are W1C acknowledgement ports and are intentionally absent. It also
records masks `0x8dc/0x8e8/0x8fc`, requester `0x400..0x524`, engine status,
memory-control `0x0f04`, and fence addresses. Reading does not acknowledge an
IRQ or change engine state.

Every GL operation that can wait has a marker flushed with `fsync` before and
after it. The MMIO stream uses periodic `fdatasync`, fault logs use
`O_DSYNC`, and the ExecuteDma tracer accepts `M9XTRACE_FSYNC=1`. Therefore the
last marker, pre-submit command image, and mutation description should survive
a power-cycle after a hang. The runner's `SHA256SUMS` covers every retained
non-binary artifact after a normal exit or timeout.

Do not run cases in parallel. Run each fault case in a fresh boot, preserve
the capture, then reboot even if the ioctl returns. The saved vendor stack has
no proven global or 3-D reset operation.

## Index 78 and physical-page ownership

The texture stimulus uploads a coordinate-coded 67x37 RGBA8 image, draws it,
updates a 19x9 rectangle which crosses the central tile/page region, draws
again, and reads the result. This provides nonrepeating bytes spanning more
than two 4-KiB pages.

First run without full mapping snapshots:

```sh
tools/m9x-hw-gap-capture.sh texture-root-discovery \
  /root/m9-captures/texture-root-discovery
```

Decode the trace and identify the one draw ExecuteDma sequence containing
index `78`, and the mapping index holding the request/system-resource object.
Then rerun one focused sequence:

```sh
export M9_GAP_EXEC_LIST=SEQUENCE
export M9_GAP_MAP_LIST=MAP_INDEX
tools/m9x-hw-gap-capture.sh texture-root \
  /root/m9-captures/texture-root-focused
```

The focused runner refuses a list of multiple ExecuteDma sequences. This
prevents a full-map snapshot on every startup submission. If the required
mapping is larger than expected, also set `M9_GAP_MAP_LENGTH` to its exact
length. The offline validator extracts every index-`78` value from the
selected BM stream.

The userspace mapping alone cannot expose the kernel CPU-copy request.
Load the vendor module symbols in KGDB and set a verified direct-map base for
this exact kernel. Do not guess the base. With the GPU process stopped at a
selected `M9_GAP_HOLD_PHASE`, source the bounded observer:

```gdb
set $m9_gap_dir = "/root/m9-captures/texture-root-focused/root-pages"
set $m9_gap_limit = 64
set $m9_direct_map_base = VERIFIED_X86_64_DIRECT_MAP_BASE
source tools/m9x-root-page-kgdb.py
continue
```

The breakpoint is `m9x101+0x5a7`, immediately after:

```text
pte_address = request+0x90 + 4 * (virtual_address >> 12)
```

At each hit it saves the complete `0xa0`-byte request, the selected PTE, a
JSONL record, and at most one copy of each valid decoded physical 4-KiB page.
It disables itself at the configured hit limit. Use these optional probe
holds to separate phases:

```sh
M9_GAP_HOLD_PHASE=texture-before-upload-finish ...
M9_GAP_HOLD_PHASE=texture-after-upload-finish ...
M9_GAP_HOLD_PHASE=texture-after-sample-finish ...
```

Continue the stopped process with `kill -CONT PID` after each host-side KGDB
snapshot. A complete ownership proof requires all of the following in one
allocation lifetime:

- the BM write to index `78`;
- its allocation response and selected local/aperture flag;
- request fields `+0x80`, `+0x90`, and flag byte;
- the exact PTE address equation checked by the offline validator;
- coordinate-coded bytes in the decoded physical pages before/after upload
  and after sampling;
- the old BAR view at both `index78-0x10000000` and the unmodified address.

The KGDB tool closes the request/root/page join. The two optional BAR views
can be added with `M9XTRACE_VRAM_ADDR` and `M9XTRACE_VRAM_BYTES` in separate
focused runs. Do not infer physical ownership merely because the old BAR view
contains PTE-shaped dwords.

## RT1 through RT3 export and bounded retirement

Each `mrt-rtN` case creates four RGBA8 attachments, binds only array slot N to
attachment N, emits distinct `gl_FragData[0..3]` colors, finishes once, and
reads only attachment N:

```sh
tools/m9x-hw-gap-capture.sh mrt-rt1 /root/m9-captures/mrt-rt1
tools/m9x-hw-gap-capture.sh mrt-rt2 /root/m9-captures/mrt-rt2
tools/m9x-hw-gap-capture.sh mrt-rt3 /root/m9-captures/mrt-rt3
```

Expected center values are green, blue, and yellow respectively. Capture and
decode all four surface descriptor pairs `b0..b7`, output-export state, the
index-`78` program/resource address, relocation records, and the common fence
writeback. A successful `mrt-after-finish` marker proves job completion; a
successful read proves that target's independent backing.

The default path calls `_exit(0)` and avoids driver teardown. Retirement is a
separate one-case experiment:

```sh
export M9_GAP_ARM_RETIRE=I_ACCEPT_BOUNDED_M9_MRT_RETIRE
tools/m9x-hw-gap-capture.sh mrt-rt1-retire \
  /root/m9-captures/mrt-rt1-retire
```

Repeat for RT2/RT3 only after preserving the previous result and rebooting if
needed. `mrt-before-context-destroy` followed by
`mrt-after-context-destroy` proves bounded context/resource retirement. A
timeout after the first marker localizes the old timeout-free close wedge
without losing the completed draw evidence.

## Query multisample scaling and counter wrap

The MSAA cases request an actual GLX visual with two, four, or eight samples
and refuse to proceed unless `GL_SAMPLE_BUFFERS==1` and `GL_SAMPLES` equals
the requested count:

```sh
tools/m9x-hw-gap-capture.sh query-msaa-2 /root/m9-captures/query-msaa-2
tools/m9x-hw-gap-capture.sh query-msaa-4 /root/m9-captures/query-msaa-4
tools/m9x-hw-gap-capture.sh query-msaa-8 /root/m9-captures/query-msaa-8
```

For each run preserve both query snapshot resources around the indexed
`1e/e6/e6/e6` records, their two raw dwords, and the returned GL value. Compare
the raw word-zero modulo-32 delta to the returned value. The saved ICD is
known statically to multiply the four-slot sum by the context sample count;
these cases determine the hardware counter's unit.

The wrap stimulus uses a 2048x2048 FBO and 1025 full-screen draws, for
`4,299,161,600` theoretical passing fragments. This is deliberately expensive
and has a 45-second process bound:

```sh
export M9_GAP_ARM_LONG=I_ACCEPT_BOUNDED_M9_COUNTER_WRAP
tools/m9x-hw-gap-capture.sh query-wrap /root/m9-captures/query-wrap
```

`M9_GAP_WRAP_SIZE` is limited to 4096 and `M9_GAP_WRAP_DRAWS` to 4096. The
probe rejects combinations that do not exceed `0xffffffff`. A timeout is
useful negative performance evidence, but it does not prove wrap semantics.
A decisive result needs begin/end raw snapshot word zero and its unsigned
subtraction, plus the public result. Do not replace this with many independent
queries; only one physical begin/end interval can prove a coherent wrap.

## Auxiliary requester wrap

`requester-wrap` submits 768 independently flushed triangle records and one
final fence:

```sh
tools/m9x-hw-gap-capture.sh requester-wrap \
  /root/m9-captures/requester-wrap
```

The draw count is bounded to 1..4096 with `M9_GAP_REQUESTER_SUBMITS`. The MMIO
observer records changes in all eight requester words. A decisive single-wrap
capture contains:

1. the quiescent pre-wrap tuple;
2. the last old-page `0x504` publication;
3. the `{2,new_gpu_base}` transition record in the old page;
4. the first new-page `0x504` publication;
5. the quiescent post-fence tuple.

The offline report lists every observed transition for `0x500..0x524`.
Correlate those timestamps with the tracer log and command pages. If the
default workload produces no wrap, increase submissions once within the
4096 bound. Do not infer names for `0x50c/0x510/0x514/0x520/0x524` from a
stable run.

## Requester and 3-D fault IRQ identification

Run `requester-wrap` first and select one ExecuteDma sequence which contains
exactly one draw record. The fault interposer refuses any other stream shape.
It records and synchronously flushes the original and replacement word before
calling the vendor ioctl.

Requester-address case:

```sh
export M9_GAP_ARM_FAULT=I_ACCEPT_ONE_M9_GPU_HANG
export M9_GAP_FAULT_EXEC=SEQUENCE
tools/m9x-hw-gap-capture.sh fault-requester \
  /root/m9-captures/fault-requester
```

This changes only the final range endpoint of the selected ExecuteDma to one
page beyond the mapped BM allocation. The kernel may reject it. Such a clean
rejection is valid evidence that the private ioctl validates this boundary;
it is not fault-IRQ evidence.

3-D decode case:

```sh
export M9_GAP_ARM_FAULT=I_ACCEPT_ONE_M9_GPU_HANG
export M9_GAP_FAULT_EXEC=SEQUENCE
tools/m9x-hw-gap-capture.sh fault-3d /root/m9-captures/fault-3d
```

This changes the sole known draw header's family to the unused
`0x05000001`, preserving launch bits and all addresses. The interposer restores
the CPU word if the ioctl returns.

For each case compare enabled raw IRQ assertions to its own pre-submit
baseline:

```text
active_low  = irq_low_raw  & irq_low_mask
active_high = irq_high_raw & irq_high_mask
```

The offline validator reports only newly active bits. It does not name them.
A source can be named requester-fault or 3-D-fault only when it is absent in a
control run, reproducibly appears in the matching single-fault run, and is
distinct from normal completion/service events. The observer never masks or
acknowledges a source. After a fault or timeout, save the persistent capture
and reboot; do not attempt the unproven global recovery entry points.

## Offline validation

Run from the repository root:

```sh
python3 tools/m9-hw-gap-capture-evidence.py --self-test
python3 tools/m9-hw-gap-capture-evidence.py \
  --capture /path/to/one-capture --output /path/to/one-capture/summary.json
python3 tools/m9-hw-gap-capture-evidence.py \
  --root-pages /path/to/root-pages
```

The first command audits the safety gates and offline parser. Capture
validation checks every SHA-256, required MMIO column, monotonic sample time,
durable phase markers, newly enabled IRQ bits, requester transitions, selected
index-`78` writes, MSAA visual agreement, and wrap-stimulus size. When a
`root-pages` directory is present it also proves each captured
`pte_address == request+0x90 + 4*(virtual>>12)`, rechecks the PTE decode, and
checks request and physical-page image sizes.

The generated `summary.json` distinguishes a completed phase from a timeout.
It is an index of evidence, not a conformance verdict. Update the feature
readiness documents only after the relevant raw result and its summary are
both checked.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0-only -->


## A fixture must not compute its own reference in the shader

MEASURED 2026-09-18.  A fixture that derived per-texel reference values from
`gl_FragCoord` arithmetic came back one 1/255 step low on **13415 of 61440**
texels.  The fragment unit is fixed point and cannot reconstruct exact 1/255
steps from coordinate maths, so the reference was wrong rather than the
driver.

Upload the traceable data and copy it 1:1 with a nearest filter instead.  Any
fixture that generates its reference in the shader will produce a false FAIL,
and a false FAIL costs more than a missing test because it sends the next
reader hunting a defect that is not there.

## A green suite does not prove the hardware ran

MEASURED 2026-09-18.  A Mesa build configured without
`--cross-file cross-t2-llvm.ini` links against the host libdrm and X
libraries; `libEGL_mesa` then fails `dri2_create_screen`, EGL falls back to
**llvmpipe**, and every fixture still reports PASS because the harness
compares against a CPU reference.  Assert `GL_RENDERER` is `Matrox M9` on
every run and count the cases that are not.

Related: a build of the shared Mesa tree compiles **every other agent's
uncommitted edits** into the binary under test.  One such build failed every
case, including untextured ones, with zero stream timeouts, while the shared
prefix passed at the same moment; rebuilding from `git archive HEAD` plus the
change under test alone restored the control to PASS.
