# M9148 preprocessing transient arena

The four ordinary first-triangle transient addresses are subviews of one
context resource.  The exact ordinary payload is `0x5a000` bytes, divided into
four contiguous `0x16800`-byte views.  The preprocessing pass writes the views
and the raster pass reads them later in the same job.  No opaque initial image
is required.

Reproduce the binary and capture evidence with:

```sh
python3 tools/m9-transient-arena-evidence.py --self-test
```

Audit the clean-room kernel representation with:

```sh
python3 tools/m9-transient-arena-evidence.py --self-test \
  --kernel ../src.linux.x86-64-desktop.260909.123559.3725172/linux-7.2/drivers/gpu/drm/m9x/m9_triangle.c
```

The second form requires one zero-filled, 4-KiB-aligned arena payload, four
derived addresses, and one trailing guard.  It rejects the earlier four-BO
model and its `0x5452000N` per-view payload sentinels.

## Pinned inputs

| input | SHA-256 |
| --- | --- |
| Windows `MSerX/MxMD.dll` | `b6905b6e658b87544bc0081d080190e4f8112b723c0b2fefd781ba5766874f43` |
| ordinary `bm-12.bin` | `abb4ac761fd529754a6bb62ad0e8e25013967ec8c1f375ed65955991a8f0d05c` |
| ordinary `execute-12.bin` | `f009dbf9ec3331e5cc73ed0172ac2356a17268009484d81b1cb2c09e84a08dfa` |
| five-stream point `case00-bm.bin` | `9b5e7ecc72675364fca9e14a0a5cf1cd8825cef42f9c27a693aba450ec297812` |
| five-stream point `case00-execute.bin` | `f9d6844fad10517f29de4ad4bfb940629968e7c58a987e720c32e86ab0e5ace6` |
| five-stream textured `gltri-bm-16.bin` | `9662cb25b143d919a72e9122135bc77632141b4e74eaceaaef9e123538b47164` |
| five-stream textured `gltri-execute-16.bin` | `19824e04fa1aaeb8e316695a0e33923bf562a629cced014a2621d2823732c5ff` |
| related M0528 MMIO trace | `8fa1b88deda8cedd8a747c17669e8ad6ace7c408d754a0687e4165321b9aabd6` |

The verifier also pins the complete instruction slices used below.  It reads
the vendor inputs without modifying them.

## One owner and one allocation

Windows context-resource startup at `MxMD.dll:0x3e7e0` allocates one wrapper
object and makes one call to `0x3ea50` with these arguments:

```text
out pointer
class = 1
size  = 0x5a000
```

The resulting single pointer is stored at device `+0x360c`, then copied to
context `+0xc4`.  Factory `0x3ea50` constructs one resource and makes one
class-1 call to the common resource allocator at `0x487d0`.  Its dimensions
are the requested size and height one.  Teardown at `0x3e950` calls the one
owner's release path, clears device `+0x360c`, and clears context `+0xc4`.

This is one context-owned backing resource, not four independently owned BOs.
The requested logical extent is exactly `0x5a000`.  It is already a multiple
of 4 KiB, so a page-granular clean-room allocator needs exactly `0x5a000`
payload bytes before a diagnostic guard.

The factory initializes an internal class-1 cursor to `0x100`.  That is object
allocator state and is not used here as a hardware-address alignment claim.
The command captures provide the address layout directly.

## Exact ordinary layout and roles

The settled ordinary triangle has these addresses:

| view | GPU address | arena offset | role across the job |
| ---: | ---: | ---: | --- |
| 0 | `0x113c1000` | `0x00000` | preprocess `b0` write; raster `00` read |
| 1 | `0x113d7800` | `0x16800` | preprocess `b2` write; raster `03` read |
| 2 | `0x113ee000` | `0x2d000` | preprocess `b4` write; raster `01` read |
| 3 | `0x11404800` | `0x43800` | preprocess `b8` write; raster `02` read |

The producer-to-consumer permutation is therefore:

```text
preprocess [b0,b4,b8,b2] -> raster [00,01,02,03]
```

All adjacent addresses differ by `0x16800`, and:

```text
4 * 0x16800 = 0x5a000
```

