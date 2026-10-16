# M9 Linux vendor GLX recovery and command capture

## Working configuration

The saved Matrox M9 package works on the M9148 LP (`102b:0540`) test host at
`192.168.1.56`.  The host runs Linux `2.6.30-dist`, Xorg `1.5.2`, the vendor
`m9x` kernel module, and `m9x_drv.so`.

The original installation mixed the Xorg 1.5 server and DDX with the package's
Xorg 1.15 GLX libraries.  The server therefore rejected `libglx.so` with:

```
libglx.so: undefined symbol: RegionEmptyData
```

This is an ABI-version mismatch, not a missing M9 kernel or EXA facility.  The
installed server exports the older `miEmptyData` region symbol.  The package's
`xserver/7_4_0` build is the matching GLX build for Xorg 1.5; its server module
uses that symbol.

The repaired pair is:

| Component | Package source | Installed SHA-256 |
| --- | --- | --- |
| server GLX | `xserver/7_4_0/libglx.so` | `fcbac1092c5604db7090e24154b4e6f1712e957f520513ee30f2c2c86a68352e` |
| client GL | `xserver/7_4_0/lib64/libGL.so.2.0.0` | `7dae6178e5263e322617b8755eb894c4cfa17513704773b0ca15b82f884c08e0` |

They replace, respectively:

- `xserver/1_15_0/libglx.so`, saved as
  `/tmp/libglx.so.m9x-wrong-1_15.backup` on the test host.
- `xserver/1_15_0/lib64/libGL.so.2.0.0`, saved as
  `/tmp/libGL.so.2.0.0.m9x-wrong-1_15.backup`.

After restarting X, the server log reports:

```
(II) M9XGLX: initialized
(II) GLX: Initialized M9XGLX GL provider for screen 0
```

`glxinfo` reports direct rendering, `Matrox` as the server GLX vendor,
`Matrox ICD for M-Series` as the renderer, OpenGL 2.0, and GLSL 1.10.  A
September 14, 2026 regression run with vblank disabled produced:

```
14696 frames in 5.0 seconds = 2939.059 FPS
```

The exact test was:

```sh
DISPLAY=:0 XAUTHORITY=/root/.serverauth.5119 \
vblank_mode=0 timeout 7 stdbuf -oL /usr/X11/bin/glxgears
```

The Xauthority filename changes each time `startx` runs and must be read from
the live X command line or the corresponding `xinit` process.

## Why the matching libraries matter

Replacing only server `libglx.so` makes the provider initialize, but the 1.15
client `libGL.so.2.0.0` then fails against Xorg 1.5 with an unresolved
`_XGetRequest`.  Server GLX and client GL must therefore come from the same
vendor `xserver/7_4_0` directory.

A compatibility preload defining `RegionEmptyData` and `RegionEmptyBox` can be
useful for static experiments with newer vendor modules.  It is not the clean
fix for this host because the package already contains the ABI-matched build.

## What can be captured

EXA and direct GLX use the same requester and bus-master transport.  EXA solid,
copy, and composite operations are enough to recover requester setup, indexed
packet framing, relocations, submission, fences, and idle handling.  EXA does
not exercise 3D draw launch, vertex declarations, depth/raster state, texture
state, or native shader uploads.  The working direct GLX client is therefore
the preferred capture source for the Mesa backend.

The direct-rendering process opens `/dev/m9x0`.  Its relevant 64-bit private
ioctls are:

| Command | Request | Purpose |
| ---: | ---: | --- |
| 1 | `0xc0384601` | `GetBmBufferAddr` |
| 12 | `0xc840460c` | `ExecuteDma` |
| 13 | `0xc018460d` | `DrawOnSyncBuffer` |
| 14 | `0xc010460e` | restart DMA front end |
| 15 | `0xc010460f` | clear DMA front-end state |
| 16 | `0xc0104610` | `SetCommandBufferSize` |
| 18 | `0xc0304612` | `ReleaseBmBuffer` |

`ExecuteDma` copies a fixed `0x840`-byte image into the kernel.  Its inline
`0x808`-byte body describes the submission, while the indexed packet bytes are
in 32 KiB bus-master mappings.  On the observed 64-bit path, the command 1
result contains:

| Offset | Meaning |
| ---: | --- |
| `+0x10` | mmap offset cookie |
| `+0x18` | second address or handle |
| `+0x20` | type |
| `+0x24` | mapping byte size |
| `+0x28`, `+0x30` | remaining allocation metadata |

The ioctl header and range descriptor layout is:

