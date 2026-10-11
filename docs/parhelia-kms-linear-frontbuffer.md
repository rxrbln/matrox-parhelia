# Linear userspace front buffer with native tiled scanout

This file documents the compatibility path currently implemented by the open
driver.  It is intentionally one step short of the vendor architecture.  The
zero-resolve target, including the binary evidence for a CPU-linear host
aperture onto a native scanout allocation, is documented in
`parhelia-vendor-linear-aperture-design.md`.

## Chosen path

The KMS ABI exposes ordinary linear dumb and PRIME buffers.  The primary plane
keeps a private, pinned native-layout scanout object.  A standard `DIRTYFB`
commit carries Xorg Damage rectangles to the plane update, where the GPU copies
only those rectangles from the public linear framebuffer to the private
scanout object.

`mode_config.prefer_shadow` is therefore false.  With unaccelerated modesetting
Xorg this makes the screen pixmap point directly at the mapped dumb BO.  Xorg
still installs its Damage record after its initial `drmModeDirtyFB()` probe and
dispatches the resulting rectangles with `drmModeDirtyFB()`.  The kernel uses
`drm_gem_fb_create_with_dirty()`, `drm_atomic_helper_dirtyfb()` and
`FB_DAMAGE_CLIPS`, so no Xorg driver-specific copy or layout knowledge is
needed.

This removes one complete copy from the old path:

```
Xorg system ShadowFB -> linear VRAM dumb BO -> native private scanout BO
```

The direct path is:

```
linear VRAM dumb BO -> native private scanout BO
```

The first arrow in the old path was a CPU `shadowUpdatePacked()` copy.  The
remaining layout resolve is a synchronous GPU blit and is restricted to the
reported damage, expanded outward to the native 64-by-tile-height boundaries.
The expansion is required for type-3 destination writes: issuing the exact
unaligned Present rectangle produces fragments within the terminal tiles even
though a full-frame blit is correct.  Since the source is the complete linear
front buffer, copying the small border around the damage preserves its current
contents.

The DRM shadow-plane helpers used in `parhelia_kms.c` are unrelated to Xorg's
ShadowFB.  They pin and vmap the source GEM object so the CPU tile converter can
remain as a fallback if the GPU resolve is unavailable.

## Why direct native scanout is not the current dumb-buffer ABI

The display engine consumes the native physical layout.  A dumb buffer must be
linearly CPU-addressable, and applications assume that byte offset
`y * pitch + x * cpp` names a pixel.  Programming a host-aperture translation
for an arbitrary public GEM allocation violates that contract for other GEM
and PRIME mappings sharing the aperture.  It was also the cause of scattered
window contents during bring-up.

The proprietary stack gives the primary surface a native type-3 2D/display
view and uses the host-aperture machinery for CPU access.  Its surface manager
owns allocation classes and aperture mappings together, so it can safely give
the CPU a linear view of native storage.  The vendor X driver passes that
BAR-derived aperture pointer directly to `fbScreenInit`; it does not use a
software primary shadow.  Generic dumb/PRIME mmap has no API to describe such
an address-transforming map by itself.

## Future zero-resolve path

Native buffers can later be exposed as an optional, Matrox-specific DRM format
modifier.  Mesa would allocate and render to that modifier and KMS could scan
it out directly.  A custom GEM mmap maps the descriptor's BAR aperture offset,
giving the CPU a linear row-major view while scanout and the GPU use the native
physical layout.  Linear dumb buffers remain the fallback.  This needs a
stable modifier definition, modifier-aware GEM allocation/mmap, aperture-slot
lifetime management, Mesa resource import/export, and plane validation.
Advertising a modifier before those pieces exist would let generic userspace
create buffers it cannot address correctly.

Kernel page-dirty tracking or deferred I/O is not a suitable replacement for
`DIRTYFB`: writes to an existing PFN-mapped VRAM VMA do not provide precise X11
damage rectangles, and scanning PTE dirty state would add page-granularity CPU
work every frame.  Xorg already provides exact damage through the standard KMS
interface.

## Validation requirements

After loading a temporary module and restarting X without an explicit
`Option "ShadowFB" "true"`, the Xorg log must report:

```
ShadowFB: preferred NO, enabled NO
Damage tracking initialized
```

Kernel logs must report the GPU linear-to-native KMS resolve and no CPU
fallback or timeout.  Validate a full-screen root pattern, moving GL window,
uncapped `glxgears`, hardware cursor, and mode changes.  Compare FPS and CPU
usage against the shadow-enabled run.
