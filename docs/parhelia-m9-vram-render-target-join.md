# M9148 local-VRAM render-target and KMS scanout join

This note closes the address-domain join required to render with the M9 3D
engine into a GEM object that the display controller can scan out.  On the
captured M9148, a local-memory command address is the fixed internal base
`0x10000000` plus the byte offset in PCI BAR0:

```text
selector-8 b0 address = 0x10000000 + drm_gem_vram_offset(gbo) + view_offset
DC0 START             =              drm_gem_vram_offset(gbo)
CPU BAR mapping       = BAR0 CPU mapping + same byte offset
```

The first equation is valid only for a TTM VRAM object pinned in BAR0.  It is
not valid for the private DMA32 command/resource objects.  Conversely, the
display controller takes a zero-based BAR0 offset and must not receive the
`0x10000000` engine-domain base.

Evidence in this note is checked by:

```sh
python3 tools/m9-vram-render-target-evidence.py
```

## Captured address domains

The controlled GL2 FBO fixture contains 54 allocation responses.  Each is a
560-byte response with allocation kind at `+0x08`, placement at `+0x20`, GPU
base at `+0x208`, map token at `+0x210`, size at `+0x218`, and alignment at
`+0x21c`.

All placement-4 allocations in that fixture fall in
`0x10000000..0x1fffffff`.  All placement-1 allocations fall in the separate
`0x40xxxxxx` system-memory domain.  Two exact local allocations join the
commands:

| allocation | tuple `(kind, placement, GPU base, map token, size, alignment)` | use |
|---:|---|---|
| 41 | `(0x10, 4, 0x11ee9000, 0x11ee9000, 0x11000, 0x200)` | window color target |
| 48 | `(0x10, 4, 0x11f41000, 0x11f41000, 0x1400, 0x80)` | FBO color target |

The producer binds allocation 48 at `b0/b1 = 11f41000/c8010020`.  The later
consumer binds that exact pair at texture indices `78/79`, while its window
destination is allocation 41 at `b0/b1 = 11ee9000/cc210080`.  Producer and
sampled consumer both read back `(77,102,147,255)`.  This proves that a local
placement-4 allocation can be a writable selector-8 render target and later a
sampled resource.  Every selector-8 target in these two captured submissions
is in the `0x1xxxxxxx` local domain; no selector-8 target uses a captured
placement-1 `0x40xxxxxx` address.

The process maps 256 MiB at physical offset `0xd0000000`, the captured BAR0.
The saved vendor kernel source independently defines the framebuffer region
as PCI resource 0 and maps that whole region in
`KernelClientMapLocalLinMemory`.  Therefore the two captured target addresses
name BAR offsets:

```text
0x11ee9000 - 0x10000000 = BAR0 + 0x01ee9000
0x11f41000 - 0x10000000 = BAR0 + 0x01f41000
```

The physical BAR base is machine configuration.  It never appears in a 3D
packet.  The engine-domain base `0x10000000` is the value recovered for this
M9148 and is independent of the host-assigned BAR address.

## Linear color descriptor shared with scanout

The selector-8 descriptor layout recovered from the vendor compiler is:

| bits | meaning |
|---:|---|
| `15:0` | aligned horizontal row span in format elements |
| `18:16` | allocation-layout selector |
| `21` | layout/address-offset mode |
| `22` | special blocked view |
| `23` | resource-layout flag |
| `31:25` | hardware format code |

The captured window render target decodes as:

```text
cc210080: format 0x66, span 128, layout 1, bit21=1, bit22=0, bit23=0
```

The read-only warm display snapshot has `DC0_START=0` and
`DC0_PITCH=cc210400`, which decodes to the same upper `cc21` descriptor family
with span 1024.  Format `0x66` is the recovered X8R8G8B8/A8R8G8B8/R8G8B8
32-bit family.  This is the direct join between a captured linear window
render target and the KMS 1024-pixel XRGB8888 scanout.  For the guarded warm
mode, the render descriptor is formed by preserving the proven `cc21` high
half and inserting the pitch in pixels.

The FBO descriptor `c8010020` has format `0x64`, span 32, layout 1, and none
of bits 21..23 set.  It demonstrates a different valid local layout and must
not be used as the linear scanout recipe.

## Required engine address windows

Vendor Linux core function `m9x421` programs internal manager windows before
submitting the matching indexed state.  The saved Linux M9148 descriptor and
manager construction produce:

| MMIO | value |
|---:|---:|
| `0x360` | `0x00861000` |
| `0x364` | `0x008e0000` |
| `0xf64` | `0x008e1000` |
| `0xf68` | `0x00940000` |
| `0xf74` | `0x00801000` |
| `0xf78` | `0x00801000` |
| `0xf7c` | `0x00860000` |
| `0x1320/24/28` | untouched (`C+0x24=0`) |
| `0x132c` | `0x00801000` |
| `0x1330` | `0x00801000` |
| `0x1334` | `0x00860000` |

