<!-- Copyright 2026 René Rebe <rene@exactco.de> -->

# M9/Phoenix clean-room Linux DRM and Mesa driver specification

This document is the implementation contract for a new driver for Matrox M9
(`102b:0540`, M9120/M9138/M9140/M9148).  It consolidates the static Linux and
Windows binary analysis with the September 14, 2026 M9148 Linux vendor-driver
captures.  It deliberately describes a new DRM and Gallium interface rather
than reproducing the insecure private `m9x` ABI.

Confidence terms used below are:

* **closed**: exact enough to implement from binary data flow, a hardware test,
  or a captured vendor command stream;
* **validation**: an implementation formula is known, but has not yet passed a
  controlled test on the M9148;
* **blocker**: information still needed for a hardware-correct implementation;
* **optional**: may remain disabled without blocking a useful first driver.

The central architectural rule is strict: M9 and Parhelia may share DRM, GEM,
KMS, scheduler, and packet-builder infrastructure, but they must not share
engine register meanings, state shadows, shaders, relocation policy, requester
control, draw records, or reset code.  The two engines are not command
compatible.

## 1. Deliverable and initial capability boundary

The first mergeable M9 implementation consists of:

1. atomic KMS for the two physical heads, native DVI/DAC outputs, EDID, vblank,
   linear scanout, software cursor, and mini-DP only where the external
   transmitter sequence is available from PInS;
2. GEM objects in visible VRAM and DMA32-coherent system memory, with immutable
   placement during initial bring-up;
3. one kernel-owned requester queue, validated and relocated M9 command lists,
   memory-writeback fences, absolute timeout bounds, and fail-closed recovery;
4. a Gallium backend with CPU vertex transformation initially, linear render
   targets, ordinary Z/stencil, scissor, clears, vertex/index buffers, four
   texture units, mipmaps, basic blend/alpha/depth state, and a minimal native
   vertex/fragment program path;
5. explicit feature gates for every path still listed as a blocker below.

PCI ID `0540` should remain behind `m9_enable=1` until the no-op fence, clear,
triangle, A/B/A context, and modeset tests in section 12 pass.  This is a test
gate, not an architectural requirement.

The current evidence is sufficient to write and validate this initial path.
It is not yet sufficient to claim a complete OpenGL 2.0 driver.  The external
resource/control pairs, atomic linked vertex/fragment selection, and constant
ranges are joined, but index `69` binds the external fixed-fragment database
with unresolved address translation and index `6c`
selects one opaque immutable linked VS/FS pair.  There are no public
programmable start/count, per-stage entry, or end-PC fields in that path.
Private 0x3158-byte whole-pipeline allocation, keyed cache reuse,
copy-on-write validation and commit-like publication are exact.  Arbitrary
programmable shaders remain gated by the internal pair byte serializer,
graph construction and scheduling beyond the pinned control recipes, hardware
stop semantics, and the remaining operation-specific linked-resource forms
listed in `parhelia-m9-driver-readiness.md`.

## 2. Kernel source structure and shared boundaries

M9 is an independent DRM driver under `drivers/gpu/drm/m9x`; it must not share
source files, private headers, objects, register definitions, packet
validators, or chip dispatch with `drivers/gpu/drm/parhelia`.  Generic DRM,
GEM and scheduler helpers are the only shared implementation boundary.  The
current source layout is intentionally M9-specific:

```text
m9x_drv.c                     PCI, device lifetime and DRM registration
m9_drv.h                      private device/BO/requester state
m9_reg.h                      M9 MMIO and indexed-register definitions
m9_gem.c                      DMA32 GEM objects and mmap plumbing
m9_requester.c                primary requester and owned no-op producer
m9_submit.c                   M9 parser, whitelist and typed relocations

planned independent files:
m9_bios.c                     option-ROM/PInS 8.0 reader and parser
m9_kms.c                      M9 CRTC, output, PLL, EDID and vblank
m9_memory.c                   M9 VRAM/window/address-class handling
m9_fence.c                    writeback sequence and dma_fence integration
m9_irq.c                      M9 status/mask/W1C banks
m9_context.c                  context images and ordered restore
m9_reset.c                    bounded requester-local recovery only
```

The independent module is named `m9x_drm.ko`, exposes DRM driver name `m9x`,
and claims only `102b:0540`.  `parhelia.ko` has no M9 PCI alias or source
dependency.  The M9 submit parser uses its own opcode table, index whitelist,
relocation kinds, state limits, and draw-record decoder.

The kernel owns all MMIO, physical or DMA addresses, requester pages, fence
memory, state-restore images, and command execution.  Userspace supplies typed
commands, GEM handles, offsets, and access flags.  Render nodes never map BAR
MMIO or learn system bus addresses.

## 3. PCI, board discovery, PInS, and display

### 3.1 Probe and address spaces

The following is closed:

* Match PCI vendor/device `102b:0540`; derive the product from subsystem ID and
  PInS rather than inventing separate PCI IDs.
* Call `pci_enable_device()`, request the regions, and enable
  `PCI_COMMAND_MASTER` before requester initialization.
* BAR0 is the framebuffer aperture.  It is a 256 MiB window on the tested
  M9148 and is not the installed-memory size.
* MMIO is BAR1 unless BAR0 is a 64-bit BAR, in which case PCI resource slot 2
  contains the MMIO BAR.  Validate resource flags and a minimum register span
  rather than assuming a fixed slot.
* Engine-visible system allocations use a 32-bit coherent DMA mask.  Reject
  render support if `dma_set_mask_and_coherent(..., DMA_BIT_MASK(32))` fails.
* Treat `readl(0x8f0) == 0xffffffff` as removal/disconnect in every wait.

Installed VRAM comes from PInS `+0x44`: sum four four-bit bank codes using
`{0,32M,64M,128M,256M,512M,1G}`; `0xffffffff` means 512 MiB.  Keep installed
memory, the BAR0 window, and the currently accessible local window as three
different quantities.

### 3.2 PInS 8.0

PInS version `>= 0x0800` is required for a full M9 board description.  The
near pointer is at flash offset `0x7ffc`; the image has signature `0x412e`,
length at `+2`, version at `+4`, and no checksum for version 8.0.  Parse and
retain at least:

| PInS offset | purpose |
| ---: | --- |
| `0x0c` | output-descriptor table offset |
| `0x0e/0x10` | register-patch table start/end |
| `0x44` | memory-bank configuration |
| `0x50/0x54` | lane-to-signal map |
| `0x5c..0x68` | four connector-to-link dwords |
| `0x6c/0x70` | transmitter-reset GPIO maps |
| `0xa8/0xac` | DAC pad trims |
| `0x120..0x140` | connector DDC selections |

Patch entries are `u16 reg, u8 flags, u8 clksel`, followed by `data`, `mask`,
and `alt_data` when flag bits 0, 1, and 2 are set.  Their stride is
`4 + 4 * popcount(flags & 7)`.  Choose `alt_data` for clock selector 1 above
100 MHz or selector 2 above 200 MHz, then apply
`new = (old & ~mask) | data`.

The saved Linux wrapper enables the PCI ROM BAR at the framebuffer base, copies
128 KiB, and passes that copy to the binary core.  A new driver should first use
`pci_map_rom()`, validate the pointer, signature, length, and version above, and
copy only the bounded PInS image.  This is sufficient when the PCI ROM image
contains the M9 PInS, as the vendor wrapper expects.

The Linux X driver's `HLPInS80_GetBoardInfo` path is now statically joined to
communication mode zero, its serial-flash device probe, the `0x7ffc` pointer,
`0x412e` signature, and bounded payload copy.  This proves that the vendor can
obtain PInS directly from the board store rather than executing VBIOS.  The
lowest GPIO/MMIO serial transaction has not yet been reduced to a clean kernel
routine.  If the standard PCI ROM image is absent or does not contain a valid
PInS, accept a validated firmware copy and refuse cold board-specific output
initialization.  Do not scan or drive unknown MMIO as a substitute.  See
`parhelia-m9-cold-init-re.md` for the call chain and pinned evidence.

