# P690 arbitrary native SURF-base closeout

Copyright 2026 René Rebe <rene@exactco.de>
SPDX-License-Identifier: GPL-2.0

## Result

The P690 accepts ordinary 8-KiB-aligned native render surfaces at non-zero
offsets inside both upper 32-MiB allocation regions.  In particular, native
256x256 32-bpp surfaces at physical BAR placements `0x04002000` and
`0x06002000` work when bound to the graphics engines as `0x08002000` and
`0x0a002000` respectively.

There is no missing low-address fold, separate surface-origin register,
32-MiB base-alignment requirement, or extra per-region bit in the surface
control word.  The earlier all-pixel failures came from observing those upper
VRAM objects through a raw BAR mmap.  An engine copy from each tested surface
to the already validated anchored surface returned every pixel correctly.

The resulting implementation rule is:

```c
allocation_alignment = 0x2000;
assert(allocation does not cross a 0x02000000 region boundary);
surf_base = physical < vram_size / 2
          ? physical
          : 0x08000000 + physical - vram_size / 2;
surf_ctrl = format << 24 | layout << 16 | pitch_pixels;
```

Raw BAR mmap is not a valid correctness oracle for an arbitrary native BO on
this device.  CPU access must use a host-aperture surface descriptor, or data
must be transferred through an engine path such as local-to-local BITBLT or
local-to-class-3 readback.

## Sources

The static audit used the saved vendor binaries:

```
52b068f92031331e98e5bec2481683a4b3d881441120834fb5fad58f112f020a  xddm32_210_00_105_se_u_nowhql/MtxPser/mtxpserd.dll
5f4788291592619175c407bc070628c6e2c235c7a6bfde890b968bd413455c47  matroxdriver-x86_64-cvs-20090511/kernel/mtx_parhelia.o
```

`MtxPser` binds PCI device `102b:2539`, so its layout and allocation findings
are direct P690 evidence.  The live test used that PCI device with 128 MiB of
local memory.  Static facts below are marked **[BIN]** and live results
**[HW]**.

## The vendor allocation contract

**[BIN]** P690 Windows function `mtxpserd.dll:0xa5330` extracts layout from
surface control bits `[18:16]`.  For layout 2 or 3 at 32 bpp, instructions
`0xa5376..0xa5393` return:

```c
alignment = 0x1000 + (whole_allocation ? 0x1000 : 0);
```

An ordinary whole native allocation therefore requires `0x2000`, not
`0x02000000`, alignment.  The mip/surface constructor calls this helper via
`0xa5660` and records its result at surface offset `+0x24` at
`0xc688d..0xc6895`.

**[BIN]** The apparently conflicting 32-MiB constant belongs to region
construction.  P690 Windows function `0x3b0e0` selects region quantum
`Q=0x02000000` for capability mask `0x00070000`, divides each input memory
range at those boundaries, and alternates region categories.  It does not
round individual surface starts to `Q`.

The Linux HAL independently preserves this distinction.  `mtx2744` at
`0x5f2e0` builds the same bounded intervals.  Generic allocator `mtx2194` at
`0x58e30` validates that request `+0x04` is a power of two and passes the
requested size/alignment unchanged to `mtx608`/`mtx2349`.  Request `+0x08`
bits `0x08` and `0x10` choose allocation direction/best candidate; they do not
encode a surface-origin register.  The selected allocation remains inside
the interval that the splitter already bounded.

Consequently the vendor rule is 8-KiB allocation alignment plus 32-MiB region
containment.  It is not one allocation per 32-MiB anchor.

## The vendor bind contract

**[BIN]** Linux HAL address resolver `mtx1132` at `0x4c730` computes:

```c
address = allocation_base + byte_offset;
if (memory_type == 1)
        address |= 2;
else if (memory_type == 2)
        address |= 3;
```

It applies no address mask, shift, region-index field, or tile-origin
adjustment to local-memory addresses.  Bind paths `mtx1450` and `mtx631` use
this result directly.

Colour/depth path `mtx2372` copies the resulting bases and controls into
separate context fields.  `mtx2954:0xf96c..0xf9a9` emits the colour pair to
`0x1220/0x1224` and the depth pair to `0x1230/0x1234`.  Thus the complete
hardware destination description is the independently emitted base and
control.  No third register receives an allocation-region or origin value.

