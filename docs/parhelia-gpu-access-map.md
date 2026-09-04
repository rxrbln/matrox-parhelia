# Parhelia GPU-access map for the open low-level driver

Goal: reverse and capture ALL GPU access a from-scratch open driver needs —
kernel device init, memory management, modeset, 2D, 3D/GL, and command
construction. This is the index; details live in the per-area docs and captures/.

Two capture instruments established (both on a working 0x0528 box, vendor 2009
mtx stack, kernel 2.6.30):
1. **mmiotrace** — logs every kernel MMIO read/write (docs/parhelia-mmio-capture.md
   for the exact procedure + gotchas). Captures the register-level access.
2. **gdb on the vendor libGL** — captures userspace state (vertex buffers,
   command construction) that never hits MMIO (see the vertex-layout capture in
   docs/parhelia-vertex-isa.md).
3. **static RE** of mtx_drv.so / libGL.so.1.3.0 / mtx.ko / VBIOS — for logic.

## 1. Kernel device init + memory management
- `modprobe mtx` does NO MMIO — init is DEFERRED to device open (X start). mtx's
  ioremaps (mmiotrace MAP entries) appear when a client first uses the device.
- MMIO regions (from mmiotrace MAP): BAR1 registers at 0xfebfe000 size 0x2000
  (8KB); BAR0 fb; plus large DMA/aperture regions (0xdfe00000, 0xd7fc0000,
  0xd0040000).
- **DMA descriptor / pull-aperture setup** (captured in the draw trace, regs
  0x0000-0x0130): a bank of 16-byte descriptors — e.g. 0x00a0=0x17fc0000/
  0x00a4=0x8200000 and 0x0130=0x1fe00000/0x0134=0x8000000 (base/limit pairs that
  map the command/data apertures into GPU address space). 0x920=1 arms it.
- **BUS-TYPE CAVEAT — the capture board is PCI, not AGP.** `lspci -vv` on the
  capture box reports `Subsystem: Matrox Graphics, Inc. Parhelia PCI 256MB`,
  BAR0 256MB prefetchable @0xd0000000, BAR1 8KB @0xfebfe000, and **only a Power
  Management capability — no AGP, no PCIe capability**. So every address in
  these captures is a **plain PCI bus address**; there is no GART translation,
  and the 0x0000-0x0130 bank is an address-window/limit setup rather than GART
  programming. Parhelia also ships as **AGP-8x** (0x0527 / AGP 0x0528) and as
  **PCIe** (LX/P650/P690 0x2538/0x2539, Phoenix 0x0529); on AGP the same
  registers are expected to carry agpgart-translated addresses. The register
  protocol (0x404 start / 0x408 = end|3) should be bus-agnostic, but the address
  *production* is not — see docs/parhelia-3d-driving.md "Address-space caveat".
  **AGP is uncaptured**; re-capture on an AGP board before claiming AGP support.