### 3.2.1 Cold reset, system clocks, and memory controller

The Linux kernel vendor module does not POST the GPU.  Its connect path maps
resources, copies the PCI ROM, creates software objects, and starts host IRQ
handling.  The Linux X driver owns the cold branch behind an initialized-state
test.  For `0x0540` it directly performs, in order:

```text
save PCI/MMIO state and execute the 0x358 reset/configuration protocol
disable VGA/display and hold all four display clocks
mask/ack interrupt banks
hold all four display controllers
apply PInS-patched 0xfxx defaults
program the 27 MHz-derived system synthesizers
program the direct and indirect memory-controller image and enable pulses
destructively verify almost 512 KiB of VRAM
restore the outer display/controller state
```

Exact register values, masks, delays, indirect-register defaults, board flag,
and error behavior are closed in `parhelia-m9-cold-init-re.md`.  The vendor
retries the whole transaction up to 2000 times and then incorrectly marks the
device initialized even on failure.  The new driver must use bounded polling,
retain a failed state, keep outputs/requester disabled, and return the failure.
Cold MC setup does not arm the requester or modeset an output; those remain
separate phases.

### 3.3 Atomic modesetting

M9 exposes four controller blocks, but controllers 2 and 3 are secondary
planes paired with physical heads 0 and 1.  Expose two DRM CRTCs first.  The
closed register maps are:

```text
DC control bases: 1800, 1a00, 1980, 1b80
timing bases:     1900, 1b00, 1e00, 1f00
clock selects:    1c00, 1c08, 1c04, 1c0c
watermarks:       1c14, 1c1c, 1c18, 1c20
```

For controller base `C`, program start at `C+0x48`, interlace second field at
`C+0x4c`, and pitch as pixels plus `3<<16` plus format at `C+0x58[31:24]`.
The format codes are `0x60/0x9c/0xcc` for 8/16/32 bpp.  Set enable and format
at `C+0x00` last; clear it first on disable.  The exact timing packing, hold
sequence, VGA-to-native handoff, video-reset strobe, and watermarks are in
`parhelia-m9.md` and are closed by hardware tests.

The pixel PLL uses a 27 MHz reference and:

```text
Fout = ref * b0 * b1 / b2 / (2*b3)
word = (b0-1) | ((b1-1)<<6) | ((b2-1)<<11) | ((b3-1)<<16)
```

Program `word|BIT(31)|BIT(21)`, then `word`, and poll bit 24 with a deadline.
The usable synthesizer registers are `0xe30/34/38/3c/40/44`; entries 4 and 5
do not have registers.  Clock selection is hold in bit 0 and source in bits
4:1; the hardware-proved working relationship is source `fs+2`.

Never copy the classic P690 watermark writes to M9: classic offsets `0x1c0c`
and `0x1c10` alias M9 clock/reset controls.  Set genlock `0x1c28` once to
`0x0f021084`; do not rewrite it in each atomic commit.

### 3.4 Connectors, EDID, DP transmitter, cursor, and gamma

DAC source values are `0/1/4/5`; digital-link source values are
`0x00/0x08/0x01/0x09`.  Digital links are at `0x1c9c + 4*link`; use PInS to
select the link and its clock-dependent companion patches.  DDC uses bit-bang
registers `0x1c48` and `0x1c4c`.

M9148 mini-DP requires the external transmitter sequence in
`parhelia-m9.md`.  Take the HMC mutex through `0x318` and poll grants in
`0x3b0` before touching `0x1c4c`.  Reset the PInS-selected GPIO, select 1.62 or
2.70 Gbit/s and lane count in transmitter registers `0x5d/0x5e`, start through
`0x5f`, verify `0x76`, `0x8d`, and DPCD status, then enable video with
transmitter register `0x0a[5:4]=3`.  This sequence is validation-required on
each connector type because the transmitter part number is still unknown.

Use the DRM software cursor initially.  Static vendor code closes four cursor
banks at `0x18c0/0x1ac0/0x18e0/0x1ae0`, their address/config/control/position
registers, `+64` signed-edge positioning, and the intended 64x64 color and
monochrome upload sizes.  However, the purported linear 64x64x32 color layout
is hardware-disproved; only R/B swapping and acceptance of config/control are
known.  Hardware cursor support remains blocked on a vendor MMIO/VRAM capture
that resolves the fetch layout.  Keep hotspot subtraction and complete-offscreen
hiding in software.  See `parhelia-m9-cursor-gamma-re.md`.

M9 has a 256-entry primary scanout LUT for each controller route.  For control
bases `0x1800/0x1a00/0x1980/0x1b80`, the index and RGB ports are respectively
`C+0x38/+0x3c/+0x40/+0x44`.  Clear control bit 14, write index zero, then write
each `u16` channel masked with `0xffc0`; OR bit 31 into each blue-port write to
latch the RGB entry.  Expose a standard 256-entry per-CRTC DRM gamma LUT.  It
affects scanout only and never render BO contents.  Restore the software ramp
after VT/suspend transitions.  Depth-30 input indexing and dithering remain a
separate GigaColor validation gate.

## 4. GEM, placement, address classes, and tiled resources

### 4.1 BO classes

Use one GEM object type with immutable initial placement:

| placement | backing | initial uses |
| --- | --- | --- |
| visible VRAM | local range within BAR0-accessible window | scanout, linear render/depth, textures, vertices |
| local VRAM outside current CPU window | pinned local range | later, after window switching is validated |
| DMA32 coherent | `dma_alloc_coherent` | requester page, command pages, fence page, context image |
| shmem | GEM shmem | CPU staging only; never submitted directly at first |

Pin every referenced engine-visible BO until its fence retires.  Put access
fences on each BO's reservation object.  Do not migrate, evict, or move a BO
while an M9 address appears in an outstanding command list.  PRIME imports
must use a validated 32-bit DMA mapping or bounce into a supported placement.

M9 has address windows and tagged requester classes, but no recovered general
per-process GPU virtual-memory contract.  Do not expose a userspace GPU VA and
do not infer a page-table format.  Kernel relocation converts a GEM handle and
checked offset into the address form required by that packet field.

### 4.2 Three distinct allocation namespaces

Preserve these as independent enums:

1. vendor internal allocation mode `0..5`;
2. public Linux backing type (`IO`, `SYS`, `PCI_LOCKED`, `SCATTER`);
3. requester address class `0..3`, mapped to low tags `{2,3,0,3}`.

Mode 2 uses `SYS` when request bit 5 is clear and `PCI_LOCKED` when set.  Mode
1 may attach `SCATTER` as secondary backing.  Other modes select range
allocators whose public memory type depends on the range's provenance; do not
name them from the mode number.  The requester base/transition address carries
the class tag; the published live pointer at `0x504` does not.

The no-op requester test must record which open-driver placement produces each
working class before general submission is enabled.  This is a validation
gate because the static joins do not name every class as VRAM or system memory.

### 4.3 Resource descriptors and tiling

Linear resources are sufficient for first rendering.  The selector-2 cached
view has an address plus two descriptor words.  In the final captured hardware
packet, index `0x78` is the relocated address, `0x79` is descriptor word 0,
and `0x7a` is descriptor word 1.  Descriptor word 0 holds horizontal span in
format elements at bits 15:0, layout selector at 18:16, flags at 21 and 23,
and seven-bit format at 31:25.  Descriptor word 1 holds ordinary height at
15:0 and depth/layer extent at 27:16.  Earlier static notes call these cached
outputs `idx78/idx79`; those names are helper arguments and must not be read as
the final hardware indices.  Render and depth views use selectors 8 and 9.