The four ordinary views exhaust the single allocation exactly.  There is no
padding or guard between views.  The arena base is observed on a 4-KiB
boundary; every view is on a `0x800` boundary.  A clean-room implementation
should retain the observed 4-KiB arena alignment and derive every address as
`base + i * 0x16800`.

The resource must remain writable during preprocessing and readable during
raster.  Its lifetime spans both passes and their intervening transition.  It
is internal local storage rather than CPU-provided vertex input.

## Dynamic repartition corroboration

Two independent captures with five raster inputs use the same address set:

```text
0x113c1000
0x113d3400  (+0x12400)
0x113e5800  (+0x24800)
0x113f7c00  (+0x36c00)
0x1140a000  (+0x49000)
```

Each address first appears as a preprocessing output and later as a raster
input.  Both captures start at the ordinary arena base but repartition the
interior at `0x12400` steps.  These are base offsets, not proof of five equal
extents: `5 * 0x12400 = 0x5b400`, which exceeds the `0x5a000` owner.  The
five-stream evidence is used only to prove that the context resource is a
shared arena whose view layout can change with the output topology.

The five-stream addresses are all `0x400` aligned.  The ordinary path's exact
replay layout has the stronger `0x800` view alignment stated above.

## Initial contents

The ordinary preprocessing launch is `[0,4)` and binds all four arena views as
outputs.  The following raster launch is `[0,3)` and binds the same four views
as inputs.  Every record consumed by raster therefore has a producer earlier
in the same job.  Bytes outside the produced records are not command input.
No vendor payload blob, magic sentinel, or preserved data from an earlier job
is part of the semantic contract.

Zero initialization remains the closest reproducible allocator policy.  The
related M0528 MMIO trace contains one continuous `rep stosq` clear over the
entire aperture interval corresponding to command-address range
`0x113c1000..0x1141b000`.  The verifier checks all 2,880 qword stores from
`0xd113c100` through `0xd1141af8`; every payload is zero.  This related-family
observation corroborates zeroing but is not needed for correctness because
M9148 preprocessing defines the consumed records.

Use a full zero fill for deterministic unwritten bytes and to avoid exposing
reused VRAM.  A per-view sentinel fill is not vendor-exact.  It should never
be interpreted as four independent guard boundaries.

## Kernel policy comparison

The earlier diagnostic allocated four independent page-rounded BOs.  Each had
`0x16800` payload bytes, a `0x1800` tail, and a distinct `0x5452000N` payload
fill.  That policy reserved `4 * 0x18000 = 0x60000` bytes and allowed unrelated
physical gaps between addresses.

| property | vendor contract | earlier diagnostic | corrected representation |
| --- | --- | --- | --- |
| owner/allocation count | one | four | one |
| payload | `0x5a000` | four times `0x16800` | `0x5a000` |
| view addresses | one base plus fixed offsets | four BO bases | one base plus fixed offsets |
| inter-view guards | none | one after every view | none |
| initial fill | no semantic dependency; zero corroborated | four sentinels | zero |
| tail guard | outside vendor payload | four `0x1800` tails | one `0x1000` tail |
| total diagnostic allocation | `0x5a000` plus any external guard | `0x60000` | `0x5b000` |

The old structure was a P1 bring-up mismatch.  It happened to provide four
usable addresses for the ordinary packet, but it did not reproduce the one
owner or its contiguous layout.  It could also classify legal cross-view
access or prefetch as an overflow and could not support the observed dynamic
five-stream repartition.

The corrected kernel audit requires:

* one resource ID and one spec for `M9_TRI_TRANSIENT_ARENA`;
* a zero-filled `0x5a000` payload aligned to 4 KiB;
* `job->transient[i] = arena_bus + i * 0x16800`;
* a compile-time `0x5a000` assertion;
* one page-rounded trailing guard, producing `0x5b000` total bytes;
* absence of the former per-view resource IDs and `0x5452000N` fills.

This closes the ordinary first-triangle extent, layout, alignment, ownership,
initial-content rule, write/read direction, and full-job lifetime.  The exact
last-view extent in a five-stream topology remains deliberately unspecified;
it is not needed by the ordinary four-view diagnostic.

<!-- Copyright 2026 Rene Rebe <rene@exactco.de> -->
<!-- SPDX-License-Identifier: GPL-2.0 -->
