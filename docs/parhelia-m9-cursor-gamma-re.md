# M9/Phoenix cursor and scanout-gamma static reverse engineering

This note closes the **saved-driver contract** for the Phoenix/M9 hardware
cursor and primary display LUT.  It is an offline result: no test machine was
contacted.  `tools/m9-cursor-gamma-evidence.py` pins the binaries and the
machine-code ranges cited below.

Evidence labels used here are:

* **[M9LIN]** `m9x_drv.so`, SHA-256
  `15ff4ad6e450c2d5a27c3d114011014e8aaf97851d107a070f31baa26eafc7e5`.
* **[M9WIN]** `MSerX/MxMDI.dll`, SHA-256
  `79c8fad0eb9e0effcb63411be39b8bacfad24dda4a0e60a22d28f506e319b2e9`.
* **[HW]** earlier live observations already recorded in `parhelia-m9.md`.

The Linux names are stripped `m9xNNN` exports.  Their roles are joined to the
driver's diagnostic-name table and to their behavior.  Windows RVAs below are
relative to image base `0x10000000`.

## Hardware cursor

### Register banks and routing

Every cursor operation in the Linux display module computes the bank as:

```c
u32 cursor_base(unsigned route)
{
    return (0x18c + ((route & 1) << 5) + (route & 2)) << 4;
}
```

This produces four banks:

| route | base | data/palette | control | position | address | config |
|---:|---:|---:|---:|---:|---:|---:|
| 0 | `0x18c0` | `0x18c0` | `0x18c4` | `0x18c8` | `0x18cc` | `0x18d0` |
| 1 | `0x1ac0` | `0x1ac0` | `0x1ac4` | `0x1ac8` | `0x1acc` | `0x1ad0` |
| 2 | `0x18e0` | `0x18e0` | `0x18e4` | `0x18e8` | `0x18ec` | `0x18f0` |
| 3 | `0x1ae0` | `0x1ae0` | `0x1ae4` | `0x1ae8` | `0x1aec` | `0x1af0` |

The calculation occurs independently in position, show, hide, color selection,
and both image upload paths.  The caller wrappers select an object from
`object[route].cursor` (`m9x2413`, `m9x8070`, `m9x14934`, `m9x27432`, and
`m9x19725`, RVAs `0x2902f0..0x290365`).  Thus the saved driver treats the
selector as four independent display-controller routes.  The existing DRM
model still exposes only two physical CRTCs; routes 2 and 3 are secondary
controller paths and must not be exposed as extra physical heads.

The cursor object constructor (`m9x7398`, RVA `0x294ed0`) stores the route at
object offset `+0x0c`, sets mode and visibility to zero, and retains the board
MMIO and surface-manager objects.  All register writes go through the same
ordered MMIO accessor.  None of the leaf cursor routines polls a ready bit,
waits for vblank, or emits a command-stream fence.

### Position, origin, hotspot, and clipping

`m9x9995` at RVA `0x295070` programs:

```c
u32 px = (x + 64) & 0x7fff;
u32 py = (y + 64) & 0x7fff;
mmio_write(base + 8, (px << 16) | py);
```

The X coordinate occupies bits 30:16 and Y occupies bits 14:0.  The `+64`
origin lets a 64-pixel cursor move partly off the top or left: coordinate
`-64` encodes zero and `-1` encodes 63.  There is no clipping operation in the
hardware leaf routine.  Values below `-64` wrap in the 15-bit field, so the
caller must hide a cursor that is completely outside the visible scanout.

No hotspot fields or hotspot register writes occur in the cursor object or
leaf routines.  The X/DRM layer must subtract `(hot_x, hot_y)` before passing
coordinates to this routine.  This is a software coordinate rule; the static
evidence does not show a hardware hotspot facility.

### Enable, disable, and monochrome colors

`m9x31451` at RVA `0x2950c0` shows the current cursor and records visible state:

```text
mode 1 (monochrome): control = 3
mode 2 (color):      control = 5
```

`m9x19230` at RVA `0x295140` hides either format with `control = 0` and clears
the software-visible flag.

`m9x14050` at RVA `0x294f50` changes the two monochrome colors only in mode 1.
It blanks the cursor, selects palette slot 0 with `control=0`, writes color 0
to `base+0`, selects slot 1 with `control=0x100`, writes color 1 to `base+0`,
then restores `control=3` only if the cursor was visible.  The exact channel
packing of those two 32-bit color arguments is not established by this code.

### Surface allocation and image layout

`m9x2013` at RVA `0x295230` lazily allocates a VRAM surface and CPU aperture,
then retains its 32-bit device address.  Its size descriptor is:

| mode | descriptor words | resulting saved-driver geometry |
|---|---|---|
| 1, monochrome | `0x0100`, `0x0040` | 64 rows, 32 device bytes per row, `0x800` bytes |
| 2, color | `0x0800`, `0x0040` | 64 rows, 256 bytes per row, `0x4000` bytes |

The first value is consistent with row width in bits.  Both upload loops prove
the total byte counts independently.