Class-3 32-bit physical addressing is now closed statically.  Select the exact
formula from `0x0f04[1:0]` (`device_mode`), `0x0f04[20:19]`
(`channel_mode`), and local/aperture location.  The executable reference in
`tools/m9-blocked-layout-evidence.py` covers all four 32-bit variants.  A
coordinate-coded upload remains a validation test, not a discovery blocker.

All 35 linear/tiled/special calculator bodies and dispatch relocations are now
hash-pinned.  The 32 tiled routines across 1/8/16/32/64/128-bit, DXT1, and
DXT2--5 have 352 checked coordinate outputs from directly invoked leaf bodies;
the 128-bit mode-2 aperture route is an explicit unsupported zero stub.  All
14 non-32-bit aperture permutation families now have maintainable formulas
that match 104,144 native comparisons.  The exact 40-bit PTE decode is also
known, but a live root-plus-page dump must still join the PTE-shaped index-`78`
image to actual resource ownership and validate physical pages before these
layouts are exposed broadly.

## 5. Requester, command transport, and UAPI

### 5.1 Deferred auxiliary requester state

The live vendor GL path uses the separate `0x400/0x404` command interval and
leaves `0x500..0x524` dormant.  The first driver can defer this auxiliary
requester.  When a demonstrated feature needs it, allocate a dedicated 4 KiB
DMA32 object.  Its recovered first-use order is:

1. compute the tagged command base and write `0x500`;
2. write zero to `0x508`;
3. publish the raw producer byte address to `0x504`.

Requester addresses are byte addresses aligned to four bytes.  The software
publisher writes `0x504` whenever the producer advances.  A wrap appends
`{2, resource_gpu_base}`, resets the software producer to the backing CPU
base, republishes `0x504`, and quiesces.  An ordinary transition begins with
opcode 1 and carries the tagged old pointer plus raw new pointer.  The 16-byte
and 32-byte variants depend on bus-master mode.

Save all eight requester dwords at `0x500,504,508,50c,510,514,520,524`.
Restore them in this exact order:

1. `0x520,0x524` unconditionally;
2. `0x50c,0x514,0x510` when the saved address-like pair differs above its low
   nibble;
3. `0x500,0x508,0x504` last.

Software must initialize only `0x500/504/508`.  The other five words are
hardware-owned opaque state.  Quiescence requires `0x508 bit 0`, two identical
successive samples of both `0x500` and `0x50c`, and a final confirmation of
`0x508 bit 0`.  Set `0x508 bit 1` only in the proved park/save path.

**Auxiliary-path blocker:** force one requester wrap while tracing
`0x500..0x524`.  This is needed to validate that path's address class and save/
restore behavior, although software does not need semantic names for the five
opaque words.  It does not block primary rendering.  Do not invent producer/
consumer meanings for them.

### 5.2 Engine arm and submit

The closed one-time context writes include:

```text
400  tagged primary command start
109c 00080000
448  00000000
1068/106c  two writeback-fence addresses
10ec/1094/1098  optional context address/limit controls
44c/450/454  packed partition/subcount controls; exact fallback
             00280800/1f3f2707/00070703
330=1000, 338=40, 33c=80, 340=c0, 354=03020100
f0c initial 8000 plus device-property bits 9:6
```

Three stable read-only samples on the tested M9148 match this exact fallback.
Preserve warm values on other products until their property tuples are joined.

Program the address windows exactly as documented in `parhelia-m9-dma.md`,
including `0x360/364`, `0xf64/f68/f74/f78/f7c`, and
`0x1320..0x1334`.  Their naming remains provisional, but values and order are
closed.  Submit a finished command endpoint by writing its translated address
to `0x404`.  `0x404` is an endpoint doorbell, not a ring-size register.  Kick
progress with `0x314=1` when required.  `0x31c` states 5, 2, and 0 belong only
to the proved park/resume/clear lifecycle; state 4 stays disabled.

The modern target's warm, unbound state has all aperture, context, and surface
window registers cleared.  Minimal no-op fence success therefore cannot be
used as evidence that rendering is ready.  Before the first triangle, derive
the GPU-domain bases and inclusive last-page values from the pinned job
resources.  In particular, the captured M9148 VRAM GPU domain begins at
`0x10000000`; PCI BAR0 begins at `0xd0000000` and must not be substituted for
that GPU address.

Read-only observation of 32,782 live vendor GL ioctls closes the initial
address domain: `0x400` points into DMA32 System RAM and always carries low tag
2, while the exclusive `0x404` endpoint is untagged and dword aligned.  The
complete auxiliary descriptor stayed idle.  See
`parhelia-m9-requester-drm-contract.md`.

The latest working vendor capture closes the private `ExecuteDma` envelope:

```text
+0x00 u64 structure_size = 0x840
+0x0c u32 total_extent
+0x10 u32 range_count
+0x18 + 16*i: {u32 start, u32 end, u32 flag0, u32 flag1}
+0x818 u64 returned mmap cookie for the next command buffer
+0x820 u64 returned kernel address (diagnostic only; never reproduce in DRM)
+0x828 u32 returned type/status
+0x82c u32 returned mapping size, observed as 0x8000
```

Each range is the end-exclusive byte interval
`[command_mapping + 8 + start, command_mapping + 8 + end)`.  The mapping is
32 KiB in the captured stack.  Ordinary records are `u32 count` followed by
`count+1` raw dwords and may contain several indexed packets.  Draw records are
raw three-dword records outside that counted framing.  This vendor envelope is
capture documentation; the new DRM UAPI should not expose its kernel pointers
or copying behavior.

### 5.3 New DRM submit ABI

Use a canonical M9 submit rather than a compatibility ioctl.  A submission
contains:

```c
struct drm_parhelia_m9_submit {
        __u32 context_id;
        __u32 command_handle;
        __u64 command_offset;
        __u32 command_length;
        __u32 relocation_count;
        __u64 relocations_ptr;
        __u32 bo_count;
        __u32 flags;
        __u64 bos_ptr;
        __u32 out_syncobj;
        __u32 pad;
        __u64 out_point;
};
```

Each relocation names a command dword, GEM handle, target offset, relocation
kind, and read/write usage.  Relocation kinds must be semantic and finite:
requester address, color/depth/texture resource address, program address,
H07 start/end, H06 packed-16 start/end, fence writeback, query snapshot, and
the proved special address-domain records.  The kernel rejects raw address
fields and patches a copied, kernel-owned command image.

The validator must:

* parse the M9 four-index header, stop at index `0x45`, and require exact value
  counts;
* distinguish counted ordinary records from H03/H06/H07 raw draw triples;
* allow only documented indices and bit masks for the selected context mode;
* prohibit MMIO/requester/IRQ/display indices from user command packets;
* check every range, integer addition, shift, and exclusive end for overflow;
* verify resource alignment, format/layout compatibility, and BO bounds;
* attach all read/write BOs to the job until its output fence retires;
* append the kernel-owned completion record and padding itself.

Do not accept a vendor-built `0x840` descriptor image, user pointers, bus
addresses, or a user-selected `0x404` endpoint.

### 5.4 Packet and draw formats

An indexed header contains four one-byte indices, least-significant first,
followed by one value for each non-`0x45` index.  `0x45` terminates unused
header slots.  Indices map to the engine window as `0x1000 + 4*index`, but
this arithmetic does not confer permission to emit an otherwise unknown
index.

The three closed draw families are:

| family | header base | dword 1 | dword 2 |
| --- | ---: | --- | --- |
| H03 | `0x03000001` | numeric start | exclusive end |
| H07 | `0x07000001` | relocated 32-bit address start | `(start & ~3)+4*N` |
| H06 | `0x06000001` | relocated packed-16 start | `((start&~3)+4*ceil(N/2)) | ((N&1)*2)` |

