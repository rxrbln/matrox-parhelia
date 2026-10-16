# M9128 odd-height filtered-blit stage triage

The archived GL fixture does not yet isolate a filtered-sampler defect.
`blitlin:75x37` differs in 2,452 final pixels and `blitlin:100x37` in
3,288, but `blitnear:100x37` also differs in 3,792. The nearest control has
no bilinear interpolation. Both `blitlin:100x40` and `blitcpu:100x40`
pass, while `blitcpu:75x37` and `blitcpu:100x37` each differ in 75,138
pixels. Those observations distinguish an odd-height or transfer boundary
from an interpolation-only explanation; they do not identify its producer.
The counts come from `tools/m9-mesa-glsl.c` and
`docs/parhelia-m9-implementation-backlog.md`; no raw intermediate images
were archived with them.

The original M9128 `MxMD.dll` resource-copy path at
`0xb21b0 -> 0x56a10/0x56c60 -> 0x1b8d0 -> 0x1b650` takes logical
rectangles and resource descriptors (see
`parhelia-m9-texture-upload-and-copy-re.md` and
`parhelia-m9-texture-coherency-re.md`). Its source-qualified structure does
not establish a scaled, filtered sampler rule for Mesa's `util_blitter`
draw. The archived size ratios alone cannot justify changing native
coordinates, filter state, or a dimension cap. In particular, a rounded
fractional-weight model can explain some non-halving differences, but not
the failing nearest control.

## CPU window contract checked without the target

The failing CPU counts were recorded after Mesa commit `cd61a2c`
(`m9_surface_even_height`), so they are not observations from the old
odd-height descriptor. For a 37×37 BGRA8 render target,
`m9_render_surface_layout` produces a 64-texel row span, 40 allocated
rows, and 10,240 bytes. `m9_surface_access` sends the small-kind window
descriptor `d0=cc010040`, `d1=00010026` (38 view rows). The kernel's
`m9_surface_min_bytes(64,38,4)` also requires 10,240 bytes. The
40×40 passing control uses the same row span, allocated rows, and capacity.
Thus the host-side capacity and class admission do not explain why the
37-high CPU case differs. The raster/sampler view retains logical height
37; the window's even view height is intentional and was measured to fix
other odd surfaces.

The source-backed Linux `m9x474`/`m9x587` local address formulas map
coordinates 0..36 identically for declared heights 37, 38 and 40 at a
64-texel span and depth zero. This only excludes a height-dependent
**local** address change; the CPU window uses the aperture calculator,
whose selector inputs have not been joined to this live descriptor.
Using the local formula as a replacement for the window would cross a
different address class and is not justified.

`m9_blit_convert` computes a packed, CPU-bilinear destination in a
staging allocation, then hands it to a void `pipe_texture_unmap`.
The unmap calls `M9_SURFACE_ACCESS WRITE`; its failure is warned but
cannot change `m9_blit_convert`'s success result. The kernel-side size
check above predicts admission, not a successful physical window write.
The nearly complete CPU mismatch therefore still needs the actual
source/destination stage files and the window ioctl result. A later fix
should target the demonstrated failing producer or consumer, without
changing valid dimensions or substituting captured addresses.

An opt-in fixture capture now retains three distinct observations: source
FBO readback before the blit, destination FBO readback after the blit, and
the existing final 320x240 sampled output. On a future locked M9128 run,
use the opt-in Mesa `M9X_BLIT_WINDOW_TRACE=1` build (commit
`531fc6980cef1cae1f1bcfc600ac546ae74af332`). It records each colour
window READ/WRITE ioctl's live GEM handle, dimensions, box, descriptor,
capacity and signed return code. This makes a void transfer-unmap WRITE
failure visible beside the raw files.

On the established healthy 1024×768 target session, after FAA gets the
first hardware slot, run this from the RE checkout. The suite gives every
case a fresh process, stops on a new stream timeout and saves the ioctl
receipts beside each three-stage capture:

```sh
M9X_MESA=/root/parhelia-m9x-mesa \
  tools/m9x-hw-lock.sh tools/m9-blit-stage-suite.sh \
  /root/tmp/m9-blit-stages-20261007
```

For one controlled case instead, use a unique prefix:

```sh
M9X_BLIT_WINDOW_TRACE=1 \
M9_GLSL_BLIT_STAGES=/tmp/blit-100x37-near \
M9_GLSL_RAW=/tmp/blit-100x37-near.final.rgba \
  m9-mesa-glsl blitnear:100x37
python3 tools/m9-blit-stage-analyze.py \
  --prefix /tmp/blit-100x37-near --src-size 100 --dst-size 37 \
  --filter nearest --final /tmp/blit-100x37-near.final.rgba
```

The stage files are `PREFIX.src.NxN.rgba` and
`PREFIX.dst.NxN.rgba`, raw GL RGBA8 with the bottom row first. Run
`blitlin:75x37`, `blitlin:74x37`, `blitlin:100x40`, and matching
`blitcpu` cases with their own prefixes. The analyzer reports per-row
source, destination, and final mismatches; the suite also saves
`PREFIX.windows` with all issued colour-window ioctl receipts and the
full `PREFIX.log`. A missing receipt means that path did not issue that
ioctl; it is not a success report. Its host-only self-test checks
four exact size/filter constructions, a single damaged row and a truncated
input.

FBO `glReadPixels` itself uses a CPU transfer. Thus a bad destination
stage file alone may be a readback fault even when the GPU-resident image
is correct. A clean destination file with bad final sampling implicates
the later sampled draw or its descriptor. A bad source file prevents
attributing destination differences to the blit. Compare the final sampled
image and both engine and CPU routes before changing either producer.
The capture is read-only and adds no GPU command replay or captured address.