- Kernel ioctl ABI (RE'd, kernel/src/mtx_ioctl.h): GET_MEMORY_INFO(0x00),
  GET_BUFFER(0x01)=alloc a DMA buffer, RELEASE_BUFFER(0x02),
  DRIVER_VALUE_REQUEST(0x03), ADD_USER(0x04), HARD_RESET(0x07). No SUBMIT ioctl -
  submission is the doorbell (below).
- 3D module params (memory type for uCode, VE cache, etc.): MtxParhlParms.c
  (see docs/parhelia-vertex-isa.md).
- TODO capture: record X-start with mmiotrace for the full modeset+init MMIO (the
  quick capture showed X reuses the boot mode = light modeset; force a mode change
  to capture it).

## 2. Modeset / display — RE'd + working
docs/parhelia-registers.md, parhelia-dualhead-complete.md, parhelia-ddc-complete.md,
and the memory notes (native modeset works on 0528/P690 up to 1920x1200). PLL,
CRTC/CRTCEXT, DC blocks (head0 0x1800/0x1900, head1 +0x200), DVO/DAC, crossbar.

## 3. 2D acceleration — RE'd
docs/parhelia-2d-accel-complete.md: DPS engine, DWGCTL opcodes, ROP, planemask,
pattern, transparency, scaled blit, clip, sync. Composite = software (no HW
blend in 2D path).

## 4. 3D / WARP — submission CAPTURED, command content = next
GROUND-TRUTH draw MMIO (captures/mmio-draw-0528*.txt): per draw =
- **arm**: 0x12b8=0x70601, 0x10b0=0x10, 0x300=0x1f0b0140, 0x384=0 (DMAPAD, NOT a
  soft-trap - see docs/parhelia-kernel-abi.md),
  0x308=0x83 (done twice).
- **submit via DMA RING**: 0x404=cmdbuf-bus-addr, then 0x408=end|3 written
  repeatedly (end advances 0x20/packet). This is the RE'd ring
  ([[parhelia-dma-ring-working]]). 0x314=1 kicks; poll 0x8f0/0x450.
- microcode ROM (mtx2785, 1203 dw, family-shared, tools/warp_ucode.h) streams to
  the 0x91c FIFO ONCE at init; **0x91c is NOT the per-draw geometry path**.
- WARP is armed (0x924 low12 0x000->0x100) by a GL context doing 3D — a from-
  scratch driver must reproduce that engine-arm.
- vertex attribute layout = [x, y, r, g, b] (captured live, docs/parhelia-vertex-isa.md).

## 5. Command construction — FULLY CAPTURED
**docs/parhelia-command-construction.md** is now the authoritative account: the
seven `/dev/matrox/mtx0` mappings and their roles, the per-draw 7-dword
descriptor (header 0x8f000001, vertex count, sequence, vertex-buffer bus
address), the head/tail publish at region-2 0x18/0x20/0x28, the confirmed
`[x,y,r,g,b]` 20-byte vertex, the (index,value) state-pair stream, and the
locked-page buffer allocation. Instrument: tools/parhelia-glsweep.c, raw capture
captures/glsweep-0528-raw.txt. Per-GL-feature register deltas:
docs/parhelia-gl-feature-map.md.

### 5a. First capture (superseded in detail, kept for provenance)
All draws use the SAME ring-submit MMIO; the FEATURE-specific state lives in the
COMMAND BUFFER the GPU DMA-reads — invisible to mmiotrace, but capturable.

**Capture method (works — the 3rd instrument):** gdb the vendor GL client, dump
the `/dev/matrox/mtx0` mappings at `glBegin` (PRE) and again at `glFlush` (POST),
then **diff**. The changed bytes ARE the packets the ICD constructed. Get the
mapping list with `info proc mappings` at a GL entry point (addresses are stable
across runs). For a single triangle: the **136KB mapping** (the command ring) had
**1539 bytes changed**, span 0x0000-0x1150; the 672KB and 3MB regions were
untouched (they are data/vertex heaps).

**Format:** the buffer decodes as a stream of **(register-index, value) dword
pairs**, index = (reg-0x1000)/4 — i.e. the same indexed register file the DMA ring
consumes. (Some runs of pairs are preceded by count/opcode words — the
[method][count][payload] record framing from the ICD RE; a strict pair-decode
mis-aligns in places, so treat the raw dump as authoritative.)

**State observed for one Gouraud triangle** (decoded, first-seen order):
0x1034=1, 0x1020, 0x10d4=1, 0x1030, 0x105c=2, 0x1098, 0x126c=0,
**0x1298=0x45000000 (2048.0f — viewport/scale)**, **0x1100-0x117c = 0xffffffff**
(the whole vertex/attribute register file initialised, 0x1118=1),
0x1048=0x3f800000 (1.0f), 0x1200=0x80, 0x1004=0x3f800000, 0x1014=2, 0x104c=1,
0x10d8=1, 0x1230=1, 0x1090=1, 0x108c=1, 0x1084=0x18, 0x1018=2, 0x101c=1,
0x1024/0x1028/0x102c=2, 0x1088=2, 0x109c=1, 0x1094=6, 0x1080=6, 0x1058=1.
Raw: captures/cmdbuf-draw-0528-decoded.txt (1109 dwords) + the pre/post .bin.

This is the missing raster state our from-scratch draw never programmed — the
0x1100-0x117c attribute-file init and the 0x1298 scale in particular.

**To map each GL feature:** run the same PRE/POST diff around each op in
gltest.c (flat/gouraud/textured/blended/depth/lit, quads/lines/points) and diff
the resulting command buffers against each other — the delta is that feature's
state.

## 6. GL feature register/encoding map — RE'd
docs/parhelia-3d-state-complete.md (texture, combiner opcodes, blend/depth/
stencil/alpha, viewport/scissor 0x1280/0x1284, constants), parhelia-fragment-isa.md,
parhelia-vertex-isa.md, parhelia-gl-driver.md.

## Systematic next steps (extend the map with the two instruments)
- DMA-buffer dump per GL feature (flat/gouraud/textured/blended/depth/lit tri,
  quads/lines/points) = the command packets for each. (gltest.c sweeps them;
  capture the command buffer, not just MMIO.)
- mmiotrace a forced modeset, a 2D accel op, a DDC read.
- Each capture -> a captures/*.txt + a decoded note here.