OR launch flags `0x20000000` or `0x60000000` into the header only through the
proved draw-state selection.  Topology and base vertex are lowered before the
draw triple.  H03 numeric ranges occur in the captured gltri and glxgears
streams.  The clean gltri submission has an internal four-vertex range at
physical command offset `0x264` and the application triangle's `[0,3)` range
at `0x4b8`.  H06/H07 remain static-closed but hardware-validation items.

## 6. Fences, scheduler, IRQs, contexts, and recovery

### 6.1 Completion and scheduler

Fence completion is a GPU memory writeback, not an interrupt and not
`0x8f0` idle.  Seed the coherent fence word to a value that cannot be mistaken
for completion, increment targets by `0x10`, and emit:

```text
4545e3e6
00000003
target | 1
```

Configure its address through `0x1068/0x106c`.  Extend the 32-bit completed
tag into a 64-bit timeline using the previous low word and an epoch.  A poll or
worker signals the job's `dma_fence`; interrupts are used for vblank and known
services only.  While pending, `0x314=1` may prompt progress.  Every wait uses
an absolute deadline and checks device removal and recovery generation.

The live ExecuteDma footer writes index `0x1b` with the second fence address,
then `e6=3`, `e6=2`, `e3=target|1`, and finally clears the dependency state with
`43=target`.  Preserve this exact footer as a golden decoder fixture; only the
kernel may append it to a validated job.

Use one DRM scheduler entity per file/context and one hardware queue initially.
Context restore, rendering, fence emission, and retirement must remain in the
same total order.  Resource fences from prior writes become job dependencies;
render-target-to-texture dependencies must emit the maximum class-1 fence via
packet index `0x43` before sampling.

### 6.2 IRQ controller

At initialization, write zero to masks `0x8dc/0x8e8/0x8fc`, then acknowledge
stale status at `0x8d8/0x8e4/0x8f8`.  Normal shared-IRQ service reads active
status, acknowledges only enabled active bits, and returns `IRQ_NONE` when no
owned source fired.

The four closed vblank mappings in low bank `0x8e4/0x8e8` are:

| DRM CRTC source | hardware bit |
| ---: | ---: |
| 0 | `0x1` |
| 1 | `0x2` |
| 2 | `0x100` |
| 3 | `0x200` |

Only physical CRTCs 0 and 1 need exposure initially, but retain all four
counter sources internally.  High bank bits and low `0x4/0x8/0x10` remain
masked.  High `0x8` is a special Windows rectangle-transfer completion, not
generic render completion.  No source has a defensible 3D-fault meaning.

### 6.3 Context isolation

Create a complete software state shadow per DRM context.  The scheduler tracks
one current hardware context.  On A-to-B switch:

1. serialize A's pending state into the shared command stream;
2. install B as current;
3. emit B's restore and dirty state through the same stream;
4. attach the resulting fence to every referenced BO and restore slot.

The vendor full index template is exactly `0xf8` bytes.  Its restore allocator
uses `round_up((3*n+1)*0x200, 0x1000)` and 0x50-byte records at base `+0x700`
with 0x600-byte stride.  Ten fields are relocation/control placeholders.  The
fixed headers are documented in `parhelia-m9-dma.md`, but the ten roles are not
yet named.  Do not replay that template until every placeholder is typed and
relocated.  The initial implementation can instead serialize complete known
state at every switch; the template is a later restore optimization.

Do not recycle a context image or release a BO until the associated fence has
retired.  The vendor stack has passed a two-process direct-GLX test with
distinct linked programs, colors, alternating draws, and correct readback in
both clients.  Repeat that test with different render target, depth, viewport,
texture, and program state before enabling simultaneous clients in the new
driver.  This is now an implementation-validation gate rather than a missing
vendor-behavior capture.

### 6.4 Recovery

No saved Linux or Windows component exposes a global 3D reset.  The Windows
`ResetHw` callback resets display state only, the Linux disable callback is
empty, and the Linux ICD's named recovery entries assert.  Therefore the only
defensible initial recovery is:

1. stop admission and snapshot requester, fence, front-end, IRQ, and software
   job state;
2. attempt the bounded quiescence handshake;
3. if quiescent, perform one requester-local park/restore in the exact saved
   descriptor order, restore address windows, kick `0x314`, and require a new
   no-op fence;
4. error every old-generation fence;
5. if any step fails, keep rendering wedged while leaving scanout running.

This requester-local sequence applies only if the auxiliary `0x500` path was
active.  It must not be used to claim recovery from a primary `0x400/0x404`
render timeout: live GL runs while `0x508` already reports the auxiliary path
quiescent.  For a primary timeout, allow one bounded `0x314` progress kick and
then fail closed.

Never free memory reachable by a non-quiescent requester.  On terminal device
teardown, first stop host DMA by clearing PCI bus mastering, then quarantine
outstanding allocations.  A full engine reset and fault-source mapping remain
blockers for transparent hang recovery, but do not block a fail-closed driver.

## 7. Mesa Gallium architecture

Add M9 as a separate backend under the existing Gallium driver rather than
adding M9 conditionals to Parhelia state emission:

```text
parhelia_screen/context/resource       shared Gallium-facing objects
m9_winsys.c                            M9 UAPI, BOs, contexts, syncobjs
m9_batch.[ch]                          counted records, packets, relocations
m9_state.[ch]                          complete per-context state shadow
m9_draw.c                              topology, vertex/index state, H03/H06/H07
m9_surface.c                           RT/Z descriptors, viewport, clears
m9_texture.c                           sampler descriptors and mip layout
m9_format.c                            pipe-format to M9 format/view table
m9_nir.c                               NIR lowering and compiler key
m9_isa.[ch]                            native IR, packer, disassembler
m9_program.c                           upload/bind, constants, varying linkage
m9_query.c                             optional occlusion-query implementation
```

Keep command construction deterministic and independently decodable.  Every
state object owns an API-level value, a lowered M9 value, dirty bits, and the
BO references used by that value.  A flush snapshots the context state into a
batch; later API changes must not mutate an already queued batch.  This is
required for multiple GL clients and for draws queued before a state change.

Use the Gallium draw module for unsupported topology and CPU vertex processing
during bring-up.  Lower fixed-function OpenGL to NIR, then to the M9 backend;
do not port the Parhelia WARP or fragment combiner compiler.

Initial advertised limits must not exceed the vendor compiler's closed upper
bounds:

| capability | upper bound |
| --- | ---: |
| vertex attributes | 15 usable linked outputs/attributes; vendor GL advertises 16 inputs |
| vertex samplers | 4 |
| fragment samplers | 16 logical |
| combined samplers | 20 |
| simultaneous physical fragment state blocks | 4; logical samplers can be serialized |
| vertex uniform components | 512 |
| fragment uniform components | 512 |
| varying floats | 60, with 14-entry varying remap |
| draw buffers | 4 |
| native backing-bank boundary | 384 instructions / 96 32-byte lines |

Report the lower implemented limit whenever the logical-to-physical mapping is
not supported by the new compiler.  The vendor's five-dependent-sampler
capture proves correct lowering into four scratch render-target passes and one
final pass while time-slicing one full sampler slot and using `74..76` as
scratch route operands.  Do not expose all 16 logical fragment samplers
until equivalent dependency/intermediate management and N/N+1 boundaries are
implemented and tested.

The four-target value is likewise a compiler/hardware upper bound.  The first
driver must report one draw buffer and one color attachment.  It must reject
fragment outputs above zero and never emit `b2..b7` or `89[11:3]` until the
extra-target export and fence-retirement contract passes its bounded tests.

