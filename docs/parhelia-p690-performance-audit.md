# P690 performance audit: evidence-backed next work

This is a local-only audit of the saved Matrox Linux/Windows artifacts, captures,
and the current open kernel and Gallium driver.  It records only changes whose
mechanism and programming are already evidenced.  It does not turn an AGP-only
or unvalidated vendor switch into a P690 feature.

The target is PCIe P690 (`102b:2539`).  Its usable non-local address class is
raw 32-bit PCI DMA class 3; it has no GPU GART aperture.  See
`parhelia-pcie-p690-vs-pcix-0528.md` §4 and
`parhelia-vertex-fetch-and-memory-classes.md` §§1--2.

## Ranked work

### 1. Coalesce a whole DRM submit into one primary-ring kick

This is the largest confirmed gap.  The open kernel builds every complete
primary CALL first, but then publishes only one CALL at a time and polls idle
after every publication:

```
for each CALL:
        MMIO 0x408 = primary + CALL_end | 3
        poll DMA idle
MMIO 0x408 = primary + final_writeback | 3
MMIO 0x314 = 1
wait for final writeback and 0x41c fence
```

That is the current code at
`linux-7.2/drivers/gpu/drm/parhelia/parhelia_submit.c:900-947`.
It serializes all state, draw, EOB/drain, and final-fence calls.  A Quake batch
therefore pays a PCIe MMIO write plus a CPU busy wait for every secondary block.

The vendor construction is exact and different: `mtx1937` appends the five-word
CALLs to the primary ring, `mtx558` appends its flush/draw calls and the final
`[0x21212425, sequence, writeback-address]`, and only then writes `0x408`.
The audited chain and order are documented as confirmed binary evidence in
`parhelia-3d-submission-audit.md` §§A.4, A.9--A.10 (especially lines 157 and
372 in the current document).  `0x408 = end | 3` is the primary producer
pointer/doorbell; no per-CALL idle wait is present in that sequence.

**Implementation shape.**  Keep `trace_submit` and `full_sync` diagnostic
paths unchanged.  In the normal path, retain the existing `dma_wmb()`, program
`0x400` as today, then make one `0x408` write to
`primary_bus + primary_words * 4 | 3`, followed by `0x314 = 1`, the existing
coherent writeback wait, and the existing `0x41c` fence wait.  The allocation is
already sized for it: 224 primary slots times eight words plus tail room
(`parhelia_drv.h:116-123`), while the maximum user packet count is 221.

**Validation result.** `coalesce_primary` now defaults to true and remains a
read-only load parameter for an emergency rollback. It is deliberately inactive
for `trace_submit` and `full_sync`, which preserve their diagnostic streams.
The P690 target passed single triangle, texture/depth, and vertex-alpha direct
gates, rendered q3dm1 correctly, and improved uncapped `glxgears` from 224.832
to 275.231 FPS. A later isolated-module reload followed by the same
`vblank_mode=0` measurement produced 277.446 FPS. Do not use `trace_submit` as a performance control: its markers
deliberately change the primary stream.

### 2. Make proven sampler tiling the normal large-texture policy

The vendor kernel enables `reg3DUseTextiled` by default
(`matroxdriver-x86_64-cvs-20090511/kernel/src/MtxParhlParms.c:101-102`).  The
saved capture/binary analysis proves sampler layout code 2 and its 32x16 tile
addressing; `parhelia-texture-sampling.md` §§5.1--5.3 provides the exact
descriptor and layout.  The open driver implements that layout, but only when
the environment contains `PARHELIA_TEXTURE_TILED`
(`mesa-26.2.2/src/gallium/drivers/parhelia/parhelia_context.c:487-526`).

Mesa now selects layout 2 automatically only for the tested P690 base-level
shape: a defined 2D 32-bit RGBA/XRGB texture, at least 32x32, with no declared
mip levels and a non-mipmap sampler. Every other automatic case remains layout
0, including sub-32 resources, incomplete uploads, noncanonical formats, and
mip-described textures. `PARHELIA_TEXTURE_TILED=1` retains its force-enable
semantics for the known Q3 launch; P690 mip sampling remains disabled after
the documented hardware timeout.

