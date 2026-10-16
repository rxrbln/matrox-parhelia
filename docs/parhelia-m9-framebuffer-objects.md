<!-- Copyright 2026 René Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
# M9 framebuffer objects in the m9x Mesa driver

State of GL_ARB_framebuffer_object on the M9128 (nano), 2026-10-09, agent
m9fbo. `MEASURED` = piglit or probe on the card; `OFFLINE` = host build and
unit tests only.

## What is advertised

EXT_framebuffer_object, EXT_framebuffer_blit and EXT_packed_depth_stencil
were always exposed by core Mesa. ARB_framebuffer_object depends on the
Gallium cap `mixed_framebuffer_sizes`, which m9x now sets (Mesa m9x
c2a8740). With it, piglit's `-fbo` runs every core test in an FBO instead
of the window. Not advertised: multisample (no MSAA visuals or storage),
`mixed_colorbuffer_formats` (MRT attachments of different formats are
reported FRAMEBUFFER_UNSUPPORTED), ARB_framebuffer_no_attachments.
Colour-renderable formats stay BGRA8/BGRX8 and the 10-bit ones; GL_RGB565,
GL_RGBA4 and similar render into BGRA8, luminance/alpha/intensity textures
report FRAMEBUFFER_UNSUPPORTED. Depth/stencil attachments are D24S8
(DEPTH_COMPONENT16/24/32 and STENCIL_INDEX* fall back to it); separate depth
and stencil buffers are UNSUPPORTED (Gallium policy).

## Implementation

| commit (m9fbo / main) | change |
|---|---|
| c2a8740 | mixed attachment sizes: clip window = intersection of the attachments, depth/HZ descriptors from the depth attachment's own extent, MRT targets past the first with their own descriptors; depth proxy for levels of mipmapped depth textures |
| c641ffb | colourless fragment shaders (depth/stencil-only draws, u_blitter's empty FS) get a constant output with all colour writes masked; they were dropped ("no color output") |
| f43509b/e0393a3, 5d0913b/3b85f60, 083097d/f503d17 | kill-state drain, query fix, scratch-target drain (see parhelia-m9-kill-state-boundary.md) |
| ab5ee90/045629e | CPU blit: volume mipmaps (z-filtered slices), bilinear depth mipmaps |
| d62996d/b57ec1b | glClear depth stored as the nearest D24 code (was floor) |
| 9923059/0eeca30 | sampler LOD range clamped to the view's level range; base level only without mip filter |
| 649a644/f6c569d | draw buffers of NONE and outputs past the bound run: per-target split passes; discarding multiple-target shaders the same way |

The engine rasterises in top-down surface rows and addresses every colour,
depth and HZ surface through a descriptor of its own row span (depth spans
64-aligned), so attachments of different extents need no common layout; the
viewport and y-flip constants follow colour target 0 (MEASURED with the
mixed-size probe `tools/m9fbo-probes/fbomix.c`: 7 extent pairs, depth and
stencil, exact).

## piglit (MEASURED)

FBO set = spec@ext_framebuffer_object, arb_framebuffer_object,
ext_framebuffer_blit, ext_packed_depth_stencil and every test named *fbo*
(419 tests): 73 PASS / 59 FAIL before, 163 after c2a8740, and on main
6019d5e (with m9piglit4's stop-slot fix) 225 PASS / 13 FAIL / 176 SKIP.
Core list m9x-basic (452) with `-fbo` effective: 268 / 17 on c641ffb,
281 PASS / 4 FAIL on 6019d5e (window-mode reference 272 / 10); the four are
depth-clear-precision-check, two stipple tests (fixed on later main) and
pbo.

## Open (with cause)

* Cube maps above 16 texels are opt-in (M9X_CUBE_64): fbo-cubemap,
  fbo-incomplete-texture-02/03 and fbo-generatemipmap-cubemap fail at the
  default limit and PASS with M9X_CUBE_64=1 (MEASURED, a pixel receipt for
  32-texel render-to-face and cube mipmap generation); the S3TC DXT1 cube
  variant fails either way.
* gl_FragDepth is stored one code above GL's for ~1/3 of depths
  (depth-clear-precision-check); a code-rounding lowering gets 856/1256
  exact, not all, so it is not merged.
* fbo-alphatest-nocolor(+ff) PASS on 6019d5e; before it (drifted kill
  lanes): in a depth-only FBO the alpha-tested draw
  whose fragments are all discarded still writes depth (passes on llvmpipe
  with -fbo). Not the colour mask: leaving colour writes on into the
  scratch surface changed nothing (MEASURED). The job runs ef 0000f000;
  the vendor captures with kill have depth off, so whether a kill program
  needs the late depth mode gl_FragDepth uses (ef 0x0800) is unknown and
  untested (stall risk without vendor evidence).
* fbo-blit-stretch fails under -fbo on llvmpipe too (its verify() reads
  framebuffer 0, the never-drawn window, instead of piglit_winsys_fbo); it
  passes on llvmpipe in window mode. FBO-to-FBO blits are exact on the M9
  (blitprobe: unscaled, stretched, flipped, full).
* texwrap of depth formats, fbo-pbo-readpixels-small (uses
  DEPTH_COMPONENT32F without requiring ARB_depth_buffer_float).

## Closed by m9fbo5 (2026-10-09)

* fbo-fragcoord2 PASS in FBO and window (MEASURED, Mesa m9x 478facb).  The
  emulated vendor compiler reads `7b` twice in one program, once with the
  facing select and once with the z select
  (`captures/m9x-vendor-glsl-fragcoord-facing-20261009`), so the refusal of
  gl_FragCoord beside gl_FrontFacing had no basis.  The generic
  gl_FragCoord path also carried z in the wrong operand.
* S3TC fbo-generatemipmap-formats (s3tc, arb_texture_compression,
  sRGB-s3tc) PASS (MEASURED, f499643 + bccb095): the vendor's small-kind
  column limit and the logical sampler extent (s3tc doc 12.6).  The DXT1 64x69
  level still needs drm 927f76a.  Without it the level is refused cleanly,
  and piglit does not probe it.
* Open: gl_FragCoord.z/w from `7b` read about (1, 1) in FBOs and (0, 0) in
  the window (glsl-fs-fragcoord-zw-*, fragcoord_w; failing since 10-02).  The
  vendor's own fs-fragcoord capture reads z = 0 too.