## 8. Draw frontend and fixed render state

### 8.1 Topology, vertices, and indices

The Direct3D topology map and primitive-count arithmetic in
`parhelia-m9-draw-frontend.md` are closed and should be translated to Gallium
primitive rules.  Unsupported line loop, quads, quad strip, polygon, adjacency,
restart, and provoking-vertex cases are lowered to supported lists/strips in
software.  Validate all resulting counts against the BO range before emission.

The vendor frontend has 16 stream descriptors and a separate index descriptor.
It folds `BaseVertexOffset` into each stream byte address before launch.  The
declaration parser supports up to 64 elements and the D3D9-style element
fields `{stream, offset, type, method, usage, usage_index}`.  Preserve these in
an M9 vertex-layout IR; do not submit the recovered two-dword frontend records
as hardware packets because their final hardware lowering is separate.

The controlled vertex corpus closes four canonical preprocessing layouts,
including float2/float3 position and packed ubyte4 color.  API offsets, strides,
and nonzero `first` disappear because the vendor CPU-repacks them before the
preprocessing pass.  Use the exact captured index-`10` span and variable
index-`23` records for those layouts.  Semantic naming of the four generated
raster streams and a server-side zero-copy VBO layout remain optional; neither
blocks the canonical CPU-repacked path.

The captured settled gltri submission contains two draw ranges: physical
command offsets `0x264` with `[0,4)` and `0x4b8` with `[0,3)`.  The latter
matches the three triangle vertices.  In the complete captured glxgears cycle,
active submissions contain respectively 10, 2, 10, 2, 10, and 2 draws;
the two final type-2 submissions contain no draws.  Use these as parser and
batching regression fixtures.

The two gltri draws are one producer/consumer pipeline, not two application
draws.  The preprocessing range writes resource addresses through
`b0=113c1000`, `b2=113d7800`, `b4=113ee000`, and `b8=11404800`; the raster
range consumes those same addresses as
`00=b0`, `01=b4`, `02=b8`, and `03=b2`.  Addresses are capture-specific and
must be relocations, while the permutation is an exact live join.  Do not
replay only the raster range.  The element formats, strides, and semantic roles
behind `00..03` remain part of the compact vertex-fetch blocker above.  A first
captured-layout triangle must reproduce both passes; a CPU-generated direct
raster input becomes valid only after the four consumer layouts are decoded.

The user raster draw's other golden state is `22=00006403`, color
`b0/b1=<11ee9000>/cc210140`, depth
`b8/b9=<11f39000>/fa210140`, auxiliary depth
`ba/bb=<11f89000>/f0010140`, depth/stencil
`a8/a9/aa=0038038a/08ffff00/00ffff00`, and viewport/clip
`f5=c4=0, f6=c5=00f00140`.  Bracketed addresses are capture-specific
relocations.  The later primitive-by-primitive capture closes the raster
selector in both `22` and `d7`: points use their special expansion, ordinary
selectors are lines `1`, line strip/loop `2`, triangles `3`, triangle strip
`4`, fan `5`, quads `6`, quad strip `7`, and polygon `d`.  H03 nonindexed,
H06 16-bit, H07 32-bit, and u8-to-u16 promotion are also captured.  Use
`parhelia-m9-vertex-topology-capture.md` as the executable mapping contract.

### 8.2 Render targets, depth, viewport, and scissor

The closed surface packet map is:

| resource | indices | relocation selector |
| --- | --- | ---: |
| color 0..3 | `b0/b1`, `b2/b3`, `b4/b5`, `b6/b7` | `2..5` |
| ordinary depth | `b8/b9` | 6 |
| hierarchical-Z metadata | `ba/bb` | 7 |

The address word preserves three high tag bits and uses 29 address bits.  The
descriptor contains row span, layout, flags, and format as specified in
section 4.3.  `ba/bb` is internal hierarchical-Z metadata rather than a
separate API stencil plane.  Windows `HZFlags` allocator `0x54be0` gates
16/32/64 stored-bit depth resources with bits 0/1/2 and creates format `0x0b`
at `ceil(width/8) x ceil(height/4)`; binder `0xaaa7d` installs its selector-12
view through the `ba/bb` shadow.  Initialize and conservatively invalidate the
complete plane with `0x007ff000`.  Complete depth clears seed the complete
plane through the format-aware clear path; partial clears invalidate the whole
plane.  Mark it dirty after uncoupled CPU/copy writes, retry the conservative
reset during resource transition, give the allocation to the creating depth
resource, make clones non-owning, and track parent plus auxiliary together.
Retain explicit null `ba/bb` as the allocation-failure/disabled fallback and
validate rejection correctness before enabling this optimization by default.

Final depth/stencil controls are `a8/a9/aa`.  Live gltri/glxgears comparison
closes `a8 bit 0` as depth-test enable and `bit 1` as depth-write enable, so
depth testing without update is the direct hardware state `bit0=1, bit1=0`.
The remaining `a8` fields contain the remapped depth comparison, stencil and
two-sided enable, and both faces' comparison and operations.  `a9/aa` contain
each face's reference/read mask/write mask.  The exact comparison map is
`{NEVER,LESS,EQUAL,LEQUAL,GREATER,
NOTEQUAL,GEQUAL,ALWAYS} -> {0,1,3,4,6,5,7,2}` and stencil operations map
`{KEEP,ZERO,REPLACE,INCRSAT,DECRSAT,INVERT,INCR,DECR} ->
{0,3,1,6,7,2,4,5}`.

Intersect scissor with viewport and every attachment bound, clamp negative
coordinates to zero, and emit exclusive endpoints.  Emit ordinary left/top
and right/bottom at `f5/f6`; emit `c4/c5` as duplicates for the zero-origin
path.  The nonzero-origin transform for `c4/c5` remains optional and must stay
disabled until captured.

### 8.3 Clear and copy

Implement rectangle-draw clear first: bind resource view 8 as color 0, install
the replicated clear value and exclusive rectangle, emit ordinary state, then
launch the draw.  Depth view 9 can be temporarily bound through the proven
special rectangle sequence.  CPU clear is the fallback for unsupported
formats/layouts.

The whole-allocation page-run clear is statically closed, including `e6/ac`,
`2a/2b/e6`, page masks, and cleanup, but it is validation-required and should
remain off until compared with a vendor result.  It accepts only page-aligned
whole resources and restricted element widths/clear patterns.

The M9 unified 2D raster path can implement blits and X acceleration through
the same validated queue.  Its destination/source, control, geometry, clip,
and line registers are recorded in `parhelia-m9-accel.md`.  Do not use the
old direct-MMIO prototype; execute the payload through requester DMA.

### 8.4 Blend, alpha, color mask, raster, and multisample

The API state inputs and compiler keys are closed:

* alpha test, blending, separate alpha, constant blend color, and format-
  dependent color masking lower into fragment native code;
* direct color masks use repeated nibble values R=`0x4444`, G=`0x2222`,
  B=`0x1111`, A=`0x8888` when the format permits;
* fill, shade, and cull use compiler key fields `[1:0]`, `[5:4]`, and `[7:6]`
  with D3D enum encodings point/wire/solid `1/2/3`, flat/Gouraud/Phong
  `0x10/0x20/0x30`, and none/CW/CCW `0x40/0x80/0xc0`;
* point fill has a gated `c1[11:0]=0xfff` effect;
* multisample enable compiles to `ad` replicated four times followed by `af`;
  a zero `MULTISAMPLEMASK` suppresses color output;
* reserved DP2 opcode 5 is the sole direct caller of the 1..31 auxiliary-state
  dispatcher.  Auxiliary state 25 stores a float at frontend `+0xf0`
  (validator `+0x170`); reset `1.0f` packs to `af[8:0]=0x100` through the
  recovered magic-bias conversion.  The public meaning of state 25 remains
  unknown and must not be invented.