| Ioctl offset | Meaning |
| ---: | --- |
| `+0x00` | qword structure size, `0x840` |
| `+0x08` | caller argument, observed as zero |
| `+0x0c` | total extent |
| `+0x10` | descriptor count `N` |
| `+0x14` | padding or unused data |
| `+0x18 + 0x10*i` | `start`, `end`, `flag0`, `flag1` for entry `i` |
| `+0x818` | output mmap cookie for the next command buffer |
| `+0x820` | output kernel address for that buffer |
| `+0x828` | output type/status word |
| `+0x82c` | output mapping size, observed as `0x8000` |

Static recovery of the vendor conversion routine proves that `start` and
`end` are the low 24 bits of its source words, while `flag0` and `flag1` come
from source bits 56 and 57.  Each entry selects the end-exclusive byte range
`[bm + 8 + start, bm + 8 + end)`.  The qword at bus-master offset zero is a
private prefix and is outside the indexed packet stream.  The total extent is
eight plus the last valid descriptor end.  Words after descriptor `N` are
uninitialized data and must not be decoded.

Captured GLX submissions contain valid M9 counted raw blocks and numeric
draw-range records.  An ordinary outer record is `u32 count` followed by
`count + 1` raw dwords.  That raw block may contain one or several indexed
packets.  Draw packets remain uncounted raw three-dword records.  The steady
glxgears cycle repeats eight submissions with total-extent values
`0x1010`, `0x4f0`, `0x5e0`, `0x4f0`, `0x5e0`, `0x4f0`, `0x8cc`, and `0x8cc`.
The `0x1010` submission contains two five-draw ranges plus a completion range.
The vendor ping-pongs between command buffers: submission zero uses the mapping
from command 1, then ExecuteDma returns the mmap cookie and size of the next
buffer.  The current tracer joins that output to the following mmap and records
all active command mappings.  The checked-in glxgears fixture has been reduced
to the active A or B mapping for each submission.

Two raw fixture sets are checked in:

- `captures/m9x-linux-glxgears-20260914` contains one steady eight-submission
  glxgears cycle.
- `captures/m9x-linux-gltri-20260914` contains the complete startup and one
  settled flat-triangle submission from `tools/gltri.c`.  Its sequence 12 has
  numeric draw ranges at bus-master offsets `0x264` and `0x4b8`; the second has
  the three-element range `[0,3)` matching the submitted triangle.

## Capture tool

Build [m9x-exec-trace.c](../tools/m9x-exec-trace.c) on the vendor host:

```sh
gcc -Wall -Wextra -Werror -std=gnu99 -O2 -fPIC -shared \
    -o /tmp/m9x-exec-trace.so /tmp/m9x-exec-trace.c -ldl
```

Run one bounded direct-rendering capture:

```sh
mkdir /tmp/m9x-cap
DISPLAY=:0 XAUTHORITY=/root/.serverauth.CURRENT \
M9XTRACE_DIR=/tmp/m9x-cap M9XTRACE_LIMIT=32 \
LD_PRELOAD=/tmp/m9x-exec-trace.so \
timeout 1 /usr/X11/bin/glxgears
```

The tool records device opens, private ioctl numbers, mappings, the pre/post
ExecuteDma images, and every 32 KiB command mapping before and after each
bounded submission.  `M9XTRACE_PTRS=1` additionally probes plausible pointers
from the inline image with safe kernel-validated writes; this is noisy and is
normally unnecessary after the bus-master mapping join.  Limit pointer dumps
with `M9XTRACE_PTR_BYTES`.

Preloading the tracer into Xorg as well records EXA submissions.  For 3D state,
preload it into the direct GL application because the direct client owns the
`/dev/m9x0` submission stream.

Vendor-framed captures can be decoded directly with:

```sh
python3 tools/m9-command-decode.py --execute execute.bin bm.bin
```

For controlled differentials, keep one X server and one GL context alive and
capture one operation at a time: no-op baseline, clear, untextured triangle,
then individual depth, blend, raster, texture, and shader-state changes.  This
avoids startup allocation and program-cache traffic obscuring the changed
packets.

## Kernel fallback

If a user-space differential cannot relate a descriptor to its mapped bytes,
the direct kernel observation point is the call to `dev->core->ioctl(...)` in
`kernel/src/m9x_fops.c`, while `dev->struct_mutex` is held.  Gate capture on
`_IOC_NR(cmd) == 12` and correlate tagged addresses with the driver's memory
regions (`busaddr`, `kaddr`, and `real_size`).  Use a one-shot buffer rather
than printing every submission: glxgears submits tens of thousands of command
12 ioctls per second.

Kernel `mmiotrace` is useful for requester MMIO, but it must be selected before
the first `/dev/m9x0` open that performs the register `ioremap`.  It cannot show
the system-memory command contents by itself.  The user-space capture is the
lower-risk first pass and requires no replacement kernel module.
