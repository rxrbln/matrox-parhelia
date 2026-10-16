# Sharing M9 buffers: dma-buf, format modifiers, and what a compositor needs

What the driver can hand to another process, and what it deliberately cannot.
Written 2026-09-18 from measurement on the M9128; the fixture is
`tools/m9-dmabuf-test`.

## dma-buf, as it actually behaves

MEASURED with a standalone probe in one locked session, boot id unchanged from
start to finish, each module gated on `m9-mesa-glsl texture` reading 0 of 76800
pixels wrong before any result was trusted.

| BO class | export | self-import | foreign import (vgem) | `mmap` of the dma_buf |
|---|---|---|---|---|
| DMA32 staging / command | ok | ok, same handle | now ok | ok |
| VRAM linear / tiled | ok | ok, same handle | `-ENOSYS` | ok |
| engine-only VRAM | now refused | ok | `-ENOSYS` | `-EINVAL` |
| a foreign dma_buf imported into m9x | - | - | - | `-EINVAL` |

**Export and import already worked before any of this.**  DRM core serves both
without a driver hook: `drm_gem_prime_export()` builds the dma_buf, and
`drm_gem_prime_import_dev()` returns the very same object for a buffer this
device exported.  Same-device export and re-import is the whole of what a
compositor on this GPU does, so `caps->dmabuf` was honest for that path.

What was missing was every *foreign* importer, because
`drm_gem_map_attach()` refuses an object with no `->get_sg_table`.  That is now
provided for the contiguous DMA32 class and the paged class.  VRAM deliberately
keeps none: it is a framebuffer BAR rather than pages, and sharing it would
need P2PDMA.

Engine-only VRAM used to hand out a working fd for memory **nothing can map** -
it has no mmap, no vmap and no CPU window by design.  Its `->export` now
refuses.

## The tiled format modifiers

Colour targets on this hardware are stored tiled, which is why direct scanout
works at all.  `m9_resource_get_handle` used to stamp every export
`DRM_FORMAT_MOD_LINEAR`.  It was not refusing tiled buffers - its one refusal
is for texture-*chain* resources, which have no single stride - so a tiled
buffer went out **described wrong**, which a foreign importer would render
scrambled with no error to show for it.

Vendor namespace `DRM_FORMAT_MOD_VENDOR_MATROX = 0x4d` ('M'), chosen away from
the sequentially allocated upstream ids.  **This is not an allocated upstream
vendor namespace.**  It is private to this out-of-tree driver until registered,
and that is stated in the uapi header as well as here.

Two values are needed, because one cannot describe both families - reading the
small family with the ordinary kind returns only the first 16x16 texels:

| modifier | layout |
|---|---|
| `DRM_FORMAT_MOD_MATROX_M9_TILED` | row span padded to 32 texels, allocation height to 32 rows, surface to 4 KiB at 32 bpp or 2 KiB at 16 |
| `DRM_FORMAT_MOD_MATROX_M9_TILED_SMALL` | the <=64x64 aperture-calculator family; height quantum 8 or 4, surface 1 KiB or 512 B |

**The modifiers name the shape contract only.**  The texel order inside a band
is the calculator's own and is *not* affine in the coordinates, so no importer
may walk one as rows.  The texture chain's order is a third, different function
and is never exported at all, so it needs no modifier.

`MOD_INVALID` keeps its legacy meaning - nothing negotiated, the kernel's answer
stands alone - so the existing DRI3 and GBM paths are untouched.

### What the fixture proves, and what it does not

| case | before | after |
|---|---|---|
| `export` | modifier reported `0` - FAIL | reports the real modifier - PASS |
| `refuselinear` | exported linear, nothing to refuse - FAIL | refused - PASS |
| `roundtrip` | PASS | PASS, 0 of 65536 texels wrong |

`roundtrip` **does not discriminate**: it passed before the change because both
ends were this same driver, so the mislabelling cancelled out.  `export` and
`refuselinear` are the two that fail on the pre-change binary, and they are what
the description being true is worth.

## What a Wayland compositor still needs

1. **The KMS plane advertises no modifiers.**  `drm_simple_display_pipe_init()`
   passes `NULL` for `format_modifiers`, so `IN_FORMATS` shows LINEAR only -
   although the plane check already accepts a tiled framebuffer directly.  A
   modifier-aware compositor is therefore told tiled scanout is unsupported and
   falls back to plain `AddFB2`, which does work.  Adding
   `DRM_FORMAT_MOD_MATROX_M9_TILED` to that list is the next step for direct
   scanout; it is uncontended and testable with modetest.
2. **No explicit sync.**  `m9_submit.c` rejects `out_syncobj`; there is no
   `drm_syncobj` and no `dma_fence` plumbing.  Needed for tear-free
   compositing rather than for correctness.
3. **No R8 or RG8**, so no NV12 plane sampling and no `GL_RED` glyph atlas.
   This is a hardware limit rather than a missing decode - see
   `parhelia-m9-implementation-backlog.md`, "Established as unreachable".
4. `max_texture_2d_size` is 2048, so a single-texture 4K output is out of
   reach.  `glsl_feature_level` is 120, which satisfies a GLES2 renderer but
   not a Vulkan one.

Already in place: `DRIVER_MODESET | DRIVER_ATOMIC` with universal planes and a
real cursor plane, per-head vblank on all four heads, a render node, and
extended desktop as a per-head window of a shared framebuffer.

## Two traps for anyone adding a format

* **Two unit systems in one driver.**  `m9_format.c` stores the un-shifted
  7-bit code; `m9_texture.c` stores `code << 1 | bit24`.  L8 is `0x30` in one
  and `0x61` in the other.
* **`m9_format.c` gives `PIPE_FORMAT_I8_UNORM` a cpp of 1, and every source
  gives `0x51`/`a2` a 16-bit element** ("I I, stored as LA"; its measured
  allocation is identical to LA8).  Latent today only because the texture path
  stores everything at 32 bpp.