The controlled fixed-state corpus now contains cull/front-face, polygon
line/point, flat shade, alpha, blend, color-mask, depth-write-off, stencil, and
sampler A/B cases together with their command and linked-state resources.
Decode those preserved differentials into golden vectors and validate them on
the new driver.  The nonzero partial sample-mask path is still unjoined;
disable partial masks until a real multisample surface can be captured.

## 9. Textures, formats, mipmaps, and coherency

### 9.1 Sampler state

Four live backend units emit selector `70+unit`.  Their packet groups are:

| state | indices |
| --- | --- |
| resource address/descriptors | address `78`, descriptor words `79,7a` |
| filter/wrap/LOD | `7c,7d`, reserved `87` |
| border color | `7e..81` |
| bump matrix | `82..85` |
| auxiliary resource | relocated `c0` |

Index `7c` is fully packed in `parhelia-m9-texture-state.md`: min/mag/mip
filter, anisotropy class, U/V/W wrap, comparison and operation/resource
controls.  Index `7d` contains signed LOD bias and two nine-bit bounds.  Keep
the orientation of those two bounds as a validation item and use symmetric
bounds until a differential proves min/max orientation.  Index `87` is
reserved because it is emitted but has no recovered producer.

The combined operation/resource code distinguishes 2D, cube, and volume, but
some compiler-token family names remain unknown.  Implement the ordinary 2D
family first, then enable cube and volume only after their captured descriptors
and coordinate behavior match.

### 9.2 Format table

Use the joined hardware format table in `parhelia-m9-texture-state.md` as the
single source for sampler, color, and depth view capability.  It covers common
RGB(A), 10:10:10:2, luminance, vector, 16/32-bit float, YUV, D16/D24/D32, and
DXT1-5 codes.  A format is advertised for a Gallium bind only when that exact
use has evidence: presence in the generic table alone does not prove that a
depth format can be sampled or that every texture format can render.

The live selector-2 texture descriptor has no separate channel-swizzle field.
Choose the correct hardware format code for each channel order and lower other
swizzles in the shader.  Reject the vendor-rejected formats listed in the
texture-state note.

### 9.3 Mipmap layout

Allocate ordinary levels consecutively using per-level aligned byte size.
Width, height, and depth halve to one.  A single root address and compact
descriptor bind the resource; there is no final packet array of per-level
addresses.  Index `7d` selects LOD range.

Implement the exact 32x16 packed tail atlas from
`parhelia-m9-texture-state.md`: trigger after the adjusted dimensions meet the
vendor limits, retain the minimum 32x16 allocation, and add the documented
per-level X/Y offsets during uploads.  Cover 2D, cube faces, volume slices,
DXT block dimensions, and one-pixel terminal levels in unit tests before
enabling mipmaps by default.

### 9.4 Write-to-sample visibility

Track the latest class-1 submission fence on render targets.  When a BO becomes
a sampled texture, emit the maximum dependency through index `0x43` even if
its descriptor is otherwise clean.  This static path is closed.

The live RGBA8 FBO chain proves that the class-1 dependency and ordinary state
order make a rendered target visible to the sampler without an extra cache
packet.  Implement that path directly.  Copy/upload destinations and
depth/stencil producer transitions are different producer families; serialize
those with a full fence and re-emit texture state until their own A/B captures
prove the narrower rule.

## 10. Native shaders and program state

### 10.1 Representation and packer

The native instruction representation is two little-endian dwords, eight
bytes.  The static packer has two five-bit operation selectors in `W0[4:0]`
and `W1[25:21]`, a format-dependent selector in `W1[18:14]`, five typed source/
resource descriptors, one result descriptor, predicate fields, result routing,
component-pair modes, and scattered component masks.  Implement this as typed
structures plus field packers; never expose a raw native byte stream to
untrusted applications.

The 0xc00-byte backing-bank boundary can hold 384 instruction slots, but the
recovered compiler stream explicitly refuses to append beyond 256 slots.
Enforce 256 generated slots until a different execution path proves a larger
compiler-visible limit; 384 is an allocation boundary, not an API promise.

The current executable lowerer corpus supplies exact generated instruction
sequences for MOV, ADD, SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN, MAX, EXP, LOG,
TEXLD with zero metadata, CMP, SETP, and FLOOR.  Use those sequences as golden
tests for the new encoder.  They prove only the documented canonical
descriptors; the compiler must still generate each source/result descriptor
from NIR rather than substituting literal captured words.

The live terminal slot is exactly:

```text
1d 7e fc ff 0f 3f 80 a2
```

It is sufficient to emit a vendor-identical final slot.  Its semantic name and
whether stop is an in-stream control or an implicit endpoint remain validation
questions rather than byte-emission blockers.

### 10.2 Program upload paths

Static recovery proves an inline upload path with selector index `0x68` and
data ports `0x60..0x67`.  One selector line carries 32 bytes/four native
instructions, with selector bit 31 enabled and byte/instruction offset in bits
13:5.  It also proves an external resource bind through relocated index `0x69`
and entry controls `0x6a/0x6d`.

The live 64-bit Linux glxgears capture proves another active upload path.  In
its sequence 18, an invariant 80-slot/0x280-byte native-looking program is
uploaded using selector index `0x8f` and paired data ports `0x90/0x91` through
`0x9e/0x9f`.  The
selector is `0x80000000 + block*0x40`; for an even data port the instruction
number is:

```text
(selector & 0x3fffffff) / 8 + (even_port - 0x90) / 2
```

Each even/odd port pair carries one two-dword instruction, eight instructions
per selector block.  The program begins with SHA-256 prefix `78434e` in the
captured fixture.  A separate relocated program resource is bound by indices
`0x69/0x6a/0x6b/0x6c`; consequently the invariant `0x8f` upload must not yet be
assumed to be the application vertex or fragment program.  This captured path
is runtime truth for the tested M9148 vendor configuration.  Keep the
`0x68/0x60..67` and `0x8f/0x90..9f` paths distinct until their selection and
roles are proved.

The saved-resource audit now pins the full hash as
`78434eea53365d762f7eda163d5981ab765d754d1bed9e5377ed231e30bc34fb`,
the exact selector/port formula, and byte equality across startup sequences 0,
8, 10, and 14.  A fresh diagnostic may use this exact 80-slot image, but it
must initially retain the complete sequence-0 setup envelope because the
smallest cold-safe surrounding transition subset has not been isolated.

The isolated GLSL A/B/A differential in
`parhelia-m9-glsl-bind-capture.md` proves that neither a vertex-only nor
fragment-only program switch uploads `90..9f`; both atomically select a linked
state record through relocated index `0x6c`.  The `0x8f` image is therefore
context/invariant state in this configuration.  Selection between the external
linked-record path and the separate `0x68/0x60..67` inline path remains open.

### 10.3 Vertex/fragment stage ownership and constants

The software stages are closed through the compiler boundary:

* serialized stage flag 1 is vertex and uses the typed vertex backend;
* flag 0 is fragment and uses the typed fragment backend;
* cross-stage record `0x67` is fragment-owned and carries the 14-entry varying
  remap plus high-water mark;
* vertex and fragment constants use separate persistent vec4 arrays;
* old-engine capacities are 256 vertex vec4s and 224 fragment vec4s, while the
  exposed compiler/API limit is 128 vec4s per stage;
* constant updates retain native first index and count, and vertex updates
  precede fragment updates.

The external program emitter tracks a relocated resource, then emits `0x69`,
`0x6a`, and `0x6d` in that order.  The inline fixed-chain builder records two
entry starts and can place both in `0x6a` or split the second to `0x6d`.