The color upload (`m9x9134`, RVA `0x295460`) writes the VRAM device address to
`base+0x0c`, writes `0xcc000000` to `base+0x10`, and copies exactly `0x4000`
bytes.  It clears bit 24 in every source dword with mask `0xfeffffff`.  The
saved driver therefore intends a linear 64 x 64 x 32-bit image with a
256-byte row.  Static code alone does not determine the byte/channel order;
the cleared bit is bit 24 of each little-endian dword.

The monochrome upload (`m9x32274`, RVA `0x295560`) writes the address, writes
config `0x06000000`, then expands 64 source rows.  It copies 16 source bytes to
the first half of each 32-byte device row and zeros the second 16 bytes.  The
source is therefore `0x400` bytes and the device allocation `0x800` bytes.
The two 64-bit bitmap-plane meanings and their foreground/background polarity
remain unjoined.

There is a critical conflict with live evidence.  Earlier **[HW]** testing
accepted config `0xcc000000` and control `5`, but the linear color image did
not produce a correct cursor at any tested stride and its red/blue channels
appeared swapped.  Consequently the table above is an exact vendor software
contract, not proof that the silicon consumes the apparent linear layout.
Keep the DRM hardware cursor disabled by default and retain the software cursor
until the live tests below close the layout.

### Live bring-up, 2026-09-16

The `m9x` DRM driver now drives route 0 from a universal cursor plane, and the
register contract above is confirmed on the M9148 under Xorg/modesetting with
twm: control reads `5`, config `0xcc000000`, the address holds the cursor BO's
VRAM offset, and the position register tracks the pointer exactly - a warp to
`512,620` with the root cursor's `7,7` hotspot reads `0x023902a5`, that is
`(512-7+64) << 16 | (620-7+64)`.  The scanout no longer contains any pointer
image, so Xorg has stopped compositing one.

Three things had to change in the driver before Xorg would even attempt the
ioctl, and they are worth recording because none of them is cursor hardware:

* `drm_simple_display_pipe_init()` creates its CRTC with no cursor plane, so
  `crtc->cursor` stayed NULL and `drm_mode_cursor_common()` returned `-ENXIO`;
  modesetting then disables the hardware cursor for the rest of the session.
* `m9_fb_create()` and `mode_config.min_width/min_height` admitted only the
  exact 1024x768 scanout, so `MODE_CURSOR2` failed with `-EINVAL` when DRM
  wrapped the 64x64 ARGB image in a framebuffer.
* Relaxing those bounds must not stop at the cursor: modesetting decides its
  colour depth by addfb-testing a `min_width x min_height` buffer, so a driver
  that refuses small framebuffers falls back to 24bpp and loses glamor.

The apparent red/blue swap recorded above predates the gamma-LUT bypass fix
(`DC_CTRL` bit 14), which distorted every colour the controller emitted.  The
appearance of the overlay on the monitor is the one part of the contract a
capture cannot check, because the cursor is composited in the display stage.

### Update, destruction, and VT contract

The leaf uploads program address and config before copying pixels, and show is
a separate call.  They contain no vblank latch or explicit cache flush.  A
safe new-driver order is therefore:

1. keep control zero;
2. allocate a cursor BO in the hardware-addressable range and map it;
3. populate the complete image, flush CPU writes as required by the BO domain;
4. program address and config;
5. program position and monochrome palette if applicable;
6. enable with control 3 or 5.

When switching formats, disabling, or releasing a BO, write control zero
before changing address/config or freeing storage.  Saved-driver destructor
wrappers `m9x17694`/`m9x1468` at RVAs `0x2951f0`/`0x295210` explicitly hide
before releasing the surface.  The format-switch upload path itself assumes
its caller has already made the cursor safe, so it is not a sufficient model
for an atomic KMS implementation.

On VT leave/suspend, hide every initialized route before disabling its display
controller and unpinning cursor storage.  On VT enter/resume, restore image,
address/config, colors, and position only after native modeset state is valid;
enable last if the DRM cursor plane is visible.  These are the ordering rules
implied by the leaf contract.  No saved code proves an asynchronous latch or
an IRQ requirement for cursor updates.

## Primary scanout gamma LUT

### Register protocol and route map

The Linux `PhxColorLUT_Apply` implementation (`m9x21989`, RVA `0x2f7420`) and
the independent Windows implementation at RVA `0x7f710` agree exactly:

| route | controller control | LUT index | red | green | blue/latch |
|---:|---:|---:|---:|---:|---:|
| 0 | `0x1800` | `0x1838` | `0x183c` | `0x1840` | `0x1844` |
| 1 | `0x1a00` | `0x1a38` | `0x1a3c` | `0x1a40` | `0x1a44` |
| 2 | `0x1980` | `0x19b8` | `0x19bc` | `0x19c0` | `0x19c4` |
| 3 | `0x1b80` | `0x1bb8` | `0x1bbc` | `0x1bc0` | `0x1bc4` |

For the primary LUT, both binaries perform this sequence:

