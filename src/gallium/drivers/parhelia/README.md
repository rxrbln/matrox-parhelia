# Parhelia Gallium driver

This is a native Gallium DRM driver. The normal Mesa DRI build exports the
Parhelia screen through the DRM pipe loader and installs the `parhelia_dri`
megadriver link.
Enable with `-Dgallium-drivers=parhelia,softpipe`.

`reference/` preserves the supplied `xf86-video-parhelia/src/parhelia3d` builder,
state, clear, indexing, tiling and header files with their original GPL-2.0 attribution.
No DDX implementation or userspace MMIO code is linked. WARP upload and the
global fence counter belong exclusively to the kernel. The scratch primary
ring produced by the reference is never submitted to the kernel.

`PARHELIA_BATCH_STATE_STREAM=1` queues complete ordered 3D state sequences
behind one terminal EOB/DWGSYNC. Leading colour and depth clears share that
submit; a clear or blit after rendering remains a drain boundary. The driver
accounts for the kernel's hidden WARP-stage CALLs as well as packet, command,
vertex, relocation, and resource limits before appending each sequence.
When the kernel exposes `CREATE_VERTEX`, aggregation uses the larger bounded
local-VRAM ring instead of inheriting the 4096-dword class-3 copy limit.

The adapter also accepts transformed/lit fixed-function reference state.
`parhelia_batch_reference()` imports the COMPLETE original C gears scene,
including GPU colour/depth clears, grouped constants and face-group boundaries.
The repository's `tools/check-parhelia-gears.sh` builds the original generator
without changing its sources and verifies the adapter under ASan/UBSan.

Hardware correctness is verified for the full 800-triangle C gears scene:
0/5/10-degree frames match the documented result (100/112/117 pixels outside
2/255 tolerance). Geometry transformation, lighting, depth and rasterization
run on the GPU.

Use `parhelia_create_render()` for render targets, NOT tightly packed dumb
buffers. The experimental kernel reserves conservative 20MiB extents with
8KiB alignment: a 3MiB-spaced colour/Z pair aliases and corrupts both buffers,
while 20MiB separation is hardware-tested. The physical banking formula is
still unresolved; this is a bounded bring-up policy, not a universal footprint
calculation. Logical allocation dimensions currently cannot exceed1024x768.
The returned mmap offset supports the render node without MAP_DUMB. Allocation
requires the kernel's opt-in `init_aperture=1` baseline; inherited display
apertures can translate or hide CPU readback. No userspace MMIO setup is needed.

`parhelia-gears-drm --render-kernel-allocation PREFIX
/dev/dri/renderD128 REFERENCE.ppm OUTPUT.ppm` passes the full frame using this
interface, with no hard-coded GPU addresses or low-window reservation.
The current hardware path supports the recovered fixed-function state,
tiled colour/depth targets, single-texture draws, and the draw-module vertex
frontend. Combined texture-and-depth descriptors, two-unit texture combining,
and mip-level selection still require capture-backed hardware work.