The controlled vertex/fragment A/B/A capture closes the final external-path
ownership edge: `69/6a/6b` remain invariant and `6c` selects a retained,
0x80-aligned linked-pair record in a captured 0x21000-byte system resource.
Changing either stage selects a new whole-pair record; restoring A restores its
exact previous `6c` value.  The pair is an opaque transport object: the
reachable public serializer exposes no independent native start/count,
per-stage entry, or end-PC controls.  Its private whole-pipeline allocation,
key lookup, copy-on-write validation, dependency promotion and publication are
exact; new arbitrary shaders still require the internal pair byte serializer
and semantic linker rather than a guessed resource layout.

The exact host-image audit additionally proves that this serializer is not a
literal copy. Linux and Windows construct the same coherent 0x100-byte host
image, but none of its eight invariant paired dwords occurs even once in any
of the 17 complete captured system allocations. The compiled-pair promoter
also targets the separate top-state `+0xd8` resource/link object, while
`m9x2427` consumes the independently owned top-state `+0xc8` program object.
Use `tools/m9x-linked-record-final-join-gdb.py` to stop at the first nonzero
write to a fresh selected record and recover the remaining semantic
constructor; do not seed an open driver with the host image or a captured
record template.

The live uniform A/B/A capture identifies vertex and fragment constant ranges,
live-component packing, and the advancing record offsets in the external
0x21000-byte resource.  Use that external range path first.  The conditional
mode-2 inline constant bank has no observed public GL trigger and remains an
optional optimization rather than a first-render blocker.

### 10.4 NIR compiler scope

The first NIR backend should support only the exact typed families required by
a fixed-function OpenGL 1.x path: MOV, ADD/SUB, MUL/MAD, dot products, min/max,
reciprocal/rsqrt, basic exp/log, one ordinary 2D TEXLD, comparison/kill for
alpha test, constants, attributes, varyings, and color/depth outputs.  Lower
matrix, lighting, fog, and fixed-function texture combine into those forms.

The executable lowerer matrix closes destination masks, source swizzles and
modifiers, destination classes, temp/varying/constant sources, predicate
polarity, and canonical MOV-through-FLOOR/CMP/SETP forms.  Matrix operations
lower exactly to DP3/DP4 rows.  The remaining arbitrary-GLSL blockers are:

* linked-resource controls distinguishing TEXLDB/TEXLDP/TEXLDL and vertex
  texture fetch.  TEXLDD has an exact nine-slot expansion, ten metadata flag
  expansions are recorded without invented semantic names, and shadow compare
  enable plus all eight functions are exact;
* the nonzero comparison controls needed by SLT/SGE;
* predicate composition around KIL and texture forms outside the recorded
  basis.  Canonical KIL has an exact seven-mask packed basis, while DDX/DDY
  selectors and 104 mask/swizzle/modifier/predicate/control variants are exact;
* graph construction and scheduling for arbitrary branch targets, loop
  backedges, reconvergence and BREAK/CALL, plus the admitted terminal choice.
  The ELSE/BREAK/BREAKP slot templates, loop/break builder joins,
  CALL/LABEL/RET graph edges, CALLNZ macro expansion, paired physical-selector
  allocation for `94/b4/c4`, ordered substitution and final W0/W1 selector
  placement are already exact;
* serializing the completed native images and linked resources into the opaque
  internal bytes of a new immutable `6c` pair.  Private allocation, keying,
  cache reclamation, copy-on-write publication and fence retention are exact.

Expose only caps whose required lowering fits this verified subset.  Use
Gallium's draw module for supported software lowering and return a compile
failure when an application shader still exceeds it.  Never guess an opcode in
a submitted program.

Physical varying selectors themselves are closed: after matching the explicit
link record, fragment input `v[p]` uses `2*p`, ordinary vertex output `o[p]`
uses `(0x58 + 2*p) & 0x7f`, and the special output uses `0x78`.  Preserve the
register-file class prefix until the final packer step; only that step consumes
the seven-bit selector.

## 11. Queries and optional engine features

Occlusion query has a complete static packet contract.  Begin and end each
write two dwords through a relocated address.  The indexed body is:

```text
e6e6e61e
<relocated snapshot address, selector 0x3d>
64000004
64000101
40008000
```

The OpenGL result is unsigned 32-bit `end.word0 - begin.word0`; word 1 is
ignored for samples-passed.  Availability follows completion of both snapshot
writes.  Multisample results are scaled in the vendor GL layer.

Every OpenGL begin/end snapshot uses selector pair 0/1.  These fields are
source selectors, not a per-query slot, and the vendor path has no query-slot
allocator or wrap rule.  Query objects instead own distinct relocated begin
and end snapshot resources.

Live queries now return exactly 16384 for a full 128x128 quad, 8192 for half
coverage, zero when depth-hidden, and 16384 after object reuse; pre-finish
availability is zero.  Keep `PIPE_QUERY_OCCLUSION_COUNTER` experimental until
physical counter overflow, snapshot word 1, ordering stress, and raw versus
ICD-scaled multisample behavior are verified.  This does not block the initial
driver.

The following also remain optional: hardware cursor, partial sample masks,
the live index-`78` table-root/physical-page ownership join, the fast
whole-page clear, uncommon interrupt services, more than four live fragment
texture blocks, and bus-master shader/constant optimizations.  All fourteen
non-32-bit class-3 aperture permutation families already have native-checked
maintainable formulas.  Nonzero-origin `c4/c5` viewport/scissor bounds are
already captured.

## 12. Bring-up and regression sequence

Every stage must run without installing an M9 module into the target's normal
`/lib/modules` tree.  Build the module locally, copy it to a temporary target
directory, and load it explicitly so a bad probe cannot make the test host
unbootable.

### 12.1 Kernel and KMS

1. Probe with rendering disabled.  Verify PCI resource selection, PInS parse,
   installed/visible VRAM separation, and disconnected-MMIO handling.
2. Start atomic KMS at 1920x1080@60 where EDID supports it.  Verify both DVI
   connectors independently, scanout pitch/start, DP transmitter operation,
   and vblank counters.
3. Run dumb-buffer mmap, page flip, VT switch, suspend-like disable/enable, and
   software cursor tests without touching render MMIO.
4. Enable the requester diagnostic.  Submit only the three-dword writeback
   fence, validate address class, then force one clean wrap and save/restore.
5. Exercise a bounded timeout with no invalid 3D command first.  Confirm the
   driver fails closed and scanout survives.

### 12.2 Command and rendering fixtures

1. Decode every checked-in vendor fixture with `m9-command-decode.py
   --execute`; require exact range boundaries and no unknown framing.
2. Replay only a whitelisted no-op/fence, then a linear clear, then the captured
   fixed-layout triangle.  Compare the render BO rather than the scanout LUT.
   The decoder fixture must preserve the relocatable gltri program-bind tuple
   `69=<external-fixed-fragment-db>, 6a=25010000, 68=00000000, 6b=00001030,
   6c=40c3a301` before both sequence-12 draws.  It must also preserve the
   sequence-14 texture tuple `78=<resource>, 79=f0210140, 7a=000100f0`.
3. Run repeated clear/triangle batches through requester wrap and 32-bit fence
   wrap simulation.
4. Run A/B/A contexts with different targets and state, then two processes in
   parallel.  Verify no state, BO reference, or completion crosses contexts.
5. Add vertex/index bounds, malformed headers, missing terminators, bad
   relocation kinds, arithmetic overflow, and forbidden-index negative tests.

### 12.3 Gallium correctness and performance

Bring up in this order: untextured triangle, depth test with depth writes on,
depth test with writes off, stencil, scissor, texture upload/sample, mipmaps,
alpha test, blending, multiple textures, FBO render-to-texture, and optional
queries.  For every state test, alternate it with a different context to catch
leaked hardware state.