The corresponding indexed mirrors are:

```text
45cdcccb: cb=base, cc=base, cd=limit
45cac9c8: c8=base, c9=base, ca=limit (only when companion size != 0)
```

The `0x10000000..0x1fffffff` interval above belongs to the separate local
render-resource allocator.  It is not an input to these `m9x421` windows.

The vendor transition also emits index `47` values `0x11a4` then `0x1308`,
`e6=0x40000000`, and index `22 = read(0x1088) | 0x20000000`, with submission
and requester waits around the groups.  A clean-room execution path must use
the recovered transition and waits; writing only `b0` does not establish a
render-ready address space.  The unbound warm snapshot had the aperture,
context, and surface windows cleared, so successful no-op requester traffic is
not evidence that these render windows are already armed.

## System-memory color targets are not established

The saved drivers clearly use DMA32 system memory for requester pages,
command streams, fences, vertex/input data, linked state, and other resources.
Those addresses occupy the `0x40xxxxxx` domain and use resource-specific low
tags.  The selected live render captures use placement-4 local memory for
every selector-8 `b0` target.  Neither the static selector-8 construction nor
the captures establish a linear DMA32 system allocation as a writable color
target.

The clean-room submit validator must therefore require a positively typed
VRAM GEM for `PARHELIA_M9_RELOC_COLOR`.  It must continue to require the
private, per-file DMA object registry for command and non-color resource
relocations.  Treating every GEM as a `drm_gem_dma_object`, or accepting a
DMA32 object for `COLOR` merely because its address fits in 32 bits, combines
two different GPU address domains and can direct rendering into arbitrary
host memory.

Depth `b8/ba` and texture `78` are also local-memory addresses in the vendor
captures, but their clean-room relocation conversion is outside this focused
color-target change.  They should be converted only with their own layout,
pinning, and access tests.  A FBO color object will eventually need to be
accepted as `TEXTURE` in a later submission to implement the proven
render-to-texture chain.

## Pinning, ownership, and command validation

`drm_gem_vram_offset()` is defined only for an object pinned in VRAM.  The DRM
VRAM plane helper pins the active KMS framebuffer during plane access, so its
offset remains stable while it is scanned out.  The focused kernel change:

1. gives M9 KMS VRAM GEMs private object functions and a positive
   `m9x_gem_is_vram()` identity;
2. keeps the command buffer lookup on `m9x_bo_lookup()`, including its
   per-file private allocation registry;
3. keeps every non-color relocation on that same DMA object type and its
   existing address/tag rules;
4. accepts `COLOR` only for a typed, already-pinned VRAM GEM;
5. bounds `VRAM offset + target offset` by both BAR0 size and the 256-MiB
   M9148 engine domain, then adds `0x10000000`;
6. retains the existing 256-byte color alignment and READ|WRITE access
   requirement.

An unpinned VRAM object fails with `-ENODEV`; the validator does not silently
use offset zero.  The submit ioctl is still validate-only and discards the
patched image, so this change does not enable user command execution.

Cross-file scanout sharing remains a separate UAPI task.  Mesa normally
receives a display buffer through PRIME, but this provisional driver does not
yet implement VRAM PRIME import/export.  A first diagnostic can use a handle
owned by the same DRM file; the production DRI3 path needs explicit sharing
and reservation-fence integration.

## Ordering and coherency

The captured FBO producer-to-consumer chain becomes visible with normal
resource dependency ordering.  No additional texture-specific cache packet
appears in that chain.  This supports the following conservative rules:

* retain the captured index-`43` dependency and normal requester/fence order
  when a rendered color target is rebound for sampling;
* attach the render fence to the GEM reservation object before making the BO
  available to the display or another client;
* wait through the ordinary atomic/implicit-fence path before page flip;
* publish CPU writes to WC BAR mappings before GPU or display fetch, and do
  not infer CPU/GPU completion from a memory barrier alone.

The capture does not prove that a dedicated cache invalidate can never be
needed for CPU-written VRAM or an address/layout transition.  It proves that
one is absent from the tested GPU-render-to-GPU-sample chain.  Front-buffer
rendering also needs vblank-aware scheduling to avoid tearing even when the
address and cache rules are correct.

## Remaining execution gates

The local color-address relocation is now mechanically closed, but direct 3D
scanout still requires the following work before enabling general execution:

* install the complete `m9x421` address-window transition at the guarded 3D
  engine bring-up boundary;
* retain a VRAM pin and reservation ownership through the hardware fence;
* add a safe framebuffer-sharing path between KMS and Mesa;
* convert local depth and texture resources with their independently verified
  layouts;
* select `b1` from the actual format, pitch, and layout instead of trusting a
  userspace descriptor word;
* submit only kernel-validated templates until every executable state mask is
  complete.

These gates explain why changing the `b0` arithmetic alone must not turn on
the userspace command path.