```c
control &= ~BIT(14);            /* leave LUT bypass */
mmio_write(control_reg, control);
mmio_write(index_reg, 0);
for (unsigned i = 0; i < 256; ++i) {
    mmio_write(red_reg,   red[i]   & 0xffc0);
    mmio_write(green_reg, green[i] & 0xffc0);
    mmio_write(blue_reg, (blue[i]  & 0xffc0) | BIT(31));
}
```

The arrays are 16-bit values.  Only their upper ten bits reach the LUT ports,
left aligned in bits 15:6.  The write to the blue port carries bit 31 on every
entry; because it follows red and green, this is the per-entry RGB latch or
advance strobe.  The index starts at zero and the loop count is exactly 256.
There is no LUT BO or DMA upload in this path.

The X-facing Linux wrapper `m9x19758` at RVA `0x2b0e50` rejects every size
other than 256 with the diagnostic `invalid gamma set size (should be 256)`.
The internal LUT object reserves `0x800` bytes per expanded channel and also
supports smaller custom/window-level tables via an index-remapping helper.
That private facility does not change the primary KMS gamma-ramp contract.

### Bypass, readiness, and identity

`PhxColorLUT_SetLutBypass` is `m9x799` at Linux RVA `0x2f77b0`; Windows has the
same operation at RVA `0x7ef50`.  Its Boolean argument directly controls
controller-control bit 14:

```text
bit 14 = 1: bypass the LUT
bit 14 = 0: use the LUT
```

Apply clears bit 14 before writing entries.  No separate LUT-enable or final
table-latch register is written, and neither implementation polls readiness in
the primary apply routine.  A separate interface named `IsLUTReady` exists,
but the concrete apply path does not call it after writes.  A new driver should
serialize LUT writes with the per-CRTC modeset lock and use the blue-port bit-31
strobe exactly as above; it must not invent an unproven vblank or IRQ wait.

The saved binaries expose two exact identity constructions:

* the ordinary 256-entry ramp is `entry[i] = i * 0x0101` (`M9WIN` RVA
  `0x7fa60`), including endpoints `0x0000` and `0xffff`;
* Phoenix's expanded 10-bit table generator emits 10-bit codes left shifted
  six.  Its 8-bit-input mode steps by four and forces the last entry to
  `0xffc0`; 5/6/10-bit modes step by 32/16/1 respectively (`M9LIN`
  `m9x19177`, RVA `0x2f72e0`; `M9WIN` RVA `0x7fb40`).

For DRM, use the standard 256-entry 16-bit identity `i * 257`; the apply mask
then preserves the upper ten bits and gives the final entry `0xffc0`.
Initialize every active CRTC to this ramp with bit 14 clear, or set bit 14 for
an explicit bypass identity.  Save/restore the software ramp across suspend
and VT transitions; hardware register contents cannot be assumed to survive.

### Render pixels, GigaColor, and GL gamma

This LUT is scanout-only.  The recovered code writes display-controller MMIO
ports in the `0x18xx..0x1bxx` range and never touches a render target, texture,
3D command packet, or BO contents.  A VRAM/readback snapshot therefore contains
the uncorrected render pixels.  A camera or capture device downstream of the
connector sees the LUT-corrected signal.

The 10-bit LUT values do not by themselves prove a 10-bit framebuffer path.
GigaColor render/texture formats and depth-30 scanout selection remain separate
contracts.  The LUT can output ten-bit amplitudes, but static code does not
show whether a depth-30 scanout indexes all ten input bits, uses the upper eight,
or combines LUT output with dithering.

There is no per-window LUT in the recovered display block.  A gamma ramp loaded
for a windowed GL client affects the entire routed CRTC, including other X
windows, while leaving framebuffer memory unchanged.  A fullscreen client may
temporarily install a per-CRTC ramp and restore it on exit/VT switch.  If a GL
API promises window-local gamma, implement that in rendering/compositing; this
hardware scanout LUT cannot isolate one window.  These policy statements follow
from the per-controller register scope; the saved leaf code does not identify
which GLX extension initiated a load.

## Implementation status and live gates

The gamma programming contract is sufficient to implement a 256-entry DRM
legacy gamma LUT per exposed CRTC, with 10-bit channel precision and exact
bypass/load order.  Cursor register routing, coordinates, controls, allocation
sizes, and the saved upload transformations are also statically closed.

Hardware cursor enable remains gated by these live tests:

1. capture MMIO and VRAM for the vendor Linux cursor on a known M9 mode;
2. determine color byte order and the actual fetch swizzle/stride that resolves
   the existing linear-layout contradiction;
3. determine the two monochrome plane meanings and color dword packing;
4. verify address alignment/addressable range and movement at all four edges;
5. verify whether address/config/image updates require display blanking or an
   explicit cache flush.

Gamma enable should first be live-tested with red-, green-, and blue-only ramps,
then identity and nonlinear ramps on both physical heads.  Depth-30/GigaColor
needs a connector capture or DAC measurement plus a 10-bit gradient to establish
input indexing and dithering.  Until those tests pass, keep depth-30 interaction
out of the advertised gamma capability; ordinary 8-bit scanout gamma has a
complete static programming contract.