Always include unthrottled `glxgears` as the common correctness/performance
regression:

```sh
vblank_mode=0 glxgears
```

The repaired vendor baseline on the M9148 is approximately 2939 FPS.  Treat
that as a workload reference, not a correctness oracle.  Also run a normal
vblank-synchronized case at the display's 60 Hz mode and inspect the rendered
window for tiling, stale-buffer, gamma, and present errors.

The OpenGL 1.x application gate is ioquake3 in a window and fullscreen with
textures, mipmaps, depth writes on/off, alpha-tested UI, blending, gamma, and
multiple simultaneous GL clients.  `glReadPixels` performance is secondary,
but its format, pack alignment, and rendered extent must remain correct.

## 13. Exact remaining-blocker ledger

| gap | consequence | smallest closing experiment |
| --- | --- | --- |
| cold fixed-triangle resource initialization | the command topology and relocation roles are exact; the bounded flat-green source is an exact 80-byte `{x,y,r,g,b}` float image; C2 has an exact 0x1080-byte allocation-base binding and 0x2000-byte reservation, while no payload image or local-aperture store trace was saved, making zeroed job-local storage the guarded probe policy rather than vendor-identical initialization evidence; Windows gives the exact AB/AC table formula and proves inactive slots are stack residue, making `AB=AC=ffffffff` deterministic for one RT; BE belongs to bound-surface usage 8 and a cross-family trace proves a zeroed 0x3f00-byte lower bound, but exact M9148 extent remains absent; the captured fixed linked slot embeds stale host VAs and is forbidden for cold replay | recover M9148 BE extent/initialization, then serialize a current-context linked record or validate the inline fixed-chain selector relation; recover C2 bytes only if the zeroed guarded probe fails |
| direct SPI/PInS fallback when PCI ROM mapping is absent or invalid | some cold boards may lack authoritative output data | reduce the statically joined `m9x1755` serial-flash transport to its GPIO/MMIO transaction sequence and validate it |
| auxiliary requester address classes and hardware-owned rollover fields | optional auxiliary streaming cannot be enabled safely | force one auxiliary wrap with all eight descriptor words and allocation metadata |
| ten relocation/control roles in the vendor full-context template | the template cannot be replayed safely; context switches must re-emit complete known state | vary one bound resource/control at a time and diff the 0xf8-byte template plus restore records |
| programmable linked-pair byte serialization | `6c` atomically selects an opaque immutable VS/FS pair; private 0x3158-byte allocation, keyed reuse, >1023-entry reclamation, COW validation/publication and fence lifetime are exact, with no universal public start/count schema | recover the internal bytes that join completed native images and resources into one pair, then validate reset behavior |
| special shader forms and physical control-flow fixups | KIL seven-mask, DDX/DDY 104-variant and TEXLDD nine-slot forms are exact; ten texture metadata expansions, eight shadow compares, ELSE/BREAK/BREAKP, graph-builder joins and pre-pack substitution are recorded; B/P/L linked-resource controls, unrecorded predicates and semantic branch placement remain | resolve graph targets/backedges/calls/reconvergence and the remaining linked-resource controls; reject combinations outside the recorded basis |
| generated-stream semantic names and server-side zero-copy vertex layouts | the initial driver must CPU-repack to the four captured canonical layouts | one-output-at-a-time shader/content snapshots, then stride/offset differentials for an optional direct VBO path |
| MRT export and retirement | four descriptor/relocation targets and common TaskMgr fences are exact, but native export and job-BO membership are unjoined; Linux close has an unbounded fence poll | prove one decoded RT1 export/write-list/common-writeback job, then bounded readback/unbind/delete/context/fd close; expose RT0 only |
| multisample/FAA surface, pattern, mask and resolve | current GLX visuals expose no sample buffers; auxiliary state 25 is the exact float producer for `af[8:0]` and resets `1.0f -> 0x100`, but its public meaning is unknown | identify state 25 through a bounded differential, acquire a vendor multisample surface, then capture edge, partial-mask, resolve and query behavior |
| 10-bit and sRGB round trips | GigaColor remains unexposed; RT0 sRGB write has an exact 25-slot suffix and clear curve; live D3D9 sampler state 29 stores `+0x46` bit 5 in both sampler paths, but the pinned final native lowerers do not consume it | sentinel render/blend/readback for both 10-bit orders and the sRGB RT suffix; join a later derived-record consumer or use explicit shader lowering for texture sRGB |
| pixel-store, overlapping-copy and resource-lifetime edge cases | ordinary subimage and framebuffer-copy producer/consumer ordering is captured | alignment/row-length/skip/type matrix, overlap, mip-boundary updates, and deletion while referenced |
| class-3 physical backing ownership | all 35 calculator routes, 352 golden vectors, all 14 non-32-bit aperture formulas, and cube-face/volume-Z base math are exact; index `78` ownership is not | capture `request+0x90`, its table root, decoded PTEs and coordinate-coded physical pages together |
| query publication and multisample scaling | fixed selectors 0/1, modulo-32 arithmetic, ordinary full/half/hidden counts, availability, and reuse are captured; there is no slot allocator | cross physical `0xffffffff`, identify snapshot word 1, stress ordering, and compare raw versus ICD-scaled sampled-surface results |
| 3D/global reset and fault IRQ | no transparent recovery from a real engine hang | controlled address fault and bad-opcode tests, one source at a time, with complete pre-ack snapshots |
| hardware cursor fetch layout | cursor remains software backed | vendor MMIO/VRAM capture plus coordinate-coded image |
| depth-30 LUT indexing/dithering | do not infer GigaColor from the RGB10 LUT ports | connector capture or DAC measurement of a 10-bit gradient |

The primary command path, two-pass canonical vertex layouts, topology,
program-pair ownership, ordinary sampler state, mipmaps, fixed-state corpus,
FBO chain, and two-client behavior are captured vendor facts.  Replaying those
facts is new-driver validation, not a remaining reverse-engineering blocker.
Linked-pair byte serialization, semantic control-flow placement, and the
remaining linked-resource shader rows are the immediate gates for general
OpenGL 2.0.  The
context template, auxiliary requester, index-`78` physical backing join,
queries, reset, cursor, and gamma can use the conservative paths described
above while their rows stay open.

## 14. Evidence and clean-room rules

The primary evidence is recorded in:

* the saved Linux wrapper `m9xdriver-*/kernel/src/m9x_base.c` for PCI BAR
  selection and the 128 KiB option-ROM handoff;
* `parhelia-m9-linux-vendor-capture.md` and the checked-in gltri/glxgears
  fixtures for live command framing and program uploads;
* `parhelia-m9-dma.md`, `parhelia-m9-requester-rollover-boundary.md`, and
  `parhelia-m9-context-lifecycle-re.md` for kernel transport and lifetime;
* `parhelia-m9-render-surfaces-clear.md`, `parhelia-m9-draw-frontend.md`, and
  `parhelia-m9-texture-state.md` for Gallium state;
* `parhelia-m9-native-shader-fragments.md`,
  `parhelia-m9-native-lower-runtime.md`,
  `parhelia-m9-program-stage-entry-re.md`, and
  `parhelia-m9-program-constants-re.md` for shader compilation;
* `parhelia-m9-blocked-layout-boundary.md`,
  `parhelia-m9-nonvblank-irq-boundary.md`, and
  `parhelia-m9-recovery-surface-audit.md` for memory layout, IRQ, and recovery.

Implementation source should cite the corresponding documented field/formula
and use independently named structures.  Do not copy vendor object code,
private ABI layouts that contain pointers, or opaque firmware fragments into
the open driver.  Preserve captured binaries as test input and decoded facts as
golden expectations.  Where static and hardware evidence disagree, the
captured hardware behavior controls and the disagreement stays documented.