The target's isolated 64x64 RGBA GL run, without the environment override,
emitted a layout-2 descriptor and retired successfully. Direct texture/depth
validation independently confirms the layout-2 hardware route. Broader
upload-heavy versus steady-state comparisons remain useful before widening the
automatic predicate.

### 3. Preserve asynchronous KMS presentation; measure the compositor path

Vendor swap behavior is fully decoded: flush rendering, enqueue a double-buffer
flip with interval, then apply it from vblank only after the `0x41c` tag retires
(`parhelia-rendertarget.md` §§6.1--6.5).  The current KMS driver has a real
head-0 vblank IRQ and transfers atomic events to DRM before commit completion
(`linux-7.2/drivers/gpu/drm/parhelia/parhelia_kms.c:382-441`).

For maximum benchmark throughput, use the application/GLX swap policy that
requests no wait and measure with `vblank_mode=0`; for normal display use the
DRM page-flip event path at the panel's actual refresh.  There is no evidence
that polling or disabling the IRQ improves rendering.  The remaining
performance task is measurement: verify whether the desktop compositor is
causing blit presentation or queued page flips, then fix that user-space choice
if it limits uncapped FPS.  Do not replace the present KMS start-address update
with the vendor's four-slot scheme: only slot 0 is hardware-confirmed here.

## Already implemented and should be retained

* **P690 class-0 vertex fetch:** Mesa maps a persistent CREATE_VERTEX BO and
  emits bounded `DRAW_VRAM` relocations; it falls back to coherent DMA32 only
  if unavailable (`parhelia_context.c:160-222`).  The kernel restricts it to
  P690, raw aperture ownership, a per-file pinned BO, and bounds-checked draw
  ranges.  This is the correct local-memory zero-copy fetch route.
* **PCIe DMA:** the primary ring, secondary blocks, fallback vertices, and
  writeback page are coherent 32-bit DMA allocations.  The saved vendor
  evidence and P690 hardware evidence both use raw bus addresses with class 3.
* **P690 texture tiling:** the code has matching layout-2 upload and descriptor
  support.  `PARHELIA_TEXTURE_TILED=1` is the current tested fast setting.
* **IRQ/vblank page flips:** the working kernel path acknowledges W1C status,
  calls `drm_crtc_handle_vblank`, and delivers atomic events from vblank.

## Excluded vendor switches

These saved defaults are clues, not implementation candidates, because no
exact P690 register state or safe capture correlates them with a faster path:

| switch | reason for exclusion |
|---|---|
| `reg3DVECacheMode=TRUE` | only a module parameter/default is known; no recovered cache-control programming |
| `reg3DVertexBuffers=FALSE` | it describes an unimplemented vendor allocation API, while the open P690 class-0 ring already has a narrower verified ABI |
| `reg3DTEFastTrilinear=0` | disabled by the vendor and requires mip sampling, which wedges the current P690 probe |
| `reg3DForceAGPTextures=FALSE`, `agp`, `agprate`, `fw` | the target is PCIe and its AGP bridge is unrelated; its memory ABI exposes no GART aperture |
| `reg3DUseIndirectPipes=FALSE` | vendor source explicitly says those modes cannot be used on real hardware |
| FAA/GigaColor settings | their programming remains incomplete and is separately documented as unsupported |

## Result

The actionable, evidence-backed optimization is primary-ring coalescing.  It
removes a current artificial CPU/GPU serialization point without changing
packet format, WARP state, memory class, or the final completion contract.
Texture layout-2 defaulting is the next safe policy improvement after image
comparison.  There is no valid P690 AGP/PCIe GART feature to expose beyond the
existing raw-DMA and class-0 VRAM paths.