The P690 Windows copy path corroborates that contract.  At
`mtxpserd.dll:0x155249..0x15527d` it loads source and destination base/control
pairs directly from surface objects.  At `0x1552b9..0x1552e6` it emits those
pairs in a `0x8d8cc9c0` packet with ordinary `DWGCTL=0x2c2`.  It does not mask
the base or consult the allocation-region records between object load and
packet construction.

Host-aperture descriptors at MMIO `0x0000..0x013f` are a distinct CPU-view
mechanism.  Their dword 1 contains this same physical engine base; dword 0 is
an independently allocated BAR coordinate.  Render, depth, texture and 2-D
surface packets consume the physical engine base rather than descriptor
dword 0.  See `parhelia-vendor-submit-surface-addressing-re.md` for the full
object-lifetime proof.

## Why the first live result was misleading

The diagnostic allocated one 8-KiB native padding BO at a 32-MiB boundary and
then a 256-KiB target immediately after it.  Relocation telemetry proved the
intended placements and operands:

```
BAND2: node/bar=0x04002000, SURF GPU base=0x08002000, size=0x40000
BAND3: node/bar=0x06002000, SURF GPU base=0x0a002000, size=0x40000
```

The clear fences retired, but direct reads through each target's raw TTM/BAR
mmap reported `65536/65536` mismatches.  The probe exited on that mismatch
before its padding report.

That mmap does not have the vendor native-surface CPU contract.  P690 BAR0 is
a programmable host aperture.  A native surface normally receives a
descriptor with an independent low BAR coordinate, physical engine target,
layout/control, and dimensions; userspace maps the descriptor coordinate.
The diagnostic instead mapped the BO's physical TTM offset.  Its poison write
and direct read therefore could not establish the contents of the surface
named by the graphics-engine operand.  Inspecting the padding through its own
raw mmap would have had the same observer defect.

The ordinary anchored test did not generalize this raw mapping.  Its only
CPU-validated target was physical/GPU `0x02000000`.  Existing visually correct
work at physical `0x04000000` and `0x06000000` used engine addresses
`0x08000000` and `0x0a000000`, but had no direct raw-CPU verification.

## Decisive hardware result

**[HW]** The corrected probe retained the same allocations, relocation bases,
surface controls, and clear packet.  It then used an anchored GPU BITBLT to
copy the tested surface into the independently validated anchored target
before checking pixels.

```
source physical 0x04002000 / GPU 0x08002000: PASS, 0/65536 mismatches
source physical 0x06002000 / GPU 0x0a002000: PASS, 0/65536 mismatches
```

This one test proves both directions needed for the conclusion: the clear
writes every native-layout pixel through the arbitrary destination base, and
the BITBLT reads every pixel through the same arbitrary surface as a source.
The copy destination and its CPU observation were already validated, so the
result does not depend on raw access to either upper-band object.

It rejects the live alternatives:

* low base bits are not masked or folded to the region start;
* physical `+0x2000` is not encoded in another origin register;
* native surfaces do not have to begin at a 32-MiB boundary;
* BAND2 and BAND3 do not require different control-word class bits; and
* the vendor `0x2000` whole-allocation alignment was not misinterpreted.

## Minimal production deltas and validation order

1. Keep the current P690 physical-to-engine bank translation.  For 128 MiB,
   physical bank A is direct and physical bank B is encoded from
   `0x08000000` while preserving its byte offset.
2. Reduce native render/depth allocation alignment from the temporary
   32-MiB anchor policy to `0x2000`, but reject an allocation whose byte range
   crosses a `0x02000000` allocator-region boundary.
3. Keep `SURF_BASE` relocation equal to that bank-encoded engine address and
   keep `SURF_CFG` unchanged.  Do not introduce an origin register, base-bit
   rewrite, or host-aperture dword-0 substitution.
4. Do not export raw physical mmap as the CPU view of an arbitrary native BO.
   Use a sorted, low-coordinate host-aperture descriptor with the exact native
   control/dimensions, or transfer through a GPU/class-3 staging path.
5. Validate compact allocation first with clear -> anchored BITBLT -> compare
   in each 32-MiB region.  Then test colour/depth role reversal, a coordinate
   ramp, overlapping source/destination copies, and allocations ending one
   tile before each region boundary.
6. Only after those pass, enable compact placement for Mesa and run the normal
   no-vsync `glxgears` correctness/performance regression followed by ioquake3.

The two upper-region full-frame passes complete step 1 for arbitrary starts
and establish that a packet/register workaround is unnecessary.
